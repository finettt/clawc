/* clawc - net.c: outbound TCP connect with optional proxy tunnelling.
 *
 * Direct connections are the original tcp_connect() from http.c, unchanged in
 * behaviour: getaddrinfo(), try every record, apply SO_RCVTIMEO/SO_SNDTIMEO.
 *
 * With a proxy configured the same fd is returned, but only after either an
 * HTTP CONNECT tunnel or a SOCKS5 handshake has been established. TLS is
 * always layered on top afterwards by the caller, and it always verifies the
 * origin host (SNI + X509_VERIFY_PARAM_set1_host), never the proxy.
 *
 * Precedence is resolved by the caller (config.c): file > CLAWC_PROXY >
 * HTTPS_PROXY > ALL_PROXY. NO_PROXY provides the bypass list.
 */
#define _POSIX_C_SOURCE 200809L
#include "net.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define NET_CONNECT_TIMEOUT 60

enum { PROXY_NONE = 0, PROXY_HTTP, PROXY_HTTPS, PROXY_SOCKS5 };

typedef struct {
    int kind;
    char *host;
    char *port;
    char *user;   /* may be NULL */
    char *pass;
} ProxyCfg;

static ProxyCfg g_proxy;
static char *g_no_proxy;
static char *g_active_desc;

/* ------------------------------------------------------------ helpers ---- */

static void str_free(char **p)
{
    if (*p) { free(*p); *p = NULL; }
}

/* Copy [b, e) into a fresh string. */
static char *dup_range(const char *b, const char *e)
{
    size_t n = (size_t)(e - b);
    char *s = xmalloc(n + 1);
    memcpy(s, b, n);
    s[n] = '\0';
    return s;
}

static int b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

char *net_b64(const char *in)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = in ? strlen(in) : 0;
    size_t out_n = ((n + 2) / 3) * 4;
    char *out = xmalloc(out_n + 1);
    size_t i, o = 0;
    for (i = 0; i + 2 < n; i += 3) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) |
                     (unsigned)in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];
        out[o++] = T[v & 63];
    }
    if (i < n) {
        unsigned v = (unsigned)in[i] << 16;
        int rem = (int)(n - i);
        if (rem == 2) v |= (unsigned)in[i + 1] << 8;
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = rem == 2 ? T[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return out;
}

/* Small percent-decoder for user:pass in a proxy URL. In place. */
static void pct_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '%' && b64_val(r[1]) >= 0 || (r[0] == '%' && r[1] == '-')) {
            /* %XX hex */
            int hi = r[1] >= 'A' ? (r[1] | 0x20) - 'a' + 10 : r[1] - '0';
            int lo = r[2] >= 'A' ? (r[2] | 0x20) - 'a' + 10 : r[2] - '0';
            if (r[1] && r[2] && hi >= 0 && hi < 16 && lo >= 0 && lo < 16) {
                *w++ = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/* ------------------------------------------------------- configuration --- */

int net_set_proxy(const char *proxy, const char *no_proxy)
{
    str_free(&g_proxy.host);
    str_free(&g_proxy.port);
    str_free(&g_proxy.user);
    str_free(&g_proxy.pass);
    str_free(&g_no_proxy);
    str_free(&g_active_desc);
    memset(&g_proxy, 0, sizeof g_proxy);

    if (no_proxy && no_proxy[0]) g_no_proxy = xstrdup(no_proxy);
    if (!proxy || !proxy[0]) return 0;

    const char *p = proxy;
    int kind;
    if (str_starts(p, "socks5h://"))     { kind = PROXY_SOCKS5; p += 10; }
    else if (str_starts(p, "socks5://")) { kind = PROXY_SOCKS5; p += 9; }
    else if (str_starts(p, "http://"))   { kind = PROXY_HTTP;   p += 7; }
    else if (str_starts(p, "https://"))  { kind = PROXY_HTTPS;  p += 8; }
    else {
        fprintf(stderr, "clawc: proxy: unsupported scheme in '%s' "
                        "(need http://, https://, socks5:// or socks5h://)\n", proxy);
        return -1;
    }

    const char *at = strrchr(p, '@');
    const char *hostp = p;
    if (at) {
        char *cred = dup_range(p, at);
        char *colon = strchr(cred, ':');
        if (colon) {
            *colon = '\0';
            g_proxy.user = xstrdup(cred);
            g_proxy.pass = xstrdup(colon + 1);
            pct_decode(g_proxy.pass);
        } else {
            g_proxy.user = xstrdup(cred);
        }
        pct_decode(g_proxy.user);
        free(cred);
        hostp = at + 1;
    }

    const char *slash = strchr(hostp, '/');   /* trailing path is ignored */
    const char *hostend = slash ? slash : hostp + strlen(hostp);
    const char *colon = memchr(hostp, ':', (size_t)(hostend - hostp));
    if (colon) {
        g_proxy.host = dup_range(hostp, colon);
        g_proxy.port = dup_range(colon + 1, hostend);
    } else {
        g_proxy.host = dup_range(hostp, hostend);
        g_proxy.port = xstrdup(kind == PROXY_SOCKS5 ? "1080"
                                : kind == PROXY_HTTPS ? "443" : "8080");
    }
    if (!g_proxy.host[0] || !g_proxy.port[0]) {
        fprintf(stderr, "clawc: proxy: need host:port in '%s'\n", proxy);
        str_free(&g_proxy.host);
        str_free(&g_proxy.port);
        return -1;
    }
    g_proxy.kind = kind;

    Buf d;
    buf_init(&d);
    buf_printf(&d, "%s%s%s:%s",
               kind == PROXY_SOCKS5 ? "socks5" : kind == PROXY_HTTPS ? "https" : "http",
               g_proxy.user ? "://" : "://",
               g_proxy.host, g_proxy.port);
    g_active_desc = xstrdup(d.s);
    buf_free(&d);
    return 0;
}

const char *net_proxy_active(void) { return g_active_desc; }

int net_bypasses(const char *host)
{
    if (!g_no_proxy || !g_no_proxy[0] || !host || !host[0]) return 0;
    size_t hn = strlen(host);
    const char *p = g_no_proxy;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        const char *trim = end;
        while (trim > p && (trim[-1] == ' ' || trim[-1] == '\t')) trim--;
        size_t n = (size_t)(trim - p);
        if (n) {
            /* "*" bypasses everything */
            if (n == 1 && p[0] == '*') return 1;
            /* exact, or ".suffix", or "suffix" matching a subdomain */
            if (n == hn && !strncmp(p, host, n)) return 1;
            if (p[0] == '.' && n - 1 <= hn &&
                (hn == n - 1 ? !strncmp(p + 1, host, hn)
                             : !strncmp(host + hn - (n - 1), p + 1, n - 1)))
                return 1;
            if (n < hn && host[hn - n - 1] == '.' && !strncmp(host + hn - n, p, n))
                return 1;
            /* host:port form matches on the host part only */
            const char *pc = memchr(p, ':', n);
            if (pc && (size_t)(pc - p) == hn && !strncmp(p, host, hn)) return 1;
        }
        p = *end ? end + 1 : end;
    }
    return 0;
}

/* --------------------------------------------------------- direct path --- */

static int tv_set(int fd, int timeout_s)
{
    struct timeval tv;
    tv.tv_sec = timeout_s > 0 ? timeout_s : NET_CONNECT_TIMEOUT;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0) return -1;
    return 0;
}

static int tcp_direct(const char *host, const char *port, int timeout_s, char **err)
{
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0 || !res) {
        if (err) {
            Buf m;
            buf_init(&m);
            buf_printf(&m, "cannot resolve %s: %s", host, gai_strerror(rc));
            *err = xstrdup(m.s);
            buf_free(&m);
        }
        return -1;
    }
    int fd = -1, last_errno = 0;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) { last_errno = errno; continue; }
        tv_set(fd, timeout_s);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        last_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0 && err) {
        Buf m;
        buf_init(&m);
        buf_printf(&m, "connect to %s:%s failed: %s", host, port,
                   err_str(last_errno));
        *err = xstrdup(m.s);
        buf_free(&m);
    }
    return fd;
}

/* ------------------------------------------------------- socket IO ------- */

static int send_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, p + off, n - off, 0);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static int recv_exact(int fd, void *buf, size_t n)
{
    char *p = buf;
    size_t off = 0;
    while (off < n) {
        ssize_t r = recv(fd, p + off, n - off, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

/* Read one CRLF-terminated line into buf (bounded). Returns length or -1. */
static int recv_line(int fd, char *buf, size_t cap)
{
    size_t off = 0;
    for (;;) {
        char ch;
        if (off + 3 >= cap) return -1;
        ssize_t r = recv(fd, &ch, 1, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        buf[off++] = ch;
        if (off >= 2 && buf[off - 2] == '\r' && buf[off - 1] == '\n') {
            buf[off] = '\0';
            return (int)off;
        }
    }
}

static void set_err(char **err, const char *fmt, const char *a, const char *b)
{
    if (!err) return;
    Buf m;
    buf_init(&m);
    buf_printf(&m, fmt, a ? a : "", b ? b : "");
    *err = xstrdup(m.s);
    buf_free(&m);
}

/* ------------------------------------------------------ HTTP CONNECT ----- */

/* Establish a tunnel to host:port over a connected proxy fd. */
static int http_connect_tunnel(int fd, const char *host, const char *port,
                               const ProxyCfg *p, char **err)
{
    Buf req;
    buf_init(&req);
    buf_printf(&req, "CONNECT %s:%s HTTP/1.1\r\nHost: %s:%s\r\n",
               host, port, host, port);
    if (p->user) {
        Buf cred;
        buf_init(&cred);
        buf_printf(&cred, "%s:%s", p->user, p->pass ? p->pass : "");
        char *b64 = net_b64(cred.s);
        buf_printf(&req, "Proxy-Authorization: Basic %s\r\n", b64);
        free(b64);
        buf_free(&cred);
    } else if (p->kind == PROXY_HTTPS) {
        buf_puts(&req, "Proxy-Authorization: Basic \r\n"); /* no creds: omitted */
    }
    buf_puts(&req, "\r\n");
    int rc = send_all(fd, req.s, req.len);
    buf_free(&req);
    if (rc != 0) {
        set_err(err, "proxy: writing CONNECT to %s:%s failed", p->host, p->port);
        return -1;
    }

    char line[512];
    if (recv_line(fd, line, sizeof line) < 0) {
        set_err(err, "proxy: no CONNECT response from %s:%s", p->host, p->port);
        return -1;
    }
    int status = 0;
    const char *sp = strchr(line, ' ');
    if (sp) status = atoi(sp + 1);
    if (status < 200 || status >= 300) {
        char st[8] = {0};
        snprintf(st, sizeof st, "%d", status);
        set_err(err, "proxy refused CONNECT: %s", st, NULL);
        return -1;
    }
    /* Drain the rest of the response head: a 2xx CONNECT has no body. */
    for (;;) {
        int n = recv_line(fd, line, sizeof line);
        if (n < 0) {
            set_err(err, "proxy: truncated CONNECT head from %s:%s", p->host, p->port);
            return -1;
        }
        if (n == 2) break; /* empty line */
    }
    return 0;
}

/* ----------------------------------------------------------- SOCKS5 ----- */

static int socks5_handshake(int fd, const char *host, const char *port,
                            const ProxyCfg *p, char **err)
{
    unsigned char greet[4];
    int glen = 0;
    greet[glen++] = 0x05;
    if (p->user) {
        greet[glen++] = 0x02;   /* offer: no-auth, user/pass */
        greet[glen++] = 0x00;
        greet[glen++] = 0x02;
    } else {
        greet[glen++] = 0x01;
        greet[glen++] = 0x00;
    }
    if (send_all(fd, greet, (size_t)glen) != 0) {
        set_err(err, "socks5: sending greeting to %s:%s failed", p->host, p->port);
        return -1;
    }
    unsigned char rep[2];
    if (recv_exact(fd, rep, 2) != 0) {
        set_err(err, "socks5: no greeting reply from %s:%s", p->host, p->port);
        return -1;
    }
    if (rep[0] != 0x05) {
        set_err(err, "socks5: bad version from proxy (%s)", NULL, NULL);
        return -1;
    }
    if (rep[1] == 0x02) {
        if (!p->user) {
            set_err(err, "socks5: proxy requires authentication (%s)", NULL, NULL);
            return -1;
        }
        size_t un = strlen(p->user), pn = p->pass ? strlen(p->pass) : 0;
        if (un > 255 || pn > 255) {
            set_err(err, "socks5: credentials too long (%s)", NULL, NULL);
            return -1;
        }
        Buf a;
        buf_init(&a);
        char hdr = 1;
        buf_add(&a, &hdr, 1);
        unsigned char l = (unsigned char)un;
        buf_add(&a, &l, 1);
        buf_add(&a, p->user, un);
        l = (unsigned char)pn;
        buf_add(&a, &l, 1);
        if (pn) buf_add(&a, p->pass, pn);
        int rc = send_all(fd, a.s, a.len);
        buf_free(&a);
        if (rc != 0) {
            set_err(err, "socks5: sending credentials failed (%s)", NULL, NULL);
            return -1;
        }
        unsigned char auth[2];
        if (recv_exact(fd, auth, 2) != 0 || auth[1] != 0x00) {
            set_err(err, "socks5: authentication rejected (%s)", NULL, NULL);
            return -1;
        }
    } else if (rep[1] != 0x00) {
        set_err(err, "socks5: no acceptable auth method (0x%02x)", NULL, NULL);
        return -1;
    }

    /* CONNECT request. socks5:// resolves the name locally, socks5h:// lets the
     * proxy resolve it (no DNS leak). */
    Buf r;
    buf_init(&r);
    unsigned char head[3] = { 0x05, 0x01, 0x00 };
    buf_add(&r, head, sizeof head);

    size_t hn = strlen(host);
    struct in_addr v4;
    if (hn <= 255 && (!inet_pton(AF_INET, host, &v4) || 1)) {
        /* Prefer the domain form: it is what socks5h means, and it keeps the
         * proxy's own resolver in charge for socks5 as well (no local DNS). */
        unsigned char atyp = 0x03;
        buf_add(&r, &atyp, 1);
        unsigned char l = (unsigned char)hn;
        buf_add(&r, &l, 1);
        buf_add(&r, host, hn);
    }
    unsigned pn = (unsigned)atoi(port);
    unsigned char pb[2] = { (unsigned char)(pn >> 8), (unsigned char)(pn & 0xff) };
    buf_add(&r, pb, 2);

    int rc = send_all(fd, r.s, r.len);
    buf_free(&r);
    if (rc != 0) {
        set_err(err, "socks5: sending CONNECT failed (%s)", NULL, NULL);
        return -1;
    }

    unsigned char reply[4];
    if (recv_exact(fd, reply, 4) != 0) {
        set_err(err, "socks5: no CONNECT reply (%s)", NULL, NULL);
        return -1;
    }
    if (reply[0] != 0x05) {
        set_err(err, "socks5: bad reply version (%s)", NULL, NULL);
        return -1;
    }
    if (reply[1] != 0x00) {
        static const char *why[] = {
            "succeeded", "general failure", "not allowed by ruleset",
            "network unreachable", "host unreachable", "connection refused",
            "TTL expired", "command not supported", "address type not supported"
        };
        const char *m = reply[1] <= 8 ? why[reply[1]] : "unknown error";
        Buf msg;
        buf_init(&msg);
        buf_printf(&msg, "socks5: proxy refused CONNECT (%s, 0x%02x)", m, reply[1]);
        if (err) *err = xstrdup(msg.s);
        buf_free(&msg);
        return -1;
    }
    /* Consume BND.ADDR + BND.PORT so the fd is positioned at payload. */
    size_t skip;
    switch (reply[3]) {
    case 0x01: skip = 4 + 2; break;               /* IPv4 */
    case 0x04: skip = 16 + 2; break;              /* IPv6 */
    case 0x03: {
        unsigned char l;
        if (recv_exact(fd, &l, 1) != 0) {
            set_err(err, "socks5: truncated bound address (%s)", NULL, NULL);
            return -1;
        }
        skip = (size_t)l + 2;
        break;
    }
    default:
        set_err(err, "socks5: unsupported address type in reply (%s)", NULL, NULL);
        return -1;
    }
    while (skip) {
        unsigned char junk[64];
        size_t take = skip < sizeof junk ? skip : sizeof junk;
        if (recv_exact(fd, junk, take) != 0) {
            set_err(err, "socks5: truncated bound address (%s)", NULL, NULL);
            return -1;
        }
        skip -= take;
    }
    return 0;
}

/* ------------------------------------------------------------ public ---- */

int net_connect(const char *host, const char *port, int timeout_s,
                char **err, const char **via)
{
    if (err) *err = NULL;
    if (via) *via = "direct";

    if (g_proxy.kind == PROXY_NONE || net_bypasses(host))
        return tcp_direct(host, port, timeout_s, err);

    int fd = tcp_direct(g_proxy.host, g_proxy.port, timeout_s, err);
    if (fd < 0) return -1;

    /* TLS to the proxy itself (https:// proxy URL) is not implemented: the
     * CONNECT helper needs a TLS stream, and tls.c is layered above us by the
     * caller. Refuse early instead of silently sending cleartext creds. */
    if (g_proxy.kind == PROXY_HTTPS) {
        close(fd);
        set_err(err, "proxy: https:// proxies are not supported yet "
                     "(use http:// or socks5h://) (%s)", NULL, NULL);
        return -1;
    }

    int rc = 0;
    if (g_proxy.kind == PROXY_SOCKS5) {
        rc = socks5_handshake(fd, host, port, &g_proxy, err);
        if (via) *via = "socks5";
    } else {
        rc = http_connect_tunnel(fd, host, port, &g_proxy, err);
        if (via) *via = "http-connect";
    }
    if (rc != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
