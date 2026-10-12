/* clawc - http.h: HTTP(S) transport.
   http://            -> raw sockets (pure C)
   https:// with TLS  -> raw sockets + in-process TLS (OpenSSL, -DCLAWC_TLS)
   https:// without   -> curl(1) subprocess fallback */
#ifndef CLAWC_HTTP_H
#define CLAWC_HTTP_H

#include "util.h"

typedef struct {
    long status;      /* HTTP status, or -1 on transport error */
    Buf body;
    char *err;        /* malloc'd error string when status < 0, else NULL */
} HttpResp;

void http_resp_free(HttpResp *r);

/* Called with each chunk of the response BODY as it arrives (streaming). */
typedef void (*HttpChunkCb)(void *user, const char *data, size_t n);

/* Generic request. body may be NULL for GET. headers: "Name: value" strings.
   insecure: skip TLS certificate/hostname verification (self-signed etc).
   on_chunk: optional incremental body callback (may be NULL). */
HttpResp *http_request_ex(const char *method, const char *url,
                          const char *const *headers, int nheaders,
                          const char *body, int timeout_s, int insecure,
                          HttpChunkCb on_chunk, void *chunk_user);

/* Simple wrapper without streaming. */
HttpResp *http_request(const char *method, const char *url,
                       const char *const *headers, int nheaders,
                       const char *body, int timeout_s, int insecure);

/* Convenience wrapper: POST with Content-Type: application/json. */
HttpResp *http_post_json(const char *url, const char *const *headers, int nheaders,
                         const char *body, int timeout_s, int insecure);

#endif
