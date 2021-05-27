/*
 * Copyright (C) 2021 "IoT.bzh"
 * Author "Fulup Ar Foll" <fulup@iot.bzh>
 *
 * Use of this efd code is governed by an MIT-style
 * license that can be found in the LICENSE file or at https://opensource.org/licenses/MIT.
 * $RP_END_LICENSE$
 */

#define _GNU_SOURCE

#include "oidc-defaults.h"
#include "http-client.h"

#include <errno.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>

#include <libafb/core/afb-ev-mgr.h>
#include <libafb/sys/ev-mgr.h>
#include <libafb/core/afb-sched.h>

#define FLAGS_SET(v, flags) ((~(v) & (flags)) == 0)

//  (void *efd, int sock, uint32_t revents, void *ctx)
static void glueOnSocketCB (struct ev_fd *efd, int sock, uint32_t revents, void *ctx)
{
    httpPoolT *httpPool= (httpPoolT*)ctx;
    int action;

    // translate libafb event into curl event
    if (FLAGS_SET(revents, EPOLLIN | EPOLLOUT)) action= CURL_POLL_INOUT;
    else if (revents & EPOLLIN)  action= CURL_POLL_IN;
    else if (revents & EPOLLOUT) action= CURL_POLL_OUT;
    else action= 0;

    int err= httpOnSocketCB(httpPool, sock, action);
    if (err) ev_fd_unref(efd);
}

// create libafb efd event and attach http processing callback to sock fd
static int glueSetSocketCB (httpPoolT *httpPool, CURL *easy, int sock, int action, void *sockp)
{
    struct ev_fd *efd= (struct ev_fd *) sockp; // on 1st call efd is null
    assert (httpPool->magic == MAGIC_HTTP_POOL);
    uint32_t events= 0;
    int err;

    // map CURL events with system events
    switch (action) {
      case CURL_POLL_REMOVE:
        goto OnErrorExit;
      case CURL_POLL_IN:
        events= EPOLLIN;
        break;
      case CURL_POLL_OUT:
        events= EPOLLOUT;
        break;
      case CURL_POLL_INOUT:
        events= EPOLLIN|EPOLLOUT;
        break;
      default:
        goto OnErrorExit;
    }

	// if efd exit set event else create a new efd
    if (!efd) {

		// create a new efd
		err= afb_ev_mgr_add_fd(&efd, sock, events, glueOnSocketCB, httpPool, 0, 1);
		if (err < 0) goto OnErrorExit;

		// add new created efd to sock context on 2nd call it will comeback as sockCtx
		err= curl_multi_assign(httpPool->multi, sock, efd);
		if (err != CURLM_OK) goto OnErrorExit;

	} else {
	 	ev_fd_set_events (efd, ev_fd_events(efd) | events);
	}

    return 0;

OnErrorExit:
    return -1;
}

// map libafb ontimer with multi version
static void glueOnTimerCB(int signal, void *ctx)
{
    // signal should be null
    if (signal) return;

    httpPoolT *httpPool = (httpPoolT *)ctx;
    (void)httpOnTimerCB(httpPool);
}

// arm a one shot timer in ms
static int glueSetTimerCB(httpPoolT *httpPool, long timeout)
{
    int err;

    if (timeout >= 0) {
      // ms delay for OnTimerCB (timeout is dynamic and depends on CURLOPT_LOW_SPEED_TIME)
      err= afb_sched_post_job (NULL /*group*/, timeout,  0 /*exec-timeout*/,glueOnTimerCB, httpPool);
	  if (err <= 0) goto OnErrorExit;
    }
    return 0;

OnErrorExit:
    return -1;
}


static httpCallbacksT libafbCbs = {
    .multiTimer = glueSetTimerCB,
    .multiSocket = glueSetSocketCB,
    .evtMainLoop = NULL,
    .evtRunLoop = NULL,
};

httpCallbacksT *glueGetCbs()
{
    return &libafbCbs;
}