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

static int count=0; // global pending http request


// The URL callback in your application where users are sent after authorization.
static void sampleCallback (httpRqtT *httpRqt) {
  	assert (httpRqt->magic == MAGIC_HTTP_RQT);
	long reqId= (long)httpRqt->context; // retreive reqId from request context

	if (httpRqt->status != 200) goto OnErrorExit;

	double seconds= (double)httpRqt->msTime/1000.0;
	fprintf (stdout, "[body]=%s", httpRqt->body);
	fprintf (stdout, "[request-ok] reqId=%d elapsed=%2.2fs\n", reqId, httpRqt->msTime, seconds);
	free (httpRqt);
	count --;
	return;

OnErrorExit:
	fprintf (stderr, "[request-fx] status=%ld", httpRqt->status);
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
	int err, start;
	runModT runmode=MOD_DEFAULT;

	if (argc <= 1) {
		fprintf (stderr, "syntax curl-http [-s|-a] url-1, ... url-n\n");
		goto OnErrorExit;
	}

	// check for option and shift argv as needed
	if (!strcasecmp (argv[1], "-s")) runmode= MOD_SYNC;
	if (!strcasecmp (argv[1], "-a")) runmode= MOD_ASYNC;
	if (runmode == MOD_DEFAULT) start=1;
	else start =2;

	// if asynchronous request a new event loop
	if (runmode == MOD_SYNC) fprintf (stdout, "-- synchronous mode\n");
	else {
		sd_event *evtLoop;
		fprintf (stdout, "-- asynchronous mode\n");
		// create a new systemd event loop
		err= sd_event_new(&evtLoop);
		if (err) {
			fprintf (stderr, "fail to create evtLoop\n");
			goto OnErrorExit;
		}

		// create multi pool and attach systemd eventloop
		httpPool= httpCreatePool(evtLoop);
		if (!httpPool) {
			fprintf (stderr, "fail to create libcurl multi pool\n");
			goto OnErrorExit;
		}
	}

	// launch all or request in asynchronous mode.
	fprintf (stdout, "-- Launching %d request\n", argc-start);
	for (long reqId=start; reqId < argc; reqId++) {
		url=argv[reqId];
		fprintf (stdout, "--- request: reqId=%d %s\n", reqId, url);

		// basic get with no header, token, query or options
		err= httpSendGet  (httpPool, url, NULL/*headers*/, NULL/*token*/, NULL/*opts*/, sampleCallback, (void*)reqId);
		if (!err) count++;
		else {
			fprintf (stderr, "fail launch request url=%s\n", url);
			goto OnErrorExit;
		}
	}

	// wait for all pending request to be finished
	if (runmode != MOD_SYNC) while (count) {
		// check every second that no more request are pending
		err= sd_event_run(httpPool->evtLoop, 1000000);
		fprintf (stdout, "--- waiting for %d pending request(s) to finish\n", count);
	}

	fprintf (stdout, "-- Done (no more pending request)\n", count);
	exit(0);

OnErrorExit:
	exit (1);
}