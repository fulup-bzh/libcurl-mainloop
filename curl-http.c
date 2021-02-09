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

#define FLAGS_SET(v, flags) ((~(v) & (flags)) == 0)

// build request with query
int httpBuildQuery(const char *uid, char *response, size_t maxlen, const char *prefix, const char *url, httpKeyValT *query)
{
    size_t index = 0;
    maxlen = maxlen - 1; // space for '\0'

    // hoops nothing to build url
    if (!prefix && !url)
        goto OnErrorExit;

    // place prefix
    if (prefix)
    {
        for (int idx = 0; prefix[idx]; idx++)
        {
            response[index++] = prefix[idx];
            if (index == maxlen)
                goto OnErrorExit;
        }
        response[index++] = '/';
    }

    // place url
    if (url)
    {
        for (int idx = 0; url[idx]; idx++)
        {
            response[index++] = url[idx];
            if (index == maxlen)
                goto OnErrorExit;
        }
        response[index++] = '?';
    }

    // loop on query arguments
    for (int idx = 0; query[idx].tag; idx++)
    {
        for (int jdx = 0; query[idx].tag[jdx]; jdx++)
        {
            response[index++] = query[idx].tag[jdx];
            if (index == maxlen)
                goto OnErrorExit;
        }
        response[index++] = '=';
        for (int jdx = 0; query[idx].value[jdx]; jdx++)
        {
            response[index++] = query[idx].value[jdx];
            if (index == maxlen)
                goto OnErrorExit;
        }
        response[index++] = '&';
    }
    response[index] = '\0'; // remove last '&'
    return 0;

OnErrorExit:
    fprintf(stderr, "[url-too-long] idp=%s url=%s cannot add query to url (httpMakeRequest)", uid, url);
    return 1;
}

// callback might be called as many time as needed to transfert all data
static size_t httpBodyCB(void *data, size_t blkSize, size_t blkCount, void *ctx)
{
    httpRqtT *httpRqt = (httpRqtT *)ctx;
    assert(httpRqt->magic == MAGIC_HTTP_RQT);
    size_t size = blkSize * blkCount;

    if (httpRqt->verbose > 1)
        fprintf(stderr, "-- httpBodyCB: blkSize=%ld blkCount=%ld\n", blkSize, blkCount);

    // final callback is called from multiCheckInfoCB when CURLMSG_DONE
    if (!data)
        return 0;

    httpRqt->body = realloc(httpRqt->body, httpRqt->hdrLen + size + 1);
    if (!httpRqt->body)
        return 0; // hoops

    memcpy(&(httpRqt->body[httpRqt->bodyLen]), data, size);
    httpRqt->bodyLen += size;
    httpRqt->body[httpRqt->bodyLen] = 0;

    return size;
}

// callback might be called as many time as needed to transfert all data
static size_t httpHeadersCB(void *data, size_t blkSize, size_t blkCount, void *ctx)
{
    httpRqtT *httpRqt = (httpRqtT *)ctx;
    assert(httpRqt->magic == MAGIC_HTTP_RQT);
    size_t size = blkSize * blkCount;

    if (httpRqt->verbose > 2)
        fprintf(stderr, "-- httpHeadersCB: blkSize=%ld blkCount=%ld\n", blkSize, blkCount);

    // final callback is called from multiCheckInfoCB when CURLMSG_DONE
    if (!data)
        return 0;

    httpRqt->headers = realloc(httpRqt->headers, httpRqt->hdrLen + size + 1);
    if (!httpRqt->headers)
        return 0; // hoops

    memcpy(&(httpRqt->headers[httpRqt->hdrLen]), data, size);
    httpRqt->hdrLen += size;
    httpRqt->headers[httpRqt->hdrLen] = 0;

    return size;
}

static void multiCheckInfoCB(httpPoolT *httpPool)
{
    int count;
    CURLMsg *msg;

    // read action resulting messages
    while ((msg = curl_multi_info_read(httpPool->multi, &count)))
    {
        if (httpPool->verbose > 2)
            fprintf(stderr, "-- multiCheckInfoCB: status=%d \n", msg->msg);

        if (msg->msg == CURLMSG_DONE)
        {
            httpRqtT *httpRqt;

            // this is a httpPool request 1st search for easyhandle
            CURL *easy = msg->easy_handle;

            // retreive httpRqt from private easy handle
            if (httpPool->verbose > 1)
                fprintf(stderr, "-- multiCheckInfoCB: done\n");
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &httpRqt);
            curl_easy_getinfo(httpRqt->easy, CURLINFO_SIZE_DOWNLOAD, &httpRqt->length);
            curl_easy_getinfo(httpRqt->easy, CURLINFO_RESPONSE_CODE, &httpRqt->status);
            curl_easy_getinfo(httpRqt->easy, CURLINFO_CONTENT_TYPE, &httpRqt->ctype);

            // do some clean up
            curl_multi_remove_handle(httpPool->multi, easy);
            curl_easy_cleanup(easy);

            // compute request elapsed time
            clock_gettime(CLOCK_MONOTONIC, &httpRqt->stopTime);
            httpRqt->msTime = (httpRqt->stopTime.tv_nsec - httpRqt->startTime.tv_nsec) / 1000000 + (httpRqt->stopTime.tv_sec - httpRqt->startTime.tv_sec) * 1000;

            // call request callback (note: callback should free httpRqt)
            httpRqtActionT status = httpRqt->callback(httpRqt);
            if (status == HTTP_HANDLE_FREE)
            {
                if (httpRqt->freeCtx && httpRqt->userData)
                    httpRqt->freeCtx(httpRqt->userData);
                free(httpRqt);
            }

            break;
        }
    }
}

// call from glue evtLoop. Map event name and pass event to curl action loop
int httpOnSocketCB(httpPoolT *httpPool, int sock, int action)
{
    assert(httpPool->magic == MAGIC_HTTP_POOL);
    int running = 0;

    if (httpPool->verbose > 2)
        fprintf(stderr, "httpOnSocketCB: sock=%d action=%d\n", sock, action);
    CURLMcode status = curl_multi_socket_action(httpPool->multi, sock, action, &running);
    if (status != CURLM_OK)
        goto OnErrorExit;

    multiCheckInfoCB(httpPool);
    return 0;

OnErrorExit:
    fprintf(stderr, "[curl-multi-action-fail]: curl_multi_socket_action fail (httpOnSocketCB)");
    return -1;
}

// called from glue event loop as Curl needs curl_multi_socket_action to be called regularly
int httpOnTimerCB(httpPoolT *httpPool)
{
    assert(httpPool->magic == MAGIC_HTTP_POOL);
    int running = 0;

    // timer transfers request to socket action (don't use curl_multi_perform)
    int err = curl_multi_socket_action(httpPool->multi, CURL_SOCKET_TIMEOUT, 0, &running);
    if (err != CURLM_OK)
        goto OnErrorExit;

    multiCheckInfoCB(httpPool);
    return 0;

OnErrorExit:
    fprintf(stderr, "multiOnTimerCB: curl_multi_socket_action fail\n");
    return -1;
}

static int httpSendQuery(httpPoolT *httpPool, const char *url, const httpKeyValT *headers, httpKeyValT *tokens, httpOptsT *opts, void *datas, long datalen, httpRqtCbT callback, void *ctx, httpFreeCtxCbT freeCtx)
{
    httpRqtT *httpRqt = calloc(1, sizeof(httpRqtT));
    httpRqt->magic = MAGIC_HTTP_RQT;
    httpRqt->easy = curl_easy_init();
    httpRqt->callback = callback;
    httpRqt->freeCtx = freeCtx;
    httpRqt->userData = ctx;
    clock_gettime(CLOCK_MONOTONIC, &httpRqt->startTime);

    char header[DFLT_HEADER_MAX_LEN];
    struct curl_slist *rqtHeaders = NULL;
    if (headers)
        for (int idx = 0; headers[idx].tag; idx++)
        {
            snprintf(header, sizeof(header), "%s=%s", headers[idx].tag, headers[idx].value);
            rqtHeaders = curl_slist_append(rqtHeaders, header);
        }

    if (tokens)
        for (int idx = 0; tokens[idx].tag; idx++)
        {
            snprintf(header, sizeof(header), "%s=%s", tokens[idx].tag, tokens[idx].value);
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

    if (opts)
    {
        curl_easy_setopt(httpRqt->easy, CURLOPT_VERBOSE, opts->verbose);
        if (opts->timeout)
            curl_easy_setopt(httpRqt->easy, CURLOPT_TIMEOUT, opts->timeout);
        if (opts->sslchk)
        {
            curl_easy_setopt(httpRqt->easy, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(httpRqt->easy, CURLOPT_SSL_VERIFYHOST, 1L);
        }
        if (opts->sslcert)
            curl_easy_setopt(httpRqt->easy, CURLOPT_SSLCERT, opts->sslcert);
        if (opts->sslkey)
            curl_easy_setopt(httpRqt->easy, CURLOPT_SSLKEY, opts->sslkey);
    }

    if (datas)
    { // raw post
        curl_easy_setopt(httpRqt->easy, CURLOPT_POSTFIELDSIZE, datalen);
        curl_easy_setopt(httpRqt->easy, CURLOPT_POST, 1L);
        curl_easy_setopt(httpRqt->easy, CURLOPT_POSTFIELDS, datas);
    }

    // add header into final request
    if (rqtHeaders)
        curl_easy_setopt(httpRqt->easy, CURLOPT_HTTPHEADER, rqtHeaders);

    if (httpPool)
    {
        CURLMcode mstatus;
        httpRqt->verbose = httpPool->verbose;

        // if httpPool add handle and run asynchronously
        mstatus = curl_multi_add_handle(httpPool->multi, httpRqt->easy);
        if (mstatus != CURLM_OK)
        {
            fprintf(stderr, "[curl-multi-fail] curl curl_multi_add_handle fail url=%s error=%s (httpSendQuery)", url, curl_multi_strerror(mstatus));
            goto OnErrorExit;
        }
    }
    else
    {
        CURLcode estatus;
        // no event loop synchronous call
        estatus = curl_easy_perform(httpRqt->easy);
        if (estatus != CURLE_OK)
        {
            fprintf(stderr, "utilsSendRqt: curl request fail url=%s error=%s", url, curl_easy_strerror(estatus));
            goto OnErrorExit;
        }

        curl_easy_getinfo(httpRqt->easy, CURLINFO_SIZE_DOWNLOAD, &httpRqt->length);
        curl_easy_getinfo(httpRqt->easy, CURLINFO_RESPONSE_CODE, &httpRqt->status);
        curl_easy_getinfo(httpRqt->easy, CURLINFO_CONTENT_TYPE, &httpRqt->ctype);

        // compute elapsed time and call request callback
        clock_gettime(CLOCK_MONOTONIC, &httpRqt->stopTime);
        httpRqt->msTime = (httpRqt->stopTime.tv_nsec - httpRqt->startTime.tv_nsec) / 1000000 + (httpRqt->stopTime.tv_sec - httpRqt->startTime.tv_sec) * 1000;

        // call request callback (note: callback should free httpRqt)
        httpRqtActionT status = httpRqt->callback(httpRqt);
        if (status == HTTP_HANDLE_FREE)
        {
            if (httpRqt->freeCtx && httpRqt->userData)
                httpRqt->freeCtx(httpRqt->userData);
            free(httpRqt);
        }

        // we're done
        curl_easy_cleanup(httpRqt->easy);
    }
    return 0;

OnErrorExit:
    free(httpRqt);
    return 1;
}

int httpSendPost(httpPoolT *httpPool, const char *url, const httpKeyValT *headers, httpKeyValT *tokens, httpOptsT *opts, void *datas, long len, httpRqtCbT callback, void *ctx, httpFreeCtxCbT freeCtx)
{
    return httpSendQuery(httpPool, url, headers, tokens, opts, datas, len, callback, ctx, freeCtx);
}

int httpSendGet(httpPoolT *httpPool, const char *url, const httpKeyValT *headers, httpKeyValT *tokens, httpOptsT *opts, httpRqtCbT callback, void *ctx, httpFreeCtxCbT freeCtx)
{
    return httpSendQuery(httpPool, url, headers, tokens, opts, NULL, 0, callback, ctx, freeCtx);
}

// create systemd source event and attach http processing callback to sock fd
static int multiSetSockCB(CURL *easy, int sock, int action, void *userdata, void *sockp)
{
    httpPoolT *httpPool = (httpPoolT *)userdata;
    assert(httpPool->magic == MAGIC_HTTP_POOL);

    if (httpPool->verbose > 1)
    {
        if (action == CURL_POLL_REMOVE)
            fprintf(stderr, "[multi-sock-remove] sock=%d (multiSetSockCB)\n", sock);
        else if (!sockp)
            fprintf(stderr, "[multi-sock-insert] sock=%d (multiSetSockCB)\n", sock);
    }
    int err = httpPool->callback->multiSocket(httpPool, easy, sock, action, sockp);
    if (err && action != CURL_POLL_REMOVE)
        fprintf(stderr, "[curl-source-attach-fail] curl_multi_assign failed (evtSetSocketCB)");

    return err;
}

static int multiSetTimerCB(CURLM *curl, long timeout, void *ctx)
{
    httpPoolT *httpPool = (httpPoolT *)ctx;
    assert(httpPool->magic == MAGIC_HTTP_POOL);

    if (httpPool->verbose > 1)
        fprintf(stderr, "-- multiSetTimerCB timeout=%ld\n", timeout);
    int err = httpPool->callback->multiTimer(httpPool, timeout);
    if (err)
        fprintf(stderr, "[afb-timer-fail] afb_sched_post_job fail error=%d (multiSetTimerCB)", err);

    return err;
}

// Create CURL multi httpPool and attach it to systemd evtLoop
httpPoolT *httpCreatePool(void *evtLoop, httpCallbacksT *mainLoopCbs, int verbose)
{

    // First call initialise global CURL static data
    static int initialised = 0;
    if (!initialised)
    {
        curl_global_init(CURL_GLOBAL_ALL);
        initialised = 1;
    }
    httpPoolT *httpPool;
    httpPool = calloc(1, sizeof(httpPoolT));
    httpPool->magic = MAGIC_HTTP_POOL;
    httpPool->verbose = verbose;
    httpPool->callback = mainLoopCbs;
    if (verbose)
        fprintf(stderr, "-- curl-multi asynchronous pool initialized\n");

    // add mainloop to httpPool
    httpPool->evtLoop = evtLoop;

    httpPool->multi = curl_multi_init();
    if (!httpPool->multi)
        goto OnErrorExit;

    curl_multi_setopt(httpPool->multi, CURLMOPT_SOCKETFUNCTION, multiSetSockCB);
    curl_multi_setopt(httpPool->multi, CURLMOPT_TIMERFUNCTION, multiSetTimerCB);
    curl_multi_setopt(httpPool->multi, CURLMOPT_SOCKETDATA, httpPool);
    curl_multi_setopt(httpPool->multi, CURLMOPT_TIMERDATA, httpPool);

    return httpPool;

OnErrorExit:
    fprintf(stderr, "[httpPool-create-fail] hoop curl_multi_init failed (httpCreatePool)");
    free(httpPool);
    return NULL;
}