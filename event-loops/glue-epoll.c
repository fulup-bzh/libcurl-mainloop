/*
 * Copyright (C) 2021 "IoT.bzh"
 * Author "Fulup Ar Foll" <fulup@iot.bzh>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT. $RP_END_LICENSE$
 *
 *
 *  * Note-1:
 *    epoll cannot attach a specific callback to a given FD.
 *    To bypass this limit this glue creates one sub-pool per callback.
 *    Each sub-pool as a wellknown FD store into epollEvtLoopT
 *    when a given sub-pool FD is triggered from main event loop (mainPool)
 *    it calls a nested epoll_wait for the corresponding sub-pool FD and
 *    then for each waiting socket/timer within sub-pool. Finally it calls
 *    corresping libhttp/curl callback with adequate sockfd and curl action.
 *
 *  Note-2: (thanks to Henrik.H for the remark.)
 *    If you fully control your epoll mainloop you should replace nested epoll
 *    with a common handle that describe your FD context within event.data.prt.
 *    Malloc something like following structure and keep track of it (event.data.ptr).
 *    'enums' is probably the simple option if you have few callbacks. Nevertheless if you
 *    have many callbacks, storing a function pointer with a context is more
 *    consistant with traditional mainloop callback approach.
 *      struct whatever {
 *        enum { TYPE_ALIEN, TYPE_CURL, TYPE_TIMERFD } type;
 *        void* callback;
 *        void* context;
 *        httpPoolT *pool;
 *        int fd;
 *        ...
 *     }
 *
 *  Note-3:
 *    Mallocing 'struct epoll_event' before 'epoll_ctl' is useless as epoll copy structure contend.
 *    I left this extra malloc because I'm lazy and it keeps epoll-curl-glue more consistant with
 *    other mainloop models.
 *
 */

#define _GNU_SOURCE

#include "../http-client.h"

#include <assert.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define FLAGS_SET(v, flags) ((~(v) & (flags)) == 0)

#define EPOLL_EVENTS_MAX 10
#include <sys/epoll.h>
#include <sys/timerfd.h>

// epoll does not allow to attach callback we create one pool for class of events
typedef struct {
    int socksPool;
    int timerPool;
    int mainPool;
} epollEvtLoopT;

//  (void *source, int sock, uint32_t revents, void *ctx)
static int glueOnSocketCB(httpPoolT *httpPool, int sock, uint32_t revents) {
    int action;

    // translate systemd event into curl event
    if (FLAGS_SET(revents, EPOLLIN | EPOLLOUT))
        action = CURL_POLL_INOUT;
    else if (revents & EPOLLIN)
        action = CURL_POLL_IN;
    else if (revents & EPOLLOUT)
        action = CURL_POLL_OUT;
    else
        action = 0;

    int status = httpOnSocketCB(httpPool, sock, action);
    return status;
}

// create systemd source event and attach http processing callback to sock fd
static int glueSetSocketCB(httpPoolT *httpPool, CURL *easy, int sock, int action, void *sockp) {
    struct epoll_event *source=  (struct epoll_event *)sockp; // on 1st call source is null
    epollEvtLoopT *evtLoop= (epollEvtLoopT*)httpPool->evtLoop;
    uint32_t events;
    int err;

    // map CURL events with system events
    switch (action) {
    case CURL_POLL_REMOVE:
        epoll_ctl(evtLoop->socksPool, EPOLL_CTL_DEL, sock, NULL);
        free(source);
        return 0;
    case CURL_POLL_IN:
        events = EPOLLIN;
        break;
    case CURL_POLL_OUT:
        events = EPOLLOUT;
        break;
    case CURL_POLL_INOUT:
        events = EPOLLIN | EPOLLOUT;
        break;
  default:
        goto OnErrorExit;
  }

  // at initial call source does not exist, we create a new one and add it to
  if (!source) {

    // create epool source event handle and use glue event within userdata
    struct epoll_event *source = malloc(sizeof(struct epoll_event));
    source->events = events;
    source->data.fd = sock; // make sure we may extract this from other FD

    // attach new event source and attach it to systemd mainloop
    err = epoll_ctl(evtLoop->socksPool, EPOLL_CTL_ADD, sock, source);
    if (err < 0) goto OnErrorExit;

    // insert new source to socket userData on 2nd call it will comeback as
    // sockp
    err = curl_multi_assign(httpPool->multi, sock, source);
    if (err != CURLM_OK)  goto OnErrorExit;

    } else {

        source->events = events;
        err = epoll_ctl(evtLoop->socksPool, EPOLL_CTL_MOD, sock, source);
        if (err < 0) goto OnErrorExit;
  }
  return 0;

OnErrorExit:
  return -1;
}

// arm a one shot timer in ms
static int glueSetTimerCB(httpPoolT *httpPool, long timeout) {
    int err;
    struct epoll_event *source = (struct epoll_event *)httpPool->evtTimer;
    epollEvtLoopT *evtLoop= (epollEvtLoopT*)httpPool->evtLoop;

    // if time is negative just kill it
    if (timeout < 0) {
        if (source->data.fd) close(source->data.fd);
        free (source);
    } else {

        // not timer yet, create one and add it to timerpool fd list
        if (!source) {
            // create epoll event and register timer within epoll loop
            source = malloc(sizeof(struct epoll_event));
            source->events = EPOLLIN;

            source->data.fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
            if (source->data.fd < 0) goto OnErrorExit;

            // attach new event source and attach it to systemd mainloop
            err = epoll_ctl(evtLoop->timerPool, EPOLL_CTL_ADD, source->data.fd, source);
            if (err < 0) {
                goto OnErrorExit;
            }

            httpPool->evtTimer = (void *)source;
        }

        // set/update timer for one shot only (tick only once tv_sec+tv_nsec=0)
        if (!timeout) timeout=1;
        struct itimerspec delay;
        delay.it_interval.tv_sec= 0;
        delay.it_interval.tv_nsec = 0;
        delay.it_value.tv_sec = timeout / 1000;
        delay.it_value.tv_nsec = (timeout % 1000) * 1000000;
        err = timerfd_settime(source->data.fd, 0, &delay, NULL);
        if (err)
        {
            goto OnErrorExit;
        }
    }
    return 0;

OnErrorExit:
    fprintf(stderr, "Hoops timerfd exited error=%s\n", strerror(errno));
    return -1;
}

// run mainloop and wait for asynchronous events
static int processWaitingSockets (httpPoolT *httpPool, long seconds) {
    epollEvtLoopT *evtPool = (epollEvtLoopT*)httpPool->evtLoop;
    struct epoll_event events[EPOLL_EVENTS_MAX];
    int err;

    int count = epoll_wait(evtPool->socksPool, events, EPOLL_EVENTS_MAX, 0);
    if (count < 0) goto OnErrorExit;

    // this will trigger curl and non curl socket
    for (int idx = 0; idx < count; idx++) {
        err= glueOnSocketCB (httpPool, events[idx].data.fd, events[idx].events);
        if (err) goto OnErrorExit;
    }

    return 0;

OnErrorExit:
    fprintf(stderr, "[processWaitingSockets] Hoops epoll exited error=%s\n", strerror(errno));
    return -1;
}

// run mainloop and wait for asynchronous events
static int processWaitingTimers (httpPoolT *httpPool, long seconds) {
    epollEvtLoopT *evtPool = (epollEvtLoopT*)httpPool->evtLoop;
    struct epoll_event events[EPOLL_EVENTS_MAX];
    int err;

    int count = epoll_wait(evtPool->timerPool, events, EPOLL_EVENTS_MAX, 0);
    if (count < 0) goto OnErrorExit;

    // this will trigger curl and non curl socket
    for (int idx = 0; idx < count; idx++) {
        err= httpOnTimerCB(httpPool);
        if (err < 0) goto OnErrorExit;
    }
    return 0;

OnErrorExit:
    fprintf(stderr, "[processWaitingTimers] Hoops epoll exited error=%s\n", strerror(errno));
    return -1;
}


// run mainloop and wait for asynchronous events
static int glueRunLoop(httpPoolT *httpPool, long seconds) {
    struct epoll_event events[EPOLL_EVENTS_MAX];
    epollEvtLoopT *evtPool = (epollEvtLoopT*)httpPool->evtLoop;
    int err;

    int count = epoll_wait(evtPool->mainPool, events, EPOLL_EVENTS_MAX, seconds * 1000);
    if (count < 0) goto OnErrorExit;

    // this will trigger curl and non curl socket
    for (int idx = 0; idx < count; idx++) {

        // curl socket ready/waiting
        if (events[idx].data.fd == evtPool->socksPool) {
            err= processWaitingSockets (httpPool, seconds);
            if (err) goto OnErrorExit;
            continue;
        }

        // curl timer ready/waiting
        if (events[idx].data.fd == evtPool->timerPool) {
            err= processWaitingTimers (httpPool, seconds);
            if (err) goto OnErrorExit;
            continue;
        }
        fprintf (stderr, "[glueRunLoop]  FD is not a libcurl handle fd=%d\n", events[idx].data.fd);
  }
  return 0;

OnErrorExit:
  fprintf(stderr, "[glueRunLoop] Hoops epoll exited error=%s\n", strerror(errno));
  return -1;
}

// create a new systemd event loop (in real world main is imported from application)
static void *gluenewEventLoop() {
    int err;
    struct epoll_event source;
    source.events= EPOLLIN | EPOLLOUT;

    // epoll is pretty basic, we create a subpool per callback (timer+socket)
    epollEvtLoopT *evtPool= malloc (sizeof(evtPool));

    // NOTE: main eventloop handle is probably already created by your application
    evtPool->mainPool = epoll_create1(EPOLL_CLOEXEC);
    if (evtPool->mainPool < 0)  goto OnErrorExit;

    // create a pool per callback, poolFD will allow us to select the callback to trigger
    evtPool->socksPool = epoll_create1(EPOLL_CLOEXEC);
    if (evtPool->socksPool < 0)  goto OnErrorExit;
    source.data.fd= evtPool->socksPool;
    err = epoll_ctl(evtPool->mainPool, EPOLL_CTL_ADD, evtPool->socksPool, &source);
    if (err <0) goto OnErrorExit;

    evtPool->timerPool = epoll_create1(EPOLL_CLOEXEC);
    if (evtPool->timerPool < 0)  goto OnErrorExit;
    source.data.fd= evtPool->timerPool;
    err = epoll_ctl(evtPool->mainPool, EPOLL_CTL_ADD, evtPool->timerPool, &source);
    if (err <0) goto OnErrorExit;

  return (void *)evtPool;

OnErrorExit:
  fprintf(stderr, "fail to create evtLoop error=%s\n", strerror(errno));
  return NULL;
}

static httpCallbacksT systemdCbs = {
    .multiTimer = glueSetTimerCB,
    .multiSocket = glueSetSocketCB,
    .evtMainLoop = gluenewEventLoop,
    .evtRunLoop = glueRunLoop,
};

httpCallbacksT *glueGetCbs() { return &systemdCbs; }