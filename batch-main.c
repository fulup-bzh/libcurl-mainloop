/*
 * Copyright (C) 2021 "IoT.bzh"
 * Author "Fulup Ar Foll" <fulup@iot.bzh>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at https://opensource.org/licenses/MIT.
 * $RP_END_LICENSE$
 *
 */

#define _GNU_SOURCE

#include "http-client.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

// default mainloop timeout 1s
#ifndef LOOP_WAIT_SEC
#define LOOP_WAIT_SEC 1
#endif

static int count = 0; // global pending http request

typedef struct
{
    int uid;
    char *url;
} reqCtxT;

// The URL callback in your application where users are sent after authorization.
static httpRqtActionT sampleCallback(httpRqtT *httpRqt)
{
    assert(httpRqt->magic == MAGIC_HTTP_RQT);
    reqCtxT *ctxRqt = (reqCtxT *)httpRqt->userData;

    if (httpRqt->status < 0)  goto OnErrorExit;

    double seconds = (double)httpRqt->msTime / 1000.0;
    // fprintf(stdout, "\n[body]=%s", httpRqt->body);
    fprintf(stderr, "[request-ok] reqId=%d elapsed=%2.2fs url=%s\n", ctxRqt->uid, seconds, ctxRqt->url);
    count--;
    free (ctxRqt->url);
    return HTTP_HANDLE_FREE;

OnErrorExit:
    fprintf(stderr, "[request-error] status=%ld length=%ld", httpRqt->status, httpRqt->length);
    count--;
    return HTTP_HANDLE_FREE;
}

typedef enum
{
    MOD_SYNC,
    MOD_ASYNC,
    MOD_DEFAULT,
} runModT;

int main(int argc, char *argv[])
{
    const char *url;
    struct timespec startTime, stopTime;
    httpPoolT *httpPool = NULL;
    int err, start, verbose = 0;
    runModT runmode = MOD_DEFAULT;
    httpCallbacksT *mainLoopCbs = NULL;
    long uid = 0;
    int maxjob=50, timeout=5;
    char *filename=NULL;
    FILE *fileFD=NULL;

    // default empty libcurl option
    httpOptsT curlOpts= {};

    if (argc <= 1)
    {
        fprintf(stderr, "[syntax-error] batch-client [-p max-jobs(50)] -f filename -vvv] \n");
        goto OnErrorExit;
    }

#ifndef GLUE_LOOP_ON
            runmode = MOD_ASYNC;
            fprintf (stderr, "[no-glue-lib] please recompile with 'make MAIN_LOOP=systemd|libuv'\n");
            goto OnErrorExit;
#endif

    // check for option and shift argv as needed
    for (start= 1; start < argc; start++)
    {
        if (!strcasecmp(argv[start], "-p")) {
            start ++;
            maxjob= atoi(argv[start]);
        };

        if (!strcasecmp(argv[start], "-f")) {
            start ++;
            filename= argv[start];
            fileFD= fopen(filename, "r");
        }

        if (!strcasecmp(argv[start], "-v"))
            verbose++;
        if (!strcasecmp(argv[start], "-vv"))
            verbose = +2;
        if (!strcasecmp(argv[start], "-vvv"))
            verbose = +3;
    }

    if (!filename || !fileFD) {
        fprintf (stderr, "No valid input file missing/invalid (-f filename)\n");
        goto OnErrorExit;
    }

    if (maxjob <= 0) {
        fprintf (stderr, "maxjob should be >0 (-p 50)\n");
        goto OnErrorExit;
    }

    if (timeout <= 0) {
        fprintf (stderr, "timeout should be >0 (-t 5)\n");
        goto OnErrorExit;
    }

    // retreive callback and mainloop from libuv/libsystemd glue interface
    mainLoopCbs = glueGetCbs();
    void *evtLoop = mainLoopCbs->evtMainLoop();
    if (!evtLoop) goto OnErrorExit;

    // create multi pool and attach systemd eventloop
    if (mainLoopCbs)
    {
        httpPool = httpCreatePool(evtLoop, mainLoopCbs, verbose);
        if (!httpPool)
        {
            fprintf(stderr, "[fail-create-pool] libcurl multi pool\n");
            goto OnErrorExit;
        }
    }

    // launch all or request in asynchronous mode.
    clock_gettime(CLOCK_MONOTONIC, &startTime);
    for (int idx=0;;)
    {
        if (count > maxjob) {
            const char indic[]="-|.*";
            fprintf(stderr, "%c idx=%d count=%d\r", indic[idx%4], idx, count);
            (void)httpPool->callback->evtRunLoop(httpPool, LOOP_WAIT_SEC);
            continue;
        }
        idx++;

        // read one request and exit at EOF
        char *request;
        size_t size=0;
        ssize_t len= getline (&request, &size, fileFD);
        if (len < 0) {
            fprintf (stderr, "EOF(%s)\n", filename);
            break;
        }
        request[len-1]='\0'; // remove \n

        // add a sample userData to http request
        reqCtxT *ctxRqt = calloc(1, sizeof(reqCtxT));
        ctxRqt->uid = uid++;
        ctxRqt->url = request;

        if (verbose)
            fprintf(stderr, "[request-sent] reqId=%d %s\n", ctxRqt->uid, ctxRqt->url);

        // basic get with no header, token, query or options
        err = httpSendGet(httpPool, ctxRqt->url, &curlOpts, NULL /*token*/, sampleCallback, (void *)ctxRqt);
        if (!err)
            count++;
        else
        {
            fprintf(stderr, "[request-fail] request url=%s\n", ctxRqt->url);
            goto OnErrorExit;
        }
    }

    // wait for all pending request to be finished
    int index=0;
    while (count)
    {

        // enter mainloop and ping stdout every xxx seconds if nothing happen
        (void)httpPool->callback->evtRunLoop(httpPool, LOOP_WAIT_SEC);
        if (verbose > 1)
            fprintf(stderr, "-- waiting %d pending request(s)\n", count);
        else {
            index++;
            const char indic[]="-|.*";
            fprintf(stderr, "%c rqt count=%d\r", indic[index%4], count);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &stopTime);
    uint64_t msElapsed = (stopTime.tv_nsec - startTime.tv_nsec) / 1000000 + (stopTime.tv_sec - startTime.tv_sec) * 1000;
    double seconds = (double)msElapsed / 1000.0;

    fprintf(stderr, "\n[request-done] total request count=%ld elapsed=%2.2fs (no more pending request)\n", uid, seconds);
    exit(0);

OnErrorExit:
    exit(1);
}