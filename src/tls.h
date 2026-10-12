/* clawc - tls.h: in-process TLS client (OpenSSL backend, compile with -DCLAWC_TLS).
   When not compiled in, http.c falls back to the curl(1) subprocess. */
#ifndef CLAWC_TLS_H
#define CLAWC_TLS_H

#include <stddef.h>
#include <sys/types.h>

typedef struct TlsConn TlsConn;

/* Wrap a connected TCP fd in TLS. Verifies certificate + hostname unless
   insecure is set. Returns NULL and malloc'd *err on failure. */
TlsConn *tls_wrap(int fd, const char *host, int insecure, char **err);

/* Send all n bytes. Returns 0 ok, -1 error. */
int tls_send(TlsConn *c, const void *buf, size_t n);

/* Receive up to n bytes. >0 data, 0 clean close, -1 error, -2 timeout. */
ssize_t tls_recv(TlsConn *c, void *buf, size_t n, int timeout_s);

void tls_close(TlsConn *c);

#endif
