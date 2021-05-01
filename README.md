Small interface to libcurl with systemd event mainloop

# Build

You need to select a supported mainloop library. As today libsystemd & libuv & epoll/timerfd

### epoll/timerfd (default)
```
  # epoll/timerfd is Linux built-in but also Linux specific
  make
```

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
# asynchronous ./http-client -v -a https://example.com https://example.com
# synchronous  ./http-client -v -s https://example.com https://example.com

```
# LDAP
Public readonly server see [forumsystems](https://www.forumsys.com/tutorials/integration-how-to/ldap/online-ldap-test-server/)

```bash
./build/http-client -v -a "ldap://ldap.forumsys.com/ou=scientists,dc=example,dc=com?uniqueMember"
```

Most enterprise LDAP require some form of authentication and run under private/protected uri. The request should obviously match your enterprise schema. Here after a typical example to retrieve groups $USER is member of.
```bash
export USER='xxxxx'
export PASSWD='yyyy'
export LDAPHOST='ldap.homename'

./build/http-client -v -a -u "uid=$USER,ou=People,dc=vannes,dc=iot,dc=bzh" -p "$PASSWD" "ldap://$LDAPHOST/ou=Groups,dc=vannes,dc=iot?dn?sub?(memberUid=$USER)"
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