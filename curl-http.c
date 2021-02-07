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

#define FLAGS_SET(v, flags) ((~(v) & (flags)) == 0)


// callback might be called as many time as needed to transfert all data
static size_t httpBodyCB(void *data, size_t blkSize, size_t blkCount, void *ctx)  {
  httpRqtT *httpRqt = (httpRqtT*)ctx;
  assert (httpRqt->magic == MAGIC_HTTP_RQT);
  size_t size= blkSize * blkCount;

  fprintf (stderr, "--- httpBodyCB: blkSize=%ld blkCount=%ld\n", blkSize, blkCount);

  // final callback is called from multiCheckInfoCB when CURLMSG_DONE
  if (!data) return 0;

  httpRqt->body= realloc(httpRqt->body, httpRqt->hdrLen + size + 1);
  if(!httpRqt->body) return 0;  // hoops

  memcpy(&(httpRqt->body[httpRqt->bodyLen]), data, size);
  httpRqt->bodyLen += size;
  httpRqt->body[httpRqt->bodyLen] = 0;

  return size;
}

// callback might be called as many time as needed to transfert all data
static size_t httpHeadersCB(void *data, size_t blkSize, size_t blkCount, void *ctx)  {
  httpRqtT *httpRqt = (httpRqtT*)ctx;
  assert (httpRqt->magic == MAGIC_HTTP_RQT);
  size_t size= blkSize * blkCount;

  // final callback is called from multiCheckInfoCB when CURLMSG_DONE
  if (!data) return 0;

  httpRqt->headers= realloc(httpRqt->headers, httpRqt->hdrLen + size + 1);
  if(!httpRqt->headers) return 0;  // hoops

  memcpy(&(httpRqt->headers[httpRqt->hdrLen]), data, size);
  httpRqt->hdrLen += size;
  httpRqt->headers[httpRqt->hdrLen] = 0;

  return size;
}

static void multiCheckInfoCB (httpPoolT *httpPool) {
  int count;
  CURLMsg *msg;
  httpRqtT *httpRqt;

    // read action resulting messages
    while ((msg = curl_multi_info_read(httpPool->multi, &count))) {
 	    fprintf (stderr, "----- multiCheckInfoCB: status=%d \n", msg->msg);

		if (msg->msg == CURLMSG_DONE) {

			// this is a pool request 1st search for easyhandle
			CURL *easy = msg->easy_handle;
		    fprintf(stderr,"multiCheckInfoCB: done\n");

			// retreive httpRqt from private easy handle
			curl_easy_getinfo(easy, CURLINFO_PRIVATE, &httpRqt);
			curl_easy_getinfo(httpRqt->easy, CURLINFO_SIZE_DOWNLOAD, &httpRqt->length);
			curl_easy_getinfo(httpRqt->easy, CURLINFO_RESPONSE_CODE, &httpRqt->status);
			curl_easy_getinfo(httpRqt->easy, CURLINFO_CONTENT_TYPE, &httpRqt->ctype);

			// do some clean up
			curl_multi_remove_handle(httpPool->multi, easy);
			curl_easy_cleanup(easy);

			// compute request elapsed time
			clock_gettime (CLOCK_MONOTONIC, &httpRqt->stopTime);
			httpRqt->msTime = (httpRqt->stopTime.tv_nsec - httpRqt->startTime.tv_nsec)/1000000 + (httpRqt->stopTime.tv_sec - httpRqt->startTime.tv_sec)*1000;

			// call request callback (note: callback should free httpRqt)
			httpRqt->callback (httpRqt);

			break;
        }
    }
}

// call from systemd evtLoop. Map event name and pass event to curl action loop
static int multiHttpCB (sd_event_source *source, int sock, uint32_t revents, void *ctx) {
    httpPoolT *httpPool= (httpPoolT*)ctx;
    assert (httpPool->magic == MAGIC_HTTP_POOL);
    int action, running=0;

   // translate systemd event into curl event
    if (FLAGS_SET(revents, EPOLLIN | EPOLLOUT)) action= CURL_POLL_INOUT;
    else if (revents & EPOLLIN)  action= CURL_POLL_IN;
    else if (revents & EPOLLOUT) action= CURL_POLL_OUT;
    else action= 0;

    fprintf (stderr, "multiHttpCB: sock=%d revent=%d action=%d\n", sock, revents, action);
    CURLMcode status= curl_multi_socket_action(httpPool->multi, sock, action, &running);
    if (status != CURLM_OK) goto OnErrorExit;

    multiCheckInfoCB (httpPool);
    return 0;

OnErrorExit:
    fprintf (stderr,"[curl-multi-action-fail]: curl_multi_socket_action fail (multiHttpCB)");
    return -1;
}

static int multiOnSocketCB (CURL *easy, int sock, int action, void *callbackCtx, void *sockCtx) {
    sd_event_source *source= (sd_event_source *) sockCtx; // on 1st call source is null
    httpPoolT *httpPool= (httpPoolT*)callbackCtx;
    assert (httpPool->magic == MAGIC_HTTP_POOL);
    uint32_t events= 0;
    int err;

    // map CURL events with system events
    switch (action) {
      case CURL_POLL_REMOVE:
	    fprintf (stderr,"[not-user-anymore] should not be called (multiOnSocketCB)");
        break;
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
    if (source) {
      err = sd_event_source_set_io_events(source, events);
      if (err < 0) goto OnErrorExit;

      err = sd_event_source_set_enabled(source, SD_EVENT_ON);
      if (err < 0) goto OnErrorExit;

    } else {

      // at initial call source does not exist, we create a new one and add it to sock context
      err= sd_event_add_io(httpPool->evtLoop, &source, sock, events, multiHttpCB, httpPool);
      if (err < 0) goto OnErrorExit;

      // add new created source to soeck context on 2nd call it will comeback as socketp
      err= curl_multi_assign(httpPool->multi, sock, source);
      if (err != CURLM_OK) goto OnErrorExit;

      (void) sd_event_source_set_description(source, "afb-curl");
    }

    return 0;

OnErrorExit:
  fprintf (stderr,"[curl-source-attach-fail] curl_multi_assign failed (multiOnSocketCB)");
  return -1;
}

// Curl needs curl_multi_socket_action to be called on regular events
static int multiOnTimerCB(sd_event_source *timer, uint64_t usec, void *ctx) {
    httpPoolT *httpPool= (httpPoolT*)ctx;
    assert (httpPool->magic == MAGIC_HTTP_POOL);
    int running= 0;

    curl_multi_perform(httpPool->multi, &running);
    int err= curl_multi_socket_action(httpPool->multi, CURL_SOCKET_TIMEOUT, 0, &running) ;
    if (err != CURLM_OK) goto OnErrorExit;

    multiCheckInfoCB (httpPool);
    return 0;

OnErrorExit:
    fprintf (stderr,"multiOnTimerCB: curl_multi_socket_action fail\n");
    return -1;
}

// Curl request immediate/deferred actions through CURLMOPT_TIMERFUNCTION
static int multiSetTimerCB (CURLM *curl, long timeout, void *ctx) {
    httpPoolT *httpPool= (httpPoolT*)ctx;
    assert (httpPool->magic == MAGIC_HTTP_POOL);
	int err;

	printf ("** set timer callback timeout=%ld\n", timeout);
	if (timeout > 1000) timeout=1000;

    // if time is negative just kill it
    if (timeout < 0) {
        if (httpPool->timer) {
              err= sd_event_source_set_enabled(httpPool->timer, SD_EVENT_OFF);
              if (err < 0) goto OnErrorExit;
        }
    } else {
        uint64_t usec;
        sd_event_now(httpPool->evtLoop, CLOCK_MONOTONIC, &usec);
        if (!httpPool->timer) { // new timer
            sd_event_add_time(httpPool->evtLoop, &httpPool->timer, CLOCK_MONOTONIC, usec+timeout*1000, 0, multiOnTimerCB, httpPool);
            sd_event_source_set_description(httpPool->timer, "curl-timer");

        } else {
            sd_event_source_set_time(httpPool->timer, usec+timeout*1000);
            sd_event_source_set_enabled(httpPool->timer, SD_EVENT_ONESHOT);
        }
    }
	return 0;

OnErrorExit:
    fprintf (stderr,"[afb-timer-fail] afb_sched_post_job fail error=%d (multiSetTimerCB)", err);
    return -1;
}

static int httpSendQuery (httpPoolT *pool, const char* url, const httpHeadersT *headers, httpHeadersT *tokens, httpOptsT *opts, void *datas, long datalen, httpRqtCbT callback, void* ctx) {
	httpRqtT *httpRqt= calloc(1, sizeof(httpRqtT));
	httpRqt->magic= MAGIC_HTTP_RQT;
    httpRqt->easy = curl_easy_init();
	httpRqt->callback= callback;
	httpRqt->context = ctx;
	clock_gettime (CLOCK_MONOTONIC, &httpRqt->startTime);

	char header[DFLT_HEADER_MAX_LEN];
	struct curl_slist* rqtHeaders= NULL;
	if (headers) for (int idx=0; headers[idx].tag; idx++) {
		snprintf(header, sizeof(header),"%s=%s", headers[idx].tag,headers[idx].value);
		rqtHeaders = curl_slist_append(rqtHeaders, header);
	}

	if (tokens) for (int idx=0; tokens[idx].tag; idx++) {
		snprintf(header, sizeof(header),"%s=%s", tokens[idx].tag,tokens[idx].value);
		rqtHeaders = curl_slist_append(rqtHeaders, header);
	}

    curl_easy_setopt(httpRqt->easy, CURLOPT_URL, url);
    curl_easy_setopt(httpRqt->easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(httpRqt->easy, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(httpRqt->easy, CURLOPT_HEADER, 0L); // do not pass header to bodyCB
    curl_easy_setopt(httpRqt->easy, CURLOPT_WRITEFUNCTION, httpBodyCB);
    curl_easy_setopt(httpRqt->easy, CURLOPT_HEADERFUNCTION, httpHeadersCB);
    curl_easy_setopt(httpRqt->easy, CURLOPT_ERRORBUFFER, httpRqt->error);
    curl_easy_setopt(httpRqt->easy, CURLOPT_HEADERDATA, httpRqt);
    curl_easy_setopt(httpRqt->easy, CURLOPT_WRITEDATA, httpRqt);
    curl_easy_setopt(httpRqt->easy, CURLOPT_PRIVATE, httpRqt);
    curl_easy_setopt(httpRqt->easy, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(httpRqt->easy, CURLOPT_LOW_SPEED_LIMIT, 30L);
	curl_easy_setopt(httpRqt->easy, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(httpRqt->easy, CURLOPT_MAXREDIRS, 5L);

	if (opts) {
    	curl_easy_setopt(httpRqt->easy, CURLOPT_VERBOSE, opts->verbose);
		if (opts->timeout) curl_easy_setopt(httpRqt->easy, CURLOPT_TIMEOUT, opts->timeout);
		if (opts->sslchk)  {
			curl_easy_setopt(httpRqt->easy, CURLOPT_SSL_VERIFYPEER, 1L);
			curl_easy_setopt(httpRqt->easy, CURLOPT_SSL_VERIFYHOST, 1L);
		}
		if (opts->sslcert) curl_easy_setopt(httpRqt->easy, CURLOPT_SSLCERT, opts->sslcert);
		if (opts->sslkey) curl_easy_setopt(httpRqt->easy, CURLOPT_SSLKEY, opts->sslkey);
	}

	if (datas) { // raw post
		curl_easy_setopt(httpRqt->easy, CURLOPT_POSTFIELDSIZE, datalen);
		curl_easy_setopt(httpRqt->easy, CURLOPT_POST, 1L);
		curl_easy_setopt(httpRqt->easy, CURLOPT_POSTFIELDS, datas);
	}

	// add header into final request
	if (rqtHeaders) curl_easy_setopt(httpRqt->easy, CURLOPT_HTTPHEADER, rqtHeaders);

	if (pool) {
		CURLMcode mstatus;
		// if pool add handle and run asynchronously
		mstatus= curl_multi_add_handle (pool->multi, httpRqt->easy);
		if (mstatus != CURLM_OK) {
			fprintf (stderr,"[curl-multi-fail] curl curl_multi_add_handle fail url=%s error=%s (httpSendQuery)", url, curl_multi_strerror(mstatus));
			goto OnErrorExit;
		} else {
			int inprogress;
			mstatus= curl_multi_perform(pool->multi, &inprogress);
			if (mstatus != CURLM_OK) {
                fprintf (stderr,"[curl-multi-fail] curl curl_multi_perform fail url=%s error=%s (httpSendQuery)", url, curl_multi_strerror(mstatus));
                goto OnErrorExit;
            }
		}

	} else {
		CURLcode  estatus;
		// no event loop synchronous call
		estatus= curl_easy_perform(httpRqt->easy);
		if (estatus != CURLE_OK) {
			fprintf (stderr,"utilsSendRqt: curl request fail url=%s error=%s", url, curl_easy_strerror(estatus));
			goto OnErrorExit;
		}

		curl_easy_getinfo(httpRqt->easy, CURLINFO_SIZE_DOWNLOAD, &httpRqt->length);
		curl_easy_getinfo(httpRqt->easy, CURLINFO_RESPONSE_CODE, &httpRqt->status);
		curl_easy_getinfo(httpRqt->easy, CURLINFO_CONTENT_TYPE, &httpRqt->ctype);

		// compute elapsed time and call request callback
		clock_gettime (CLOCK_MONOTONIC, &httpRqt->stopTime);
		httpRqt->msTime = (httpRqt->stopTime.tv_nsec - httpRqt->startTime.tv_nsec)/1000000 + (httpRqt->stopTime.tv_sec - httpRqt->startTime.tv_sec)*1000;

		// call request callback (note: callback should free httpRqt)
		httpRqt->callback (httpRqt);

		// we're done
		curl_easy_cleanup(httpRqt->easy);
	}
	return 0;

OnErrorExit:
	free(httpRqt);
	return 1;
}

int httpSendPost (httpPoolT *pool, const char* url, const httpHeadersT *headers, httpHeadersT *tokens, httpOptsT *opts, void *datas, long len, httpRqtCbT callback, void* ctx) {
	return httpSendQuery (pool, url, headers, tokens, opts, datas, len, callback, ctx);
}

int httpSendGet (httpPoolT *pool, const char* url, const httpHeadersT *headers, httpHeadersT *tokens, httpOptsT *opts, httpRqtCbT callback, void* ctx) {
	return httpSendQuery (pool, url, headers, tokens, opts, NULL, 0, callback, ctx);
}

// build request with query
int httpBuildQuery (const char *uid, char *response, size_t maxlen, const char *prefix, const char *url, httpQueryT *query) {
	size_t index=0;
	maxlen=maxlen-1; // space for '\0'

	// hoops nothing to build url
	if (!prefix && ! url)  goto OnErrorExit;

	// place prefix
	if (prefix) {
		for (int idx=0; prefix[idx]; idx++) {
			response[index++]=prefix[idx];
			if (index == maxlen) goto OnErrorExit;
		}
		response[index++]='/';
	}

	// place url
	if (url) {
		for (int idx=0; url[idx]; idx++) {
			response[index++]=url[idx];
			if (index == maxlen) goto OnErrorExit;
		}
		response[index++]='?';
	}

	// loop on query arguments
	for (int idx=0; query[idx].tag; idx++) {
		for (int jdx=0; query[idx].tag[jdx]; jdx++) {
			response[index++]= query[idx].tag[jdx];
			if (index == maxlen) goto OnErrorExit;
		}
		response[index++]='=';
		for (int jdx=0; query[idx].value[jdx]; jdx++) {
			response[index++]= query[idx].value[jdx];
			if (index == maxlen) goto OnErrorExit;
		}
		response[index++]='&';
	}
	response[index]='\0'; // remove last '&'
	return 0;

OnErrorExit:
	fprintf (stderr,"[url-too-long] idp=%s url=%s cannot add query to url (httpMakeRequest)", uid, url);
	return 1;
}

// Create CURL multi pool and attach it to systemd evtLoop
httpPoolT* httpCreatePool(sd_event *evtLoop) {

	// First call initialise global CURL static data
	static int initialised=0;
	if (!initialised) {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		initialised =1;
	}
	httpPoolT *httpPool;

	httpPool =calloc(1, sizeof(httpPoolT));
	httpPool->magic=MAGIC_HTTP_POOL;
	httpPool->evtLoop= evtLoop;
	httpPool->multi= curl_multi_init();
	if (!httpPool->multi) goto OnErrorExit;

	curl_multi_setopt(httpPool->multi, CURLMOPT_SOCKETDATA, httpPool);
	curl_multi_setopt(httpPool->multi, CURLMOPT_SOCKETFUNCTION, multiOnSocketCB);
	curl_multi_setopt(httpPool->multi, CURLMOPT_TIMERDATA, httpPool);
	curl_multi_setopt(httpPool->multi, CURLMOPT_TIMERFUNCTION, multiSetTimerCB);

	return httpPool;

OnErrorExit:
	fprintf (stderr,"[pool-create-fail] hoop curl_multi_init failed (httpCreatePool)");
	free(httpPool);
	return NULL;
}