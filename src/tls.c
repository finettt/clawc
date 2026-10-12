/* clawc - tls.c: minimal TLS 1.2+ client on top of OpenSSL (libssl/libcrypto).
   Only used when built with -DCLAWC_TLS; keeps the curl fallback unnecessary. */
#include "tls.h"

#ifdef CLAWC_TLS

#include "util.h"

#include <arpa/inet.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct TlsConn {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
};

static char *openssl_err(const char *what) {
    unsigned long e = ERR_get_error();
    char detail[256];
    if (e) ERR_error_string_n(e, detail, sizeof detail);
    else snprintf(detail, sizeof detail, "unknown error");
    Buf b;
    buf_init(&b);
    buf_printf(&b, "%s: %s", what, detail);
    return b.s;
}

static int is_ip_addr(const char *s) {
    unsigned char tmp[16];
    return inet_pton(AF_INET, s, tmp) == 1 || inet_pton(AF_INET6, s, tmp) == 1;
}

TlsConn *tls_wrap(int fd, const char *host, int insecure, char **err) {
    *err = NULL;
    TlsConn *c = xmalloc(sizeof *c);
    c->fd = fd;
    c->ctx = SSL_CTX_new(TLS_client_method());
    if (!c->ctx) {
        *err = openssl_err("SSL_CTX_new");
        free(c);
        return NULL;
    }
    SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION);
    SSL_CTX_set_default_verify_paths(c->ctx);
    SSL_CTX_set_verify(c->ctx, insecure ? SSL_VERIFY_NONE : SSL_VERIFY_PEER, NULL);

    c->ssl = SSL_new(c->ctx);
    if (!c->ssl) {
        *err = openssl_err("SSL_new");
        tls_close(c);
        return NULL;
    }
    SSL_set_fd(c->ssl, fd);
    /* SNI: RFC 6066 forbids IP literals, only send for hostnames */
    if (!is_ip_addr(host))
        SSL_set_tlsext_host_name(c->ssl, host);
    if (!insecure) {
        X509_VERIFY_PARAM *param = SSL_get0_param(c->ssl);
        if (is_ip_addr(host)) {
            if (!X509_VERIFY_PARAM_set1_ip_asc(param, host)) {
                *err = openssl_err("set1_ip_asc");
                tls_close(c);
                return NULL;
            }
        } else {
            X509_VERIFY_PARAM_set1_host(param, host, 0);
        }
    }

    if (SSL_connect(c->ssl) != 1) {
        int se = SSL_get_error(c->ssl, -1);
        if (se == SSL_ERROR_SSL && !insecure) {
            long vr = SSL_get_verify_result(c->ssl);
            if (vr != X509_V_OK) {
                Buf b;
                buf_init(&b);
                buf_printf(&b, "TLS verify failed: %s",
                           X509_verify_cert_error_string(vr));
                *err = b.s;
                tls_close(c);
                return NULL;
            }
        }
        *err = openssl_err("TLS handshake");
        tls_close(c);
        return NULL;
    }
    if (!insecure && SSL_get_verify_result(c->ssl) != X509_V_OK) {
        *err = xstrdup("TLS verify failed");
        tls_close(c);
        return NULL;
    }
    return c;
}

int tls_send(TlsConn *c, const void *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        int r = SSL_write(c->ssl, (const char *)buf + off, (int)(n - off));
        if (r <= 0) {
            int e = SSL_get_error(c->ssl, r);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

ssize_t tls_recv(TlsConn *c, void *buf, size_t n, int timeout_s) {
    time_t deadline = time(NULL) + (timeout_s > 0 ? timeout_s : 60);
    for (;;) {
        if (SSL_pending(c->ssl) == 0) {
            time_t left = deadline - time(NULL);
            if (left <= 0) return -2;
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(c->fd, &rfds);
            struct timeval tv;
            tv.tv_sec = left;
            tv.tv_usec = 0;
            int r = select(c->fd + 1, &rfds, NULL, NULL, &tv);
            if (r == 0) return -2;
            if (r < 0) return -1;
        }
        int r = SSL_read(c->ssl, buf, (int)n);
        if (r > 0) return r;
        int e = SSL_get_error(c->ssl, r);
        if (e == SSL_ERROR_ZERO_RETURN) return 0;          /* clean close */
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
        return -1;
    }
}

void tls_close(TlsConn *c) {
    if (!c) return;
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
    }
    if (c->ctx) SSL_CTX_free(c->ctx);
    if (c->fd >= 0) close(c->fd);
    free(c);
}

#else /* !CLAWC_TLS */

/* Stubs so ws.c links without OpenSSL. wss:// fails with a clear error;
   plain ws:// never wraps and works fine. */
#include "util.h"

#include <stdlib.h>

TlsConn *tls_wrap(int fd, const char *host, int insecure, char **err) {
    (void)fd;
    (void)host;
    (void)insecure;
    if (err) *err = xstrdup("TLS not compiled in (build with CLAWC_TLS)");
    return NULL;
}

int tls_send(TlsConn *c, const void *buf, size_t n) {
    (void)c;
    (void)buf;
    (void)n;
    return -1;
}

ssize_t tls_recv(TlsConn *c, void *buf, size_t n, int timeout_s) {
    (void)c;
    (void)buf;
    (void)n;
    (void)timeout_s;
    return -1;
}

void tls_close(TlsConn *c) {
    (void)c;
}

#endif /* CLAWC_TLS */
