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
#include <stdint.h>

#define MAGIC_HTTP_RQT 951357
#define MAGIC_HTTP_POOL 583498
#define DFLT_HEADER_MAX_LEN 1024

typedef struct httpPoolS httpPoolT;

typedef enum
{
    HTTP_HANDLE_FREE,
    HTTP_HANDLE_KEEP,
} httpRqtActionT;

typedef struct
{
    const char *tag;
    const char *value;
} httpKeyValT;

// curl options
typedef struct
{
    char *username;
    char *password;
    char *bearer;
    long timeout;
    long sslchk;
    long verbose;
    long maxsz;
    long speedlimit;
    long speedlow;
    long maxredir;
    const char *proxy;
    const char *cainfo;
    const char *sslcert;
    const char *sslkey;
    const char *tostr;
} httpOptsT;

typedef struct httpRqtS httpRqtT;
typedef httpRqtActionT (*httpRqtCbT)(httpRqtT *httpRqt);
typedef void (*httpFreeCtxCbT)(void *userData);

// http request handle
typedef struct httpRqtS
{
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
    void *userData;
    httpRqtCbT callback;
    httpFreeCtxCbT freeCtx;
} httpRqtT;

// mainloop glue API interface
typedef void *(*evtMainLoopCbT)();
typedef int (*multiTimerCbT)(httpPoolT *httpPool, long timeout);
typedef int (*multiSocketCbT)(httpPoolT *httpPool, CURL *easy, int sock, int action, void *sockp);
typedef int (*evtRunLoopCbT)(httpPoolT *httpPool, long seconds);

// glue callbacks handle
typedef struct
{
    evtMainLoopCbT evtMainLoop;
    evtRunLoopCbT evtRunLoop;
    multiTimerCbT multiTimer;
    multiSocketCbT multiSocket;
} httpCallbacksT;

// multi-pool handle
typedef struct httpPoolS
{
    int magic;
    int verbose;
    CURLM *multi;
    void *evtLoop;
    void *evtTimer;
    httpCallbacksT *callback;
} httpPoolT;

// glue proto to get mainloop callbacks
httpCallbacksT *glueGetCbs(void);

// API to build and lauch request (if httpPoolT==NULL then run synchronously)
int httpBuildQuery(const char *uid, char *response, size_t maxlen, const char *prefix, const char *url, httpKeyValT *query);
int httpSendPost(httpPoolT *pool, const char *url, const httpKeyValT *headers, httpKeyValT *tokens, httpOptsT *opts, void *databuf, long datalen, httpRqtCbT callback, void *ctx, httpFreeCtxCbT freeCtx);
int httpSendGet(httpPoolT *pool, const char *url, const httpKeyValT *headers, httpKeyValT *tokens, httpOptsT *opts, httpRqtCbT callback, void *ctx, httpFreeCtxCbT freeCtx);

// init curl multi pool with an abstract mainloop and corresponding callbacks
httpPoolT *httpCreatePool(void *evtLoop, httpCallbacksT *mainLoopCbs, int verbose);

// curl action callback to be called from glue layer
int httpOnSocketCB(httpPoolT *httpPool, int sock, int action);
int httpOnTimerCB(httpPoolT *httpPool);
