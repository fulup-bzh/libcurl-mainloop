Small interface to libcurl with systemd event mainloop

# Build

You need to select a supported mainloop library. As today libsystemd & libuv

### libsystemd
```
  dnf/zypper/apt install libsystemd-devel
  make MAIN_LOOP=systemd
```

### libuv
```
  dnf/zypper/apt install libuv-devel
  make MAIN_LOOP=libuv
```

### synchronous only (no gluelib/mainloop)
```
  make 
```

# Test
```
# asynchronous ./curl-http -v -a https://example.com https://example.com  
# synchronous  ./curl-http -v -s https://example.com https://example.com 

```

# Libcurl asynchronous C-API.

Synchronous/asynchronous transparently supported. In both modes user callback receives on request completion a handler with status, headers, body and statistics.

check curl-mail.c for a complete example.

## Api example
```
char urltarget[DFLT_URL_MAX_LEN]; // final url as compose by httpBuildQuery (should be big enough)

// headers + tokens uses the same syntax and are merge in httpSendGet
const httpKeyValT sampleHeaders[]= {
	{.tag="Content-type", .value="application/x-www-form-urlencoded"},
	{.tag="Accept", .value="application/json"},
	{NULL}  // terminator
};

// Url query needs to be build before sending the request with httpBuildQuery
httpKeyValT query[]= {
	{.tag="client_id"    , .value=idp->credentials->clientId},
	{.tag="client_secret", .value=idp->credentials->secret},
	{.tag="code"         , .value=code},
	{.tag="redirect_uri" , .value=redirectUrl},
	{.tag="state"        , .value=afb_session_uuid(hreq->comreq.session)},

	{NULL} // terminator
};

// build your query and send your request
err= httpBuildQuery (idp->uid, urltarget, sizeof(url), urlPrefix, urlSource, query);
err= httpSendGet  (pool, urltarget, headers, tokens, opts, callback, (void*)userData);
err= httpSendPost (pool, urltarget, headers, tokens, opts, (void*)databuf, datalen, callback, void*(userData));
```