/* Encrypted transport for the authenticated bridge. No implicit fallback. */
#define _POSIX_C_SOURCE 200809L
#include "bridge_tls.h"
#include "util.h"
#include <arpa/inet.h>
#include <netinet/in.h>

int bridge_loopback(const struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)sa;
        return (ntohl(v4->sin_addr.s_addr) >> 24) == 127;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)sa;
        return IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr);
    }
    return 0;
}

#if defined(CLAWC_TLS)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <time.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

struct BridgeTlsCtx { SSL_CTX *ctx; };
struct BridgeTls { SSL *ssl; SSL_CTX *owned_ctx; int fd; };

static int wait_ssl(BridgeTls *c, int result, long long end) {
    int e = SSL_get_error(c->ssl, result);
    short events = e == SSL_ERROR_WANT_READ ? POLLIN : e == SSL_ERROR_WANT_WRITE ? POLLOUT : 0;
    if (!events) return -1;
    for (;;) {
        long long now = now_ms(), left = end - now;
        if (now < 0 || left <= 0) return -1;
        struct pollfd p = {c->fd, events, 0};
        int rc = poll(&p, 1, (int)left);
        if (rc > 0) return 0;
        if (rc == 0 || errno != EINTR) return -1;
    }
}
static int nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}
static BridgeTls *new_conn(SSL_CTX *ctx, int fd) {
    BridgeTls *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd = fd;
    c->ssl = SSL_new(ctx);
    if (!c->ssl || !SSL_set_fd(c->ssl, fd) || nonblock(fd) != 0) {
        bridge_tls_free(c);
        return NULL;
    }
    return c;
}
static int handshake(BridgeTls *c, int server, int seconds) {
    long long end = now_ms() + (long long)seconds * 1000;
    for (;;) {
        int r = server ? SSL_accept(c->ssl) : SSL_connect(c->ssl);
        if (r == 1) return 0;
        if (wait_ssl(c, r, end) != 0) return -1;
    }
}
BridgeTlsCtx *bridge_tls_server_new(const char *cert, const char *key) {
    if (!cert || !cert[0] || !key || !key[0]) return NULL;
    BridgeTlsCtx *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->ctx = SSL_CTX_new(TLS_server_method());
    if (!s->ctx || !SSL_CTX_set_min_proto_version(s->ctx, TLS1_2_VERSION) ||
        SSL_CTX_use_certificate_chain_file(s->ctx, cert) != 1 ||
        SSL_CTX_use_PrivateKey_file(s->ctx, key, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(s->ctx) != 1) {
        bridge_tls_server_free(s);
        return NULL;
    }
    return s;
}
void bridge_tls_server_free(BridgeTlsCtx *s) {
    if (s) { SSL_CTX_free(s->ctx); free(s); }
}
BridgeTls *bridge_tls_accept(BridgeTlsCtx *s, int fd, int seconds) {
    BridgeTls *c = new_conn(s->ctx, fd);
    if (!c) return NULL;
    if (handshake(c, 1, seconds) != 0) { bridge_tls_free(c); return NULL; }
    return c;
}
BridgeTls *bridge_tls_connect_opts(int fd, const char *host, const char *ca, int seconds, int insecure) {
    unsigned char ip[16];
    int is_ip = inet_pton(AF_INET, host, ip) == 1 || inet_pton(AF_INET6, host, ip) == 1;
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;
    if (!SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) ||
        (!insecure && (ca && ca[0] ? SSL_CTX_load_verify_locations(ctx, ca, NULL) != 1
                     : SSL_CTX_set_default_verify_paths(ctx) != 1))) {
        SSL_CTX_free(ctx); return NULL;
    }
    SSL_CTX_set_verify(ctx, insecure ? SSL_VERIFY_NONE : SSL_VERIFY_PEER, NULL);
    BridgeTls *c = new_conn(ctx, fd);
    if (!c) { SSL_CTX_free(ctx); return NULL; }
    c->owned_ctx = ctx;
    X509_VERIFY_PARAM *param = SSL_get0_param(c->ssl);
    if ((!insecure && (is_ip ? X509_VERIFY_PARAM_set1_ip_asc(param, host)
               : X509_VERIFY_PARAM_set1_host(param, host, 0)) != 1) ||
        (!is_ip && SSL_set_tlsext_host_name(c->ssl, host) != 1) ||
        handshake(c, 0, seconds) != 0 || (!insecure && SSL_get_verify_result(c->ssl) != X509_V_OK)) {
        bridge_tls_free(c); return NULL;
    }
    return c;
}
BridgeTls *bridge_tls_connect(int fd, const char *host, const char *ca, int seconds) {
    return bridge_tls_connect_opts(fd, host, ca, seconds, 0);
}
ssize_t bridge_tls_read(BridgeTls *c, void *buf, size_t n, int seconds) {
    long long end = now_ms() + (long long)seconds * 1000;
    for (;;) {
        int r = SSL_read(c->ssl, buf, (int)n);
        if (r > 0) return r;
        if (SSL_get_error(c->ssl, r) == SSL_ERROR_ZERO_RETURN) return 0;
        if (wait_ssl(c, r, end) != 0) return -1;
    }
}
int bridge_tls_write(BridgeTls *c, const void *buf, size_t n, int seconds) {
    long long end = now_ms() + (long long)seconds * 1000;
    size_t pos = 0;
    while (pos < n) {
        int r = SSL_write(c->ssl, (const char *)buf + pos, (int)(n - pos));
        if (r > 0) pos += (size_t)r;
        else if (wait_ssl(c, r, end) != 0) return -1;
    }
    return 0;
}
void bridge_tls_free(BridgeTls *c) {
    if (c) { SSL_free(c->ssl); SSL_CTX_free(c->owned_ctx); free(c); }
}
#else
BridgeTlsCtx *bridge_tls_server_new(const char *cert, const char *key) { (void)cert; (void)key; return 0; }
void bridge_tls_server_free(BridgeTlsCtx *s) { (void)s; }
BridgeTls *bridge_tls_accept(BridgeTlsCtx *s, int fd, int seconds) { (void)s; (void)fd; (void)seconds; return 0; }
BridgeTls *bridge_tls_connect(int fd, const char *host, const char *ca, int seconds) { (void)fd; (void)host; (void)ca; (void)seconds; return 0; }
BridgeTls *bridge_tls_connect_opts(int fd, const char *host, const char *ca, int seconds, int insecure) { (void)fd; (void)host; (void)ca; (void)seconds; (void)insecure; return 0; }
ssize_t bridge_tls_read(BridgeTls *c, void *b, size_t n, int seconds) { (void)c; (void)b; (void)n; (void)seconds; return -1; }
int bridge_tls_write(BridgeTls *c, const void *b, size_t n, int seconds) { (void)c; (void)b; (void)n; (void)seconds; return -1; }
void bridge_tls_free(BridgeTls *c) { (void)c; }
#endif
