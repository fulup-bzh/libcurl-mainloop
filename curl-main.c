/*
 * Copyright (C) 2021 "IoT.bzh"
 * Author "Fulup Ar Foll" <fulup@iot.bzh>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at https://opensource.org/licenses/MIT.
 * $RP_END_LICENSE$
 *
 * --------------------------------------------------------------------------------
 * Usage:
 *		+ build query to url before request
 *		+ add header+token+options to request
 *		+ send your request
 *			- get httpSendGet
 *			- post httpSendPost/httpSendJson
 * --------------------------------------------------------------------------------
 *	char urltarget[DFLT_URL_MAX_LEN]; // final url as compose by httpBuildQuery (should be big enough)
 *
 *	// headers + tokens uses the same syntax and are merge in httpSendGet
 *	const httpHeadersT sampleHeaders[]= {
 *		{.tag="Content-type", .value="application/x-www-form-urlencoded"},
 *		{.tag="Accept", .value="application/json"},
 *		{NULL}  // terminator
 *	};
 *
 *	// Url query needs to be build before sending the request with httpBuildQuery
 *	httpQueryT query[]= {
 *		{.tag="client_id"    , .value=idp->credentials->clientId},
 *		{.tag="client_secret", .value=idp->credentials->secret},
 *		{.tag="code"         , .value=code},
 *		{.tag="redirect_uri" , .value=redirectUrl},
 *		{.tag="state"        , .value=afb_session_uuid(hreq->comreq.session)},
 *
 *		{NULL} // terminator
 *	};
 *
 *  // build your query and send your request
 *	err= httpBuildQuery (idp->uid, urltarget, sizeof(url), urlPrefix, urlSource, query);
 *	err= httpSendGet  (pool, urltarget, headers, tokens, opts, callback, (void*)context);
 *  err= httpSendPost (pool, urltarget, headers, tokens, opts, (void*)databuf, datalen, callback, void*(context));
 *
 **/

#define _GNU_SOURCE

#include "curl-http.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// default mainloop timeout 1s
#ifndef MAIN_LOOP_WAIT
#define MAIN_LOOP_WAIT 1
#endif

static int count=0; // global pending http request

typedef struct {
	int uid;
	const char *url;
} reqCtxT;


// The URL callback in your application where users are sent after authorization.
static void sampleCallback (httpRqtT *httpRqt) {
  	assert (httpRqt->magic == MAGIC_HTTP_RQT);
	reqCtxT *ctxRqt = (reqCtxT*)httpRqt->context;

	if (httpRqt->status != 200) goto OnErrorExit;

	double seconds= (double)httpRqt->msTime/1000.0;
	fprintf (stdout, "\n[body]=%s", httpRqt->body);
	fprintf (stdout, "[request-ok] reqId=%d elapsed=%2.2fs url=%s\n", ctxRqt->uid, seconds, ctxRqt->url);
	free (httpRqt->context);
	free (httpRqt);
	count --;
	return;

OnErrorExit:
	fprintf (stderr, "[request-fx] status=%ld", httpRqt->status);
	free (httpRqt->context);
	free (httpRqt);
	count --;
}

typedef enum {
	MOD_SYNC,
	MOD_ASYNC,
	MOD_DEFAULT,
} runModT;

int main(int argc, char *argv[]) {
	const char * url;
	httpPoolT* httpPool=NULL;
	int err, start, verbose=0;
	runModT runmode=MOD_DEFAULT;
	long uid=0;

	if (argc <= 1) {
		fprintf (stderr, "syntax curl-http [-v] [-s|-a] url-1, ... url-n\n");
		goto OnErrorExit;
	}

	// check for option and shift argv as needed
	for (start=1; argv[start][0] == '-'; start ++) {
		if (!strcasecmp (argv[start], "-s")) runmode= MOD_SYNC;
		if (!strcasecmp (argv[start], "-a")) runmode= MOD_ASYNC;
		if (!strcasecmp (argv[start], "-v")) verbose++;
		if (!strcasecmp (argv[start], "-vv")) verbose=+2;
		if (!strcasecmp (argv[start], "-vvv")) verbose=+3;
	}

	// if asynchronous request a new event loop and create http multi pool
	if (runmode != MOD_SYNC)  {
		sd_event *evtLoop;
		// create a new systemd event loop
		err= sd_event_new(&evtLoop);
		if (err) {
			fprintf (stderr, "fail to create evtLoop\n");
			goto OnErrorExit;
		}

		// create multi pool and attach systemd eventloop
		httpPool= httpCreatePool(evtLoop, verbose);
		if (!httpPool) {
			fprintf (stderr, "fail to create libcurl multi pool\n");
			goto OnErrorExit;
		}
	}

	// launch all or request in asynchronous mode.
	if (verbose) fprintf (stdout, "-- Launching %d request\n", (argc - start));
	for (long reqId=start; reqId < argc; reqId++) {

		// add a sample context to http request
		reqCtxT *ctxRqt= calloc (1, sizeof(reqCtxT));
		ctxRqt->url=argv[reqId];
		ctxRqt->uid= uid++;

		if (verbose) fprintf (stdout, "-- request: reqId=%d %s\n", ctxRqt->uid, ctxRqt->url);

		// basic get with no header, token, query or options
		err= httpSendGet  (httpPool, ctxRqt->url, NULL/*headers*/, NULL/*token*/, NULL/*opts*/, sampleCallback, (void*)ctxRqt);
		if (!err) count++;
		else {
			fprintf (stderr, "fail launch request url=%s\n", ctxRqt->url);
			goto OnErrorExit;
		}
	}

	// wait for all pending request to be finished
	if (runmode != MOD_SYNC) while (count) {
		// enter mainloop and ping stdout every xxx seconds if nothing happen
		err= sd_event_run(httpPool->evtLoop, MAIN_LOOP_WAIT *1000000);
		if (verbose > 1) fprintf (stdout, "-- waiting %d pending request(s)\n", count);
		else fprintf (stdout,".");
	}

	fprintf (stdout, "-- Done (no more pending request)\n", count);
	exit(0);

OnErrorExit:
	exit (1);
}