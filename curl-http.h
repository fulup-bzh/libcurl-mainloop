/*
 * Copyright (C) 2021 "IoT.bzh"
 * Author "Fulup Ar Foll" <fulup@iot.bzh>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at https://opensource.org/licenses/MIT.
 * $RP_END_LICENSE$
 *
 * Examples:
 *  GET  httpSendGet(oidc->httpPool, "https://example.com", idp->headers, NULL|token, NULL|opts, callback, ctx);
 *  POST httpSendPost(oidc->httpPool, url, idp->headers, NULL|token, NULL|opts, (void*)post,datalen, callback, ctx);
 */

#pragma once

#include <curl/curl.h>
#include <sys/types.h>
#include <systemd/sd-event.h>

#define MAGIC_HTTP_RQT 951357
#define MAGIC_HTTP_POOL 583498
#define DFLT_HEADER_MAX_LEN 1024

typedef struct {
  const char *tag;
  const char *value;
} httpQueryT;

typedef struct {
  const char* tag;
  const char *value;
} httpHeadersT;

typedef struct {
 	char *username;
	char *password;
	char *bearer;
	long  timeout;
	long  sslchk;
	long verbose;
	const char *proxy;
	const char *cainfo;
	const char *sslcert;
	const char *sslkey;
	const char *tostr;
} httpOptsT;

typedef struct httpRqtS httpRqtT;
typedef void (*httpRqtCbT)(httpRqtT *httpRqt);

typedef struct httpRqtS {
	int magic;
	int verbose;
	char *body;
	char *headers;
	char *ctype;
	long length;
	long hdrLen;
	long bodyLen;
	long status;
    char error[CURL_ERROR_SIZE];
    void *easy;
	struct timespec startTime;
	struct timespec stopTime;
	uint64_t msTime;
	void *context;
	httpRqtCbT callback;
} httpRqtT;

typedef struct httpPoolS {
	int magic;
	int verbose;
    sd_event *evtLoop;
	sd_event_source *timer;
    CURLM *multi;
} httpPoolT;

httpPoolT* httpCreatePool(sd_event *evtLoop, int verbose);
int httpBuildQuery (const char *uid, char *response, size_t maxlen, const char *prefix, const char *url, httpQueryT *query);
int httpSendPost (httpPoolT *pool, const char* url, const httpHeadersT *headers, httpHeadersT *tokens, httpOptsT *opts, void *databuf, long datalen, httpRqtCbT callback, void* ctx);
int httpSendGet  (httpPoolT *pool, const char* url, const httpHeadersT *headers, httpHeadersT *tokens, httpOptsT *opts, httpRqtCbT callback, void* ctx);
