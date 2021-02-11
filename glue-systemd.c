/*
 * Copyright (C) 2021 "IoT.bzh"
 * Author "Fulup Ar Foll" <fulup@iot.bzh>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at https://opensource.org/licenses/MIT.
 * $RP_END_LICENSE$
 */

#define _GNU_SOURCE

#include "http-client.h"

#include <errno.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>

#include <systemd/sd-event.h>

#define FLAGS_SET(v, flags) ((~(v) & (flags)) == 0)

//  (void *source, int sock, uint32_t revents, void *ctx)
static int glueOnSocketCB (sd_event_source *source, int sock, uint32_t revents, void *ctx)
{
    httpPoolT *httpPool= (httpPoolT*)ctx;
    int action;

   // translate systemd event into curl event
    if (FLAGS_SET(revents, EPOLLIN | EPOLLOUT)) action= CURL_POLL_INOUT;
    else if (revents & EPOLLIN)  action= CURL_POLL_IN;
    else if (revents & EPOLLOUT) action= CURL_POLL_OUT;
    else action= 0;

    int status=httpOnSocketCB(httpPool, sock, action);
    return status;
}


// create systemd source event and attach http processing callback to sock fd
static int glueSetSocketCB (httpPoolT *httpPool, CURL *easy, int sock, int action, void *sockp)
{
    sd_event_source *source = (sd_event_source *)sockp; // on 1st call source is null
    sd_event *evtLoop = (sd_event *)httpPool->evtLoop;
    uint32_t events;
    int err;

    // map CURL events with system events
    switch (action)
    {
    case CURL_POLL_REMOVE:
        sd_event_source_set_enabled(source, SD_EVENT_OFF);
        sd_event_source_unref(source);
        goto OnErrorExit;

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

    // at initial call source does not exist, we create a new one and add it to sock userData
    if (!source)
    {
        // attach new event source and attach it to systemd mainloop
        err = sd_event_add_io(evtLoop, &source, sock, events, glueOnSocketCB, httpPool);
        if (err < 0)
            goto OnErrorExit;

        // insert new source to socket userData on 2nd call it will comeback as sockp
        err = curl_multi_assign(httpPool->multi, sock, source);
        if (err != CURLM_OK)
            goto OnErrorExit;
    }

    err = sd_event_source_set_io_events(source, events);
    if (err < 0)
        goto OnErrorExit;

    err = sd_event_source_set_enabled(source, SD_EVENT_ON);
    if (err < 0)
        goto OnErrorExit;

    return 0;

OnErrorExit:
    return -1;
}

// map libuv ontimer with multi version
static int glueOnTimerCB(sd_event_source *timer, uint64_t usec, void *ctx)
{
    httpPoolT *httpPool = (httpPoolT *)ctx;
    int status= httpOnTimerCB(httpPool);
    return status;
}

// arm a one shot timer in ms
static int glueSetTimerCB(httpPoolT *httpPool, long timeout)
{
    int err;
    sd_event_source *evtTimer = (sd_event_source *)httpPool->evtTimer;
    sd_event *evtLoop = (sd_event *)httpPool->evtLoop;

    // if time is negative just kill it
    if (timeout < 0)
    {
        if (httpPool->evtTimer)
        {
            err = sd_event_source_set_enabled(httpPool->evtTimer, SD_EVENT_OFF);
            if (err < 0)
                goto OnErrorExit;
        }
    }
    else
    {
        uint64_t usec;
        sd_event_now(httpPool->evtLoop, CLOCK_MONOTONIC, &usec);
        if (!httpPool->evtTimer)
        { // new timer
            sd_event_add_time(evtLoop, &evtTimer, CLOCK_MONOTONIC, usec + timeout * 1000, 0, glueOnTimerCB, httpPool);
            sd_event_source_set_description(evtTimer, "curl-timer");
        }
        else
        {
            sd_event_source_set_time(evtTimer, usec + timeout * 1000);
            sd_event_source_set_enabled(evtTimer, SD_EVENT_ONESHOT);
        }
    }
    return 0;

OnErrorExit:
    return -1;
}

// run mainloop
static int glueRunLoop(httpPoolT *httpPool, long seconds)
{
    int status = sd_event_run(httpPool->evtLoop, seconds * 1000000);
    return status;
}

// create a new systemd event loop
static void *gluenewEventLoop()
{
    sd_event *evtLoop;
    int err = sd_event_new(&evtLoop);
    if (err)
        goto OnErrorExit;
    return (void *)evtLoop;

OnErrorExit:
    fprintf(stderr, "fail to create evtLoop\n");
    return NULL;
}

static httpCallbacksT systemdCbs = {
    .multiTimer = glueSetTimerCB,
    .multiSocket = glueSetSocketCB,
    .evtMainLoop = gluenewEventLoop,
    .evtRunLoop = glueRunLoop,
};

httpCallbacksT *glueGetCbs()
{
    return &systemdCbs;
}