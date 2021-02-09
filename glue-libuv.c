/*
 * Copyright (C) 2021 "IoT.bzh"
 * Author "Fulup Ar Foll" <fulup@iot.bzh>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at https://opensource.org/licenses/MIT.
 * $RP_END_LICENSE$
 */

#define _GNU_SOURCE

#include "curl-http.h"

#include <errno.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>

#include <uv.h>

#define FLAGS_SET(v, flags) ((~(v) & (flags)) == 0)

typedef struct
{
    httpPoolT *httpPool;
    int sock;
} libuvRqtCtxT;

//  (void *source, int sock, uint32_t revents, void *ctx)
static void glueOnSocketCB(uv_poll_t *evtSocket, int status, int events)
{
    libuvRqtCtxT *ctx = evtSocket->data;

    // translate systemd event into curl event
    int action = 0;
    uint32_t revents;
    if (revents & UV_READABLE)
        action = CURL_POLL_IN;
    else if (revents & UV_READABLE)
        action = CURL_POLL_OUT;
    else if (revents & UV_DISCONNECT)
    {
        action = CURL_POLL_REMOVE;
        free(ctx);
    }

    (void)httpOnSocketCB(ctx->httpPool, ctx->sock, action);
    return;
}

// create a source event and attach http processing callback to sock fd
static int glueSetSocketCB(httpPoolT *httpPool, CURL *easy, int sock, int action, void *sockp)
{
    uv_poll_t *source = (uv_poll_t *)sockp; // on 1st call source is null
    uv_loop_t *evtLoop = (uv_loop_t *)httpPool->evtLoop;
    uint32_t events;
    int err;

    // map CURL events with system events
    switch (action)
    {
    case CURL_POLL_REMOVE:
        uv_poll_stop(source);
        free(source->data);
        goto OnErrorExit;

    case CURL_POLL_IN:
        events = UV_READABLE;
        break;
    case CURL_POLL_OUT:
        events = UV_WRITABLE;
        break;
    case CURL_POLL_INOUT:
        events = UV_WRITABLE | UV_READABLE;
        break;
    default:
        goto OnErrorExit;
    }

    // at initial call source does not exist, we create a new one and add it to sock userData
    if (!source)
    {

        // Create libuv request handle
        libuvRqtCtxT *ctx = calloc(1, sizeof(libuvRqtCtxT));
        ctx->httpPool = httpPool;
        ctx->sock = sock;

        // create livuv handle and attache user data context
        source = malloc(sizeof(uv_poll_t));
        source->data = ctx;

        // create libuv event
        err = uv_poll_init_socket(evtLoop, source, sock);
        if (err < 0)
            goto OnErrorExit;

        // attach libuv handle to curl multi socket handle
        err = curl_multi_assign(httpPool->multi, sock, source);
        if (err != CURLM_OK)
            goto OnErrorExit;
    }

    err = uv_poll_start(source, events, glueOnSocketCB);
    if (err < 0)
        goto OnErrorExit;

    return 0;

OnErrorExit:
    return -1;
}

// map libuv ontimer with multi version
static void glueOnTimerCB(uv_timer_t *evtTimer)
{
    httpPoolT *httpPool = (httpPoolT *)evtTimer->data;
    (void)httpOnTimerCB(httpPool);
}

// call httpOnTimerCB after xx milliseconds
static int glueSetTimerCB(httpPoolT *httpPool, long timeout)
{
    int err;
    uv_timer_t *evtTimer = (uv_timer_t *)httpPool->evtTimer;

    // timer does not exit, create one and attach httpPool to userdata context
    if (!evtTimer)
    {
        evtTimer = malloc(sizeof(uv_timer_t));
        evtTimer->data = httpPool;
        uv_timer_init(httpPool->evtLoop, evtTimer);
        httpPool->evtTimer = evtTimer;
    }

    // start or stop timer (in ms)
    if (timeout < 0)
        uv_timer_stop(evtTimer);
    else
        uv_timer_start(evtTimer, glueOnTimerCB, timeout, 0);

    return 0;
}

// run mainloop
static int glueRunLoop(httpPoolT *httpPool, long seconds)
{
    int status = uv_run((uv_loop_t *)httpPool->evtLoop, seconds * 1000000);
    return status;
}

// create a new systemd event loop
static void *glueNewEventLoop()
{
    uv_loop_t *evtLoop = malloc(sizeof(uv_loop_t));
    uv_loop_init(evtLoop);
    return (void *)evtLoop;
}

static httpCallbacksT libUvCbs = {
    .multiTimer = glueSetTimerCB,
    .multiSocket = glueSetSocketCB,
    .evtMainLoop = glueNewEventLoop,
    .evtRunLoop = glueRunLoop,
};

httpCallbacksT *glueGetCbs()
{
    return &libUvCbs;
}