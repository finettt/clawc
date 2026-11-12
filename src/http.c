/* clawc - http.c */
#define _XOPEN_SOURCE 700
#include "http.h"
#include "net.h"
#include "tls.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define HTTP_MAX_BODY (8 * 1024 * 1024)

void http_resp_free(HttpResp *r) {
    if (!r) return;
    buf_free(&r->body);
    free(r->err);
    free(r);
}

static HttpResp *resp_err(const char *msg) {
    HttpResp *r = xmalloc(sizeof *r);
    memset(r, 0, sizeof *r);
    buf_init(&r->body);
    r->status = -1;
    r->err = xstrdup(msg);
    return r;
}

static int parse_url(const char *url, char **scheme, char **host, char **port, char **path) {
    const char *p = strstr(url, "://");
    if (!p) return -1;
    *scheme = xmalloc((size_t)(p - url) + 1);
    memcpy(*scheme, url, (size_t)(p - url));
    (*scheme)[p - url] = 0;
    const char *h = p + 3;
    const char *slash = strchr(h, '/');
    const char *hostend = slash ? slash : h + strlen(h);
    const char *colon = memchr(h, ':', (size_t)(hostend - h));
    if (colon) {
        *host = xmalloc((size_t)(colon - h) + 1);
        memcpy(*host, h, (size_t)(colon - h));
        (*host)[colon - h] = 0;
        *port = xmalloc((size_t)(hostend - colon));
        memcpy(*port, colon + 1, (size_t)(hostend - colon - 1));
        (*port)[hostend - colon - 1] = 0;
    } else {
        *host = xmalloc((size_t)(hostend - h) + 1);
        memcpy(*host, h, (size_t)(hostend - h));
        (*host)[hostend - h] = 0;
        *port = xstrdup(str_eq(*scheme, "https") ? "443" : "80");
    }
    *path = xstrdup(slash ? slash : "/");
    return 0;
}

static HttpResp *parse_http_response(Buf *raw) {
    char *hdr_end = strstr(raw->s, "\r\n\r\n");
    if (!hdr_end) {
        Buf m;
        buf_init(&m);
        buf_printf(&m, "malformed HTTP response (got %zu bytes)", raw->len);
        HttpResp *e = resp_err(m.s);
        buf_free(&m);
        return e;
    }
    long status = 0;
    {
        const char *sp = strchr(raw->s, ' ');
        if (sp) status = atol(sp + 1);
    }

    int chunked = 0;
    char *line = raw->s;
    while (line < hdr_end) {
        if (str_starts(line, "Transfer-Encoding:") && strstr(line, "chunked")) chunked = 1;
        char *nl = strstr(line, "\r\n");
        if (!nl || nl > hdr_end) break;
        line = nl + 2;
    }

    HttpResp *r = xmalloc(sizeof *r);
    memset(r, 0, sizeof *r);
    buf_init(&r->body);
    r->status = status;

    char *body_start = hdr_end + 4;
    if (!chunked) {
        buf_puts(&r->body, body_start);
    } else {
        char *p = body_start;
        while (*p) {
            unsigned long clen = strtoul(p, &p, 16);
            while (*p && *p != '\r') p++;
            if (p[0] == '\r' && p[1] == '\n') p += 2;
            if (clen == 0) break;
            if (clen > HTTP_MAX_BODY) break;
            buf_add(&r->body, p, clen);
            p += clen;
            while (*p && *p != '\r') p++;
            if (p[0] == '\r' && p[1] == '\n') p += 2;
        }
    }
    return r;
}

/* ---- native sockets (+ optional in-process TLS) ---- */

static HttpResp *http_post_raw(const char *method, const char *scheme, const char *host,
                               const char *port, const char *path, const char *const *headers,
                               int nheaders, const char *body, int timeout_s, int insecure,
                               HttpChunkCb on_chunk, void *chunk_user) {
    char *conn_err = NULL;
    int fd = net_connect(host, port, timeout_s, &conn_err, NULL);
    if (fd < 0) {
        HttpResp *e = resp_err(conn_err ? conn_err : "connect failed");
        free(conn_err);
        return e;
    }

    int use_tls = str_eq(scheme, "https");
#ifdef CLAWC_TLS
    TlsConn *tls = NULL;
    if (use_tls) {
        char *err = NULL;
        tls = tls_wrap(fd, host, insecure, &err);
        if (!tls) {
            close(fd);
            HttpResp *e = resp_err(err ? err : "TLS handshake failed");
            free(err);
            return e;
        }
    }
#else
    (void)insecure;
    if (use_tls) {
        close(fd);
        return resp_err("built-in TLS not compiled in (use make TLS=1 or install curl)");
    }
#endif

    Buf req;
    buf_init(&req);
    buf_printf(&req, "%s %s HTTP/1.1\r\n", method, path);
    if (atoi(port) != 80) buf_printf(&req, "Host: %s:%s\r\n", host, port);
    else buf_printf(&req, "Host: %s\r\n", host);
    buf_puts(&req, "Accept: application/json, text/plain, */*\r\n");
    if (body) {
        buf_puts(&req, "Content-Type: application/json\r\n");
        buf_printf(&req, "Content-Length: %zu\r\n", strlen(body));
    }
    buf_puts(&req, "Connection: close\r\n");
    int i;
    for (i = 0; i < nheaders; i++) buf_printf(&req, "%s\r\n", headers[i]);
    buf_puts(&req, "\r\n");
    if (body) buf_puts(&req, body);

    int send_rc;
#ifdef CLAWC_TLS
    send_rc = use_tls ? tls_send(tls, req.s, req.len) : -2;
    if (!use_tls)
#endif
    {
        size_t off = 0;
        send_rc = 0;
        while (off < req.len) {
            ssize_t n = send(fd, req.s + off, req.len - off, 0);
            if (n <= 0) { send_rc = -1; break; }
            off += (size_t)n;
        }
    }
    buf_free(&req);
    if (send_rc != 0) {
#ifdef CLAWC_TLS
        if (tls) tls_close(tls);
        else
#endif
        close(fd);
        return resp_err("send failed");
    }

    Buf raw;
    buf_init(&raw);
    char tmp[4096];
    int hdr_done = 0;
    size_t hdr_end_pos = 0;
    for (;;) {
        ssize_t n;
#ifdef CLAWC_TLS
        if (use_tls) {
            n = tls_recv(tls, tmp, sizeof tmp, timeout_s);
            if (n == -2) break; /* timeout: return what we have */
            if (n <= 0) break;
        } else
#endif
        {
            n = recv(fd, tmp, sizeof tmp, 0);
            if (n <= 0) break;
        }
        if (raw.len + (size_t)n > HTTP_MAX_BODY) break;
        buf_add(&raw, tmp, (size_t)n);
        if (on_chunk) {
            if (!hdr_done) {
                char *he = strstr(raw.s, "\r\n\r\n");
                if (he) {
                    hdr_done = 1;
                    hdr_end_pos = (size_t)(he - raw.s) + 4;
                    if (raw.len > hdr_end_pos)
                        on_chunk(chunk_user, raw.s + hdr_end_pos, raw.len - hdr_end_pos);
                }
            } else {
                on_chunk(chunk_user, tmp, (size_t)n);
            }
        }
    }
#ifdef CLAWC_TLS
    if (tls) tls_close(tls);
    else
#endif
    close(fd);

    HttpResp *r = parse_http_response(&raw);
    buf_free(&raw);
    return r;
}

/* ---- curl(1) fallback for https:// in builds without -DCLAWC_TLS ---- */

#ifndef CLAWC_TLS
static HttpResp *http_post_curl(const char *method, const char *url,
                                const char *const *headers, int nheaders,
                                const char *body, int timeout_s, int insecure) {
    /* request/response temp files live in a writable temp dir: honor $TMPDIR,
       then /tmp (preserves prior behavior), then $HOME as a last resort so
       builds without a writable /tmp (e.g. Android) still work. */
    const char *dirs[3] = { getenv("TMPDIR"), "/tmp", getenv("HOME") };
    char body_path[512], out_path[512];
    int bfd = -1, ofd = -1;
    int di;
    for (di = 0; di < 3; di++) {
        if (!dirs[di] || !dirs[di][0]) continue;
        snprintf(body_path, sizeof body_path, "%s/clawc-req-XXXXXX", dirs[di]);
        snprintf(out_path,  sizeof out_path,  "%s/clawc-resp-XXXXXX", dirs[di]);
        bfd = mkstemp(body_path);
        ofd = mkstemp(out_path);
        if (bfd >= 0 && ofd >= 0) break;
        if (bfd >= 0) { unlink(body_path); close(bfd); }
        if (ofd >= 0) { unlink(out_path); close(ofd); }
        bfd = ofd = -1;
    }
    if (bfd < 0 || ofd < 0) return resp_err("mkstemp failed (no writable temp dir)");
    if (body) {
        size_t blen = strlen(body);
        if ((size_t)write(bfd, body, blen) != blen) {
            close(bfd); close(ofd);
            unlink(body_path); unlink(out_path);
            return resp_err("write failed");
        }
    }
    close(bfd);
    close(ofd);

    Buf cmd;
    buf_init(&cmd);
    buf_printf(&cmd, "curl -sS --max-time %d %s-X %s ",
               timeout_s > 0 ? timeout_s : 60, insecure ? "-k " : "", method);
    buf_printf(&cmd, "-H 'Content-Type: application/json' -H 'Accept: application/json' ");
    int i;
    for (i = 0; i < nheaders; i++)
        buf_printf(&cmd, "-H '%s' ", headers[i]); /* trusted config only */
    if (body) buf_printf(&cmd, "--data-binary @%s ", body_path);
    buf_printf(&cmd, "-o '%s' -w '%%{http_code}' '%s' 2>/dev/null", out_path, url);

    HttpResp *r = xmalloc(sizeof *r);
    memset(r, 0, sizeof *r);
    buf_init(&r->body);
    r->status = -1;

    FILE *fp = popen(cmd.s, "r");
    if (!fp) {
        buf_free(&cmd);
        unlink(body_path); unlink(out_path);
        r->err = xstrdup("popen(curl) failed");
        return r;
    }
    Buf code;
    buf_init(&code);
    char tmp[64];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, fp)) > 0) buf_add(&code, tmp, n);
    int rc = pclose(fp);
    unlink(body_path);

    if (rc != 0 && code.len == 0) {
        buf_free(&code);
        unlink(out_path);
        r->err = xstrdup("curl failed (not installed? network down?)");
        return r;
    }
    r->status = strtol(code.s, NULL, 10);
    buf_free(&code);
    char *data = read_file(out_path);
    unlink(out_path);
    if (data) {
        buf_puts(&r->body, data);
        free(data);
    }
    return r;
}
#endif

HttpResp *http_request_ex(const char *method, const char *url,
                          const char *const *headers, int nheaders,
                          const char *body, int timeout_s, int insecure,
                          HttpChunkCb on_chunk, void *chunk_user) {
    char *scheme = NULL, *host = NULL, *port = NULL, *path = NULL;
    if (parse_url(url, &scheme, &host, &port, &path) != 0)
        return resp_err("bad URL (need http:// or https://)");
    HttpResp *r;
    if (str_eq(scheme, "http") || str_eq(scheme, "https")) {
#ifdef CLAWC_TLS
        r = http_post_raw(method, scheme, host, port, path, headers, nheaders,
                          body, timeout_s, insecure, on_chunk, chunk_user);
#else
        if (str_eq(scheme, "https")) {
            r = http_post_curl(method, url, headers, nheaders, body, timeout_s, insecure);
            /* curl fallback has no live streaming: fire the whole body at once */
            if (r->status >= 0 && on_chunk && r->body.len)
                on_chunk(chunk_user, r->body.s, r->body.len);
        } else {
            r = http_post_raw(method, scheme, host, port, path, headers, nheaders,
                              body, timeout_s, insecure, on_chunk, chunk_user);
        }
#endif
    } else {
        r = resp_err("unsupported URL scheme");
    }
    free(scheme);
    free(host);
    free(port);
    free(path);
    return r;
}

HttpResp *http_request(const char *method, const char *url,
                       const char *const *headers, int nheaders,
                       const char *body, int timeout_s, int insecure) {
    return http_request_ex(method, url, headers, nheaders, body, timeout_s, insecure,
                           NULL, NULL);
}

HttpResp *http_post_json(const char *url, const char *const *headers, int nheaders,
                         const char *body, int timeout_s, int insecure) {
    return http_request("POST", url, headers, nheaders, body, timeout_s, insecure);
}
