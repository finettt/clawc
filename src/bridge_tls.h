/* Bridge TLS uses OpenSSL when enabled. A successful wrapper borrows fd;
 * callers must free the wrapper and close fd. All I/O has an absolute timeout. */
#ifndef CLAWC_BRIDGE_TLS_H
#define CLAWC_BRIDGE_TLS_H
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
typedef struct BridgeTlsCtx BridgeTlsCtx;
typedef struct BridgeTls BridgeTls;
BridgeTlsCtx *bridge_tls_server_new(const char *cert, const char *key);
void bridge_tls_server_free(BridgeTlsCtx *ctx);
BridgeTls *bridge_tls_accept(BridgeTlsCtx *ctx, int fd, int seconds);
BridgeTls *bridge_tls_connect(int fd, const char *host, const char *ca, int seconds);
BridgeTls *bridge_tls_connect_opts(int fd, const char *host, const char *ca, int seconds, int insecure);
ssize_t bridge_tls_read(BridgeTls *c, void *buf, size_t n, int seconds);
int bridge_tls_write(BridgeTls *c, const void *buf, size_t n, int seconds);
void bridge_tls_free(BridgeTls *c);
int bridge_loopback(const struct sockaddr *sa);
#endif
