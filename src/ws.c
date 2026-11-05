/* clawc - ws.c: WebSocket client. Frames only - no extensions, no subprotocols.
   Works over plain TCP (ws://) or our TLS layer (wss://). */
#define _XOPEN_SOURCE 700
#include "ws.h"
#include "http.h"

#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int io_send(WsConn *c, const void *b, size_t n) {
    if (c->tls) return tls_send(c->tls, b, n) == (int)n ? 0 : -1;
    size_t off = 0;
    while (off < n) {
        ssize_t r = send(c->fd, (const char *)b + off, n - off, 0);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

static int io_recv(WsConn *c, void *b, size_t n, int timeout_s) {
    if (c->tls) {
        ssize_t r = tls_recv(c->tls, b, n, timeout_s);
        return r > 0 ? (int)r : 0;
    }
    struct timeval tv = { timeout_s, 0 };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ssize_t r = recv(c->fd, b, n, 0);
    return r > 0 ? (int)r : 0;
}

static int io_all(WsConn *c, void *b, size_t n, int timeout_s) {
    size_t off = 0;
    while (off < n) {
        int r = io_recv(c, (char *)b + off, n - off, timeout_s);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

int ws_connect(WsConn *c, const char *url, int insecure, char **err) {
    memset(c, 0, sizeof *c);
    c->fd = -1;
    buf_init(&c->acc);
    int tls = strncmp(url, "wss://", 6) == 0;
    const char *p = url + (tls ? 6 : 5);
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    char host[256];
    int port = tls ? 443 : 80;
    const char *col = memchr(p, ':', (size_t)(hostend - p));
    if (col) {
        snprintf(host, sizeof host, "%.*s", (int)(col - p), p);
        port = atoi(col + 1);
    } else {
        snprintf(host, sizeof host, "%.*s", (int)(hostend - p), p);
    }
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        if (err) *err = xstrdup("cannot resolve host");
        return -1;
    }
    c->fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (c->fd < 0 || connect(c->fd, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        if (err) *err = xstrdup("connect failed");
        ws_close(c);
        return -1;
    }
    freeaddrinfo(res);

    if (tls) {
        c->tls = tls_wrap(c->fd, host, insecure, err);
        if (!c->tls) {
            ws_close(c);
            return -1;
        }
    }

    /* handshake */
    unsigned char key[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, key, 16) != 16) {
        if (fd >= 0) close(fd);
        if (err) *err = xstrdup("no randomness");
        ws_close(c);
        return -1;
    }
    close(fd);
    static const char B64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char b64[32];
    int i, o = 0;
    for (i = 0; i < 16; i += 3) {
        unsigned v = key[i] << 16;
        if (i + 1 < 16) v |= key[i + 1] << 8;
        if (i + 2 < 16) v |= key[i + 2];
        b64[o++] = B64[(v >> 18) & 63];
        b64[o++] = B64[(v >> 12) & 63];
        b64[o++] = (i + 1 < 16) ? B64[(v >> 6) & 63] : '=';
        b64[o++] = (i + 2 < 16) ? B64[v & 63] : '=';
    }
    b64[o] = 0;
    Buf req;
    buf_init(&req);
    buf_printf(&req,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n",
        slash ? slash : "/", host, b64);
    int rc = io_send(c, req.s, req.len);
    buf_free(&req);
    if (rc != 0) {
        if (err) *err = xstrdup("handshake send failed");
        ws_close(c);
        return -1;
    }
    /* read response headers */
    Buf h;
    buf_init(&h);
    char ch;
    while (h.len < 8192 && io_recv(c, &ch, 1, 15) == 1) {
        buf_add(&h, &ch, 1);
        if (h.len >= 4 && strcmp(h.s + h.len - 4, "\r\n\r\n") == 0) break;
    }
    int ok = strstr(h.s, "101") != NULL &&
             (strstr(h.s, "websocket") || strstr(h.s, "WebSocket"));
    buf_free(&h);
    if (!ok) {
        if (err) *err = xstrdup("websocket upgrade rejected");
        ws_close(c);
        return -1;
    }
    return 0;
}

int ws_send_text(WsConn *c, const char *s, size_t n) {
    unsigned char hdr[10];
    int hl = 0;
    hdr[0] = 0x81; /* FIN + text */
    if (n < 126) {
        hdr[1] = 0x80 | (unsigned char)n;
        hl = 2;
    } else if (n < 65536) {
        hdr[1] = 0x80 | 126;
        hdr[2] = (unsigned char)(n >> 8);
        hdr[3] = (unsigned char)n;
        hl = 4;
    } else {
        hdr[1] = 0x80 | 127;
        int k;
        for (k = 0; k < 8; k++) hdr[2 + k] = (unsigned char)((unsigned long long)n >> (56 - 8 * k));
        hl = 10;
    }
    unsigned char mask[4];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, mask, 4) != 4) {
        if (fd >= 0) close(fd);
        mask[0] = 0x12; mask[1] = 0x34; mask[2] = 0x56; mask[3] = 0x78;
    } else close(fd);
    memcpy(hdr + hl, mask, 4);
    if (io_send(c, hdr, (size_t)hl + 4) != 0) return -1;
    /* masked payload */
    char *m = xmalloc(n ? n : 1);
    size_t i;
    for (i = 0; i < n; i++) m[i] = s[i] ^ mask[i % 4];
    int rc = io_send(c, m, n);
    free(m);
    return rc;
}

int ws_recv_msg(WsConn *c, Buf *out, int timeout_s) {
    buf_free(out);
    buf_init(out);
    buf_free(&c->acc);
    buf_init(&c->acc);
    for (;;) {
        unsigned char hdr[2];
        if (io_all(c, hdr, 2, timeout_s) != 0) return -1;
        int fin = (hdr[0] & 0x80) != 0;
        int op = hdr[0] & 0x0f;
        int masked = (hdr[1] & 0x80) != 0;
        unsigned long long len = hdr[1] & 0x7f;
        if (len == 126) {
            unsigned char e[2];
            if (io_all(c, e, 2, timeout_s) != 0) return -1;
            len = ((unsigned long long)e[0] << 8) | e[1];
        } else if (len == 127) {
            unsigned char e[8];
            if (io_all(c, e, 8, timeout_s) != 0) return -1;
            len = 0;
            int k;
            for (k = 0; k < 8; k++) len = (len << 8) | e[k];
        }
        unsigned char mask[4] = {0, 0, 0, 0};
        if (masked && io_all(c, mask, 4, timeout_s) != 0) return -1;
        if (len > 4u << 20) return -1;
        char *pay = xmalloc((size_t)len + 1);
        if (len && io_all(c, pay, (size_t)len, timeout_s) != 0) {
            free(pay);
            return -1;
        }
        if (masked) {
            unsigned long long i;
            for (i = 0; i < len; i++) pay[i] ^= mask[i % 4];
        }
        if (op == 0x8) { /* close */
            free(pay);
            return 1;
        }
        if (op == 0x9) { /* ping -> pong */
            unsigned char ph[2] = { 0x8a, 0x80 };
            unsigned char m2[4] = { 1, 2, 3, 4 };
            io_send(c, ph, 2);
            io_send(c, m2, 4);
            unsigned char z[8];
            int k;
            for (k = 0; k < (int)len && k < 8; k++) z[k] = pay[k] ^ m2[k % 4];
            if (len) io_send(c, z, (size_t)(len < 8 ? len : 8));
            free(pay);
            continue;
        }
        if (op == 0xA) { /* pong */
            free(pay);
            continue;
        }
        buf_add(&c->acc, pay, (size_t)len);
        free(pay);
        if (fin) {
            buf_add(out, c->acc.s ? c->acc.s : "", c->acc.len);
            return 0;
        }
    }
}

void ws_close(WsConn *c) {
    if (c->tls) {
        tls_close(c->tls);
        c->tls = NULL;
    } else if (c->fd >= 0) {
        close(c->fd);
    }
    c->fd = -1;
    buf_free(&c->acc);
}
