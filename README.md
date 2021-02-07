Small interface to libcurl with systemd event mainloop

## Build & test
```
# install dependencies (libsystemd-devel &  libcurl-devel)
  make

# run
./curl-http -a https://example.com  # asynchronous
./curl-http -s https://example.com  # synchronous

```

## Simple C API for get/post operations.

Synchronous and asynchronous mode transparently supported. In both modes user callback receives on request completion a handler with status, headers, body and statistics.

* query: httpBuildQuery
* get: httpSendGet
* post: httpSendPost


## Check main.c for api sample
```
char urltarget[DFLT_URL_MAX_LEN]; // final url as compose by httpBuildQuery (should be big enough)

// headers + tokens uses the same syntax and are merge in httpSendGet
const httpHeadersT sampleHeaders[]= {
	{.tag="Content-type", .value="application/x-www-form-urlencoded"},
	{.tag="Accept", .value="application/json"},
	{NULL}  // terminator
};

// Url query needs to be build before sending the request with httpBuildQuery
httpQueryT query[]= {
	{.tag="client_id"    , .value=idp->credentials->clientId},
	{.tag="client_secret", .value=idp->credentials->secret},
	{.tag="code"         , .value=code},
	{.tag="redirect_uri" , .value=redirectUrl},
	{.tag="state"        , .value=afb_session_uuid(hreq->comreq.session)},

	{NULL} // terminator
};

// build your query and send your request
err= httpBuildQuery (idp->uid, urltarget, sizeof(url), urlPrefix, urlSource, query);
err= httpSendGet  (pool, urltarget, headers, tokens, opts, callback, (void*)context);
err= httpSendPost (pool, urltarget, headers, tokens, opts, (void*)databuf, datalen, callback, void*(context));
```