/* clawc - llm.c */
#define _XOPEN_SOURCE 700
#include "llm.h"
#include "http.h"
#include "json.h"
#include "tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void msg_clear(Msg *m) {
    int i;
    free(m->role);
    free(m->content);
    free(m->img_path);
    free(m->tool_call_id);
    for (i = 0; i < m->ncalls; i++) {
        free(m->calls[i].id);
        free(m->calls[i].name);
        free(m->calls[i].args);
    }
    free(m->calls);
    memset(m, 0, sizeof *m);
}

void msg_free(Msg *m) {
    msg_clear(m);
    free(m);
}

static char *endpoint_for(const LlmCfg *cfg) {
    /* suffix without leading slash; joined exactly once */
    const char *suffix = (cfg->api_kind == API_ANTHROPIC) ? "messages" : "chat/completions";
    if (strstr(cfg->api_url, suffix)) return xstrdup(cfg->api_url);
    size_t n = strlen(cfg->api_url);
    int slash = n && cfg->api_url[n-1] == '/';
    Buf b;
    buf_init(&b);
    buf_printf(&b, "%s%s%s", cfg->api_url, slash ? "" : "/", suffix);
    return b.s;
}

static HttpResp *post(const LlmCfg *cfg, const char *body,
                      HttpChunkCb on_chunk, void *chunk_user) {
    char *url = endpoint_for(cfg);
    char auth[1024];
    const char *headers[4];
    int nh = 0;
    if (cfg->api_kind == API_ANTHROPIC) {
        snprintf(auth, sizeof auth, "x-api-key: %s", cfg->api_key ? cfg->api_key : "");
        headers[nh++] = auth;
        headers[nh++] = "anthropic-version: 2023-06-01";
    } else if (cfg->api_key && cfg->api_key[0]) {
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", cfg->api_key);
        headers[nh++] = auth;
    }
    HttpResp *r = http_request_ex("POST", url, headers, nh, body, cfg->timeout_s,
                                  cfg->insecure, on_chunk, chunk_user);
    free(url);
    return r;
}

/* ---- vision: read an image file, return base64 (malloc'd) ---- */

static char *img_mime(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "image/jpeg";
    dot++;
    if (str_eq(dot, "png")) return "image/png";
    if (str_eq(dot, "webp")) return "image/webp";
    if (str_eq(dot, "gif")) return "image/gif";
    return "image/jpeg";
}

static char *b64_file(const char *path, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *raw = xmalloc(cap);
    size_t n = fread(raw, 1, cap, f);
    fclose(f);
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = xmalloc((n + 2) / 3 * 4 + 1);
    size_t i, o = 0;
    for (i = 0; i + 2 < n; i += 3) {
        unsigned v = ((unsigned char)raw[i] << 16) |
                     ((unsigned char)raw[i + 1] << 8) | (unsigned char)raw[i + 2];
        out[o++] = T[(v >> 18) & 63]; out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];  out[o++] = T[v & 63];
    }
    if (i < n) {
        unsigned v = (unsigned char)raw[i] << 16;
        if (i + 1 < n) v |= (unsigned char)raw[i + 1] << 8;
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    free(raw);
    return out;
}

/* ---- request builders ---- */

static char *build_openai(const LlmCfg *cfg, const Msg *msgs, int nmsgs, int stream,
                         int with_tools) {
    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\"model\":");
    json_quote(&b, cfg->model);
    buf_printf(&b, ",\"max_tokens\":%d,\"stream\":%s",
               cfg->max_tokens > 0 ? cfg->max_tokens : 4096,
               stream ? "true" : "false");
    if (with_tools) buf_printf(&b, ",\"tools\":%s", tools_openai_json());
    buf_puts(&b, ",\"messages\":[");
    int i, j;
    for (i = 0; i < nmsgs; i++) {
        const Msg *m = &msgs[i];
        if (i) buf_puts(&b, ",");
        buf_printf(&b, "{\"role\":\"%s\"", m->role);
        if (str_eq(m->role, "tool")) {
            buf_puts(&b, ",\"tool_call_id\":");
            json_quote(&b, m->tool_call_id ? m->tool_call_id : "");
            buf_puts(&b, ",\"content\":");
            json_quote(&b, m->content ? m->content : "");
        } else if (str_eq(m->role, "assistant") && m->ncalls > 0) {
            buf_puts(&b, ",\"content\":");
            if (m->content && m->content[0]) json_quote(&b, m->content);
            else buf_puts(&b, "null");
            buf_puts(&b, ",\"tool_calls\":[");
            for (j = 0; j < m->ncalls; j++) {
                if (j) buf_puts(&b, ",");
                buf_puts(&b, "{\"id\":");
                json_quote(&b, m->calls[j].id ? m->calls[j].id : "");
                buf_puts(&b, ",\"type\":\"function\",\"function\":{\"name\":");
                json_quote(&b, m->calls[j].name);
                buf_puts(&b, ",\"arguments\":");
                json_quote(&b, m->calls[j].args ? m->calls[j].args : "{}");
                buf_puts(&b, "}}");
            }
            buf_puts(&b, "]");
        } else if (m->img_path && m->img_path[0]) {
            char *b64 = b64_file(m->img_path, 2u << 20);
            buf_puts(&b, ",\"content\":[{\"type\":\"text\",\"text\":");
            json_quote(&b, m->content ? m->content : "");
            buf_puts(&b, "}");
            if (b64) {
                buf_printf(&b, ",{\"type\":\"image_url\",\"image_url\":"
                               "{\"url\":\"data:%s;base64,%s\"}}",
                           img_mime(m->img_path), b64);
                free(b64);
            }
            buf_puts(&b, "]");
        } else {
            buf_puts(&b, ",\"content\":");
            json_quote(&b, m->content ? m->content : "");
        }
        buf_puts(&b, "}");
    }
    buf_puts(&b, "]}");
    return b.s;
}

static void anthropic_tool_results(Buf *b, const Msg *msgs, int nmsgs, int *i) {
    /* emit one user message containing tool_result blocks for consecutive tool msgs */
    buf_puts(b, "{\"role\":\"user\",\"content\":[");
    int first = 1;
    for (; *i < nmsgs && str_eq(msgs[*i].role, "tool"); (*i)++) {
        if (!first) buf_puts(b, ",");
        first = 0;
        buf_puts(b, "{\"type\":\"tool_result\",\"tool_use_id\":");
        json_quote(b, msgs[*i].tool_call_id ? msgs[*i].tool_call_id : "");
        buf_puts(b, ",\"content\":");
        json_quote(b, msgs[*i].content ? msgs[*i].content : "");
        buf_puts(b, "}");
    }
    buf_puts(b, "]}");
    (*i)--; /* outer loop will ++ */
}

static char *build_anthropic(const LlmCfg *cfg, const Msg *msgs, int nmsgs,
                           int with_tools) {
    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\"model\":");
    json_quote(&b, cfg->model);
    buf_printf(&b, ",\"max_tokens\":%d,", cfg->max_tokens > 0 ? cfg->max_tokens : 4096);
    /* system prompt: concat of all system messages */
    buf_puts(&b, "\"system\":");
    {
        Buf s;
        buf_init(&s);
        int i;
        for (i = 0; i < nmsgs; i++)
            if (str_eq(msgs[i].role, "system")) {
                if (s.len) buf_puts(&s, "\n\n");
                buf_puts(&s, msgs[i].content ? msgs[i].content : "");
            }
        json_quote(&b, s.s);
        buf_free(&s);
    }
    buf_puts(&b, ",\"tools\":");
    if (with_tools) buf_puts(&b, tools_anthropic_json());
    else buf_puts(&b, "[]");
    buf_puts(&b, ",\"messages\":[");
    int i, j, nout = 0;
    for (i = 0; i < nmsgs; i++) {
        const Msg *m = &msgs[i];
        if (str_eq(m->role, "system")) continue;
        if (str_eq(m->role, "tool")) {
            if (nout) buf_puts(&b, ",");
            anthropic_tool_results(&b, msgs, nmsgs, &i);
            nout++;
            continue;
        }
        if (nout) buf_puts(&b, ",");
        nout++;
        if (str_eq(m->role, "assistant") && m->ncalls > 0) {
            buf_puts(&b, "{\"role\":\"assistant\",\"content\":[");
            if (m->content && m->content[0]) {
                buf_puts(&b, "{\"type\":\"text\",\"text\":");
                json_quote(&b, m->content);
                buf_puts(&b, "}");
                if (m->ncalls) buf_puts(&b, ",");
            }
            for (j = 0; j < m->ncalls; j++) {
                if (j) buf_puts(&b, ",");
                buf_puts(&b, "{\"type\":\"tool_use\",\"id\":");
                json_quote(&b, m->calls[j].id ? m->calls[j].id : "");
                buf_puts(&b, ",\"name\":");
                json_quote(&b, m->calls[j].name);
                buf_puts(&b, ",\"input\":");
                if (m->calls[j].args && m->calls[j].args[0]) {
                    JVal *v = json_parse(m->calls[j].args);
                    if (v) { json_dump(v, &b); json_free(v); }
                    else buf_puts(&b, "{}");
                } else {
                    buf_puts(&b, "{}");
                }
                buf_puts(&b, "}");
            }
            buf_puts(&b, "]}");
        } else if (m->img_path && m->img_path[0] && !str_eq(m->role, "assistant")) {
            char *b64 = b64_file(m->img_path, 2u << 20);
            buf_printf(&b, "{\"role\":\"%s\",\"content\":[", m->role);
            buf_puts(&b, "{\"type\":\"text\",\"text\":");
            json_quote(&b, m->content ? m->content : "");
            buf_puts(&b, "}");
            if (b64) {
                buf_printf(&b, ",{\"type\":\"image\",\"source\":"
                               "{\"type\":\"base64\",\"media_type\":\"%s\","
                               "\"data\":\"%s\"}}", img_mime(m->img_path), b64);
                free(b64);
            }
            buf_puts(&b, "]");
        } else {
            buf_printf(&b, "{\"role\":\"%s\",\"content\":",
                       str_eq(m->role, "assistant") ? "assistant" : "user");
            json_quote(&b, m->content ? m->content : "");
            buf_puts(&b, "}");
        }
    }
    buf_puts(&b, "]}");
    return b.s;
}

/* ---- response parsing (non-streamed) ---- */

static int parse_openai_reply(const char *body, Msg *out, char **err) {
    JVal *root = json_parse(body);
    if (!root) { *err = xstrdup("unparseable JSON from API"); return -1; }
    JVal *e = json_obj_get(root, "error");
    if (e) {
        const char *m = json_get_str(e, "message");
        *err = xstrdup(m ? m : "API returned an error object");
        json_free(root);
        return -1;
    }
    JVal *choices = json_obj_get(root, "choices");
    JVal *msg = (choices && choices->type == J_ARR && choices->nitems > 0)
                    ? json_obj_get(choices->items[0], "message") : NULL;
    if (!msg) {
        *err = xstrdup("no choices in API response");
        json_free(root);
        return -1;
    }
    const char *content = json_get_str(msg, "content");
    out->content = xstrdup(content ? content : "");
    JVal *tcs = json_obj_get(msg, "tool_calls");
    if (tcs && tcs->type == J_ARR) {
        int i;
        out->calls = xmalloc((size_t)tcs->nitems * sizeof *out->calls);
        memset(out->calls, 0, (size_t)tcs->nitems * sizeof *out->calls);
        out->ncalls = tcs->nitems;
        for (i = 0; i < tcs->nitems; i++) {
            JVal *tc = tcs->items[i];
            JVal *fn = json_obj_get(tc, "function");
            out->calls[i].id = xstrdup(json_get_str(tc, "id") ? json_get_str(tc, "id") : "");
            out->calls[i].name = xstrdup(json_get_str(fn, "name") ? json_get_str(fn, "name") : "");
            const char *args = json_get_str(fn, "arguments");
            out->calls[i].args = xstrdup(args && args[0] ? args : "{}");
        }
    }
    json_free(root);
    return 0;
}

static int parse_anthropic_reply(const char *body, Msg *out, char **err) {
    JVal *root = json_parse(body);
    if (!root) { *err = xstrdup("unparseable JSON from API"); return -1; }
    JVal *e = json_obj_get(root, "error");
    if (e) {
        const char *m = json_get_str(e, "message");
        *err = xstrdup(m ? m : "API returned an error object");
        json_free(root);
        return -1;
    }
    JVal *content = json_obj_get(root, "content");
    if (!content || content->type != J_ARR) {
        *err = xstrdup("no content in API response");
        json_free(root);
        return -1;
    }
    Buf text;
    buf_init(&text);
    int i, ncalls = 0;
    for (i = 0; i < content->nitems; i++) {
        JVal *blk = content->items[i];
        const char *type = json_get_str(blk, "type");
        if (str_eq(type, "text")) {
            const char *t = json_get_str(blk, "text");
            if (t) buf_puts(&text, t);
        } else if (str_eq(type, "tool_use")) {
            ncalls++;
        }
    }
    out->content = text.s;
    if (ncalls) {
        out->calls = xmalloc((size_t)ncalls * sizeof *out->calls);
        memset(out->calls, 0, (size_t)ncalls * sizeof *out->calls);
        out->ncalls = ncalls;
        int k = 0;
        for (i = 0; i < content->nitems; i++) {
            JVal *blk = content->items[i];
            if (!str_eq(json_get_str(blk, "type"), "tool_use")) continue;
            out->calls[k].id = xstrdup(json_get_str(blk, "id") ? json_get_str(blk, "id") : "");
            out->calls[k].name = xstrdup(json_get_str(blk, "name") ? json_get_str(blk, "name") : "");
            JVal *input = json_obj_get(blk, "input");
            Buf ab;
            buf_init(&ab);
            if (input) json_dump(input, &ab);
            else buf_puts(&ab, "{}");
            out->calls[k].args = ab.s;
            k++;
        }
    }
    json_free(root);
    return 0;
}

/* ---- SSE streaming (OpenAI dialect) ---- */

#define MAX_STREAM_CALLS 16

typedef struct {
    Buf line_buf;             /* incomplete SSE line fragments */
    Buf raw_json;             /* fallback: whole body if not SSE */
    Buf content;
    struct {
        Buf id, name, args;
        int used;
    } calls[MAX_STREAM_CALLS];
    int mode;                 /* 0 unknown, 1 sse, 2 json */
    int done;
    LlmDeltaCb cb;
    void *user;
} StreamState;

static void stream_accumulate_tool_call(StreamState *s, JVal *tc) {
    double idx_d = json_get_num(tc, "index", 0);
    int idx = (int)idx_d;
    if (idx < 0 || idx >= MAX_STREAM_CALLS) return;
    s->calls[idx].used = 1;
    const char *id = json_get_str(tc, "id");
    if (id && id[0] && s->calls[idx].id.len == 0) buf_puts(&s->calls[idx].id, id);
    JVal *fn = json_obj_get(tc, "function");
    const char *name = fn ? json_get_str(fn, "name") : NULL;
    if (name && name[0] && s->calls[idx].name.len == 0) buf_puts(&s->calls[idx].name, name);
    const char *args = fn ? json_get_str(fn, "arguments") : NULL;
    if (args) buf_puts(&s->calls[idx].args, args);
}

static void stream_handle_line(StreamState *s, const char *line) {
    if (!line[0]) return;
    if (s->mode == 0) {
        if (str_starts(line, "data:")) s->mode = 1;
        else if (line[0] == '{') {
            s->mode = 2;
            buf_puts(&s->raw_json, line);
            return;
        } else return;
    }
    if (s->mode == 2) {
        buf_puts(&s->raw_json, line);
        return;
    }
    const char *p = line;
    if (str_starts(p, "data:")) p += 5;
    while (*p == ' ') p++;
    if (str_eq(p, "[DONE]")) { s->done = 1; return; }
    if (p[0] != '{') return;
    JVal *v = json_parse(p);
    if (!v) return;
    JVal *e = json_obj_get(v, "error");
    if (e) {
        const char *m = json_get_str(e, "message");
        if (m) buf_printf(&s->content, "[api error] %s", m);
        json_free(v);
        return;
    }
    JVal *choices = json_obj_get(v, "choices");
    if (choices && choices->type == J_ARR && choices->nitems > 0) {
        JVal *delta = json_obj_get(choices->items[0], "delta");
        if (delta) {
            const char *text = json_get_str(delta, "content");
            if (text && text[0]) {
                buf_puts(&s->content, text);
                if (s->cb) s->cb(s->user, text, strlen(text));
            }
            JVal *tcs = json_obj_get(delta, "tool_calls");
            if (tcs && tcs->type == J_ARR) {
                int i;
                for (i = 0; i < tcs->nitems; i++)
                    stream_accumulate_tool_call(s, tcs->items[i]);
            }
        }
    }
    json_free(v);
}

static void sse_on_chunk(void *user, const char *data, size_t n) {
    StreamState *s = user;
    if (s->mode == 2) {
        buf_add(&s->raw_json, data, n);
        return;
    }
    if (s->mode == 0 && n > 0 && data[0] == '{') {
        s->mode = 2; /* server ignored stream:true, sent plain JSON */
        buf_add(&s->raw_json, data, n);
        return;
    }
    buf_add(&s->line_buf, data, n);
    /* process complete lines */
    char *start = s->line_buf.s;
    for (;;) {
        char *nl = strchr(start, '\n');
        if (!nl) break;
        *nl = 0;
        size_t len = strlen(start);
        if (len && start[len-1] == '\r') start[len-1] = 0;
        stream_handle_line(s, start);
        start = nl + 1;
        if (s->mode == 2) {
            /* switched to JSON mode mid-stream: rest is raw body */
            buf_add(&s->raw_json, start, strlen(start));
            s->line_buf.len = 0;
            s->line_buf.s[0] = 0;
            return;
        }
        if (s->done) break;
    }
    /* keep the incomplete tail */
    size_t tail = strlen(start);
    memmove(s->line_buf.s, start, tail + 1);
    s->line_buf.len = tail;
}

static void stream_state_free(StreamState *s) {
    buf_free(&s->line_buf);
    buf_free(&s->raw_json);
    buf_free(&s->content);
    int i;
    for (i = 0; i < MAX_STREAM_CALLS; i++) {
        buf_free(&s->calls[i].id);
        buf_free(&s->calls[i].name);
        buf_free(&s->calls[i].args);
    }
}

int llm_chat(const LlmCfg *cfg, const Msg *msgs, int nmsgs, Msg *out, char **err,
             LlmDeltaCb on_delta, void *delta_user) {
    return llm_chat_ex(cfg, msgs, nmsgs, out, err, on_delta, delta_user, 1);
}

int llm_chat_ex(const LlmCfg *cfg, const Msg *msgs, int nmsgs, Msg *out, char **err,
                LlmDeltaCb on_delta, void *delta_user, int with_tools) {
    memset(out, 0, sizeof *out);
    out->role = xstrdup("assistant");
    *err = NULL;

    int stream = (on_delta != NULL) && (cfg->api_kind == API_OPENAI);
    char *body = (cfg->api_kind == API_ANTHROPIC)
                     ? build_anthropic(cfg, msgs, nmsgs, with_tools)
                     : build_openai(cfg, msgs, nmsgs, stream, with_tools);

    StreamState ss;
    memset(&ss, 0, sizeof ss);
    buf_init(&ss.line_buf);
    buf_init(&ss.raw_json);
    buf_init(&ss.content);
    {
        int i;
        for (i = 0; i < MAX_STREAM_CALLS; i++) {
            buf_init(&ss.calls[i].id);
            buf_init(&ss.calls[i].name);
            buf_init(&ss.calls[i].args);
        }
    }
    ss.cb = on_delta;
    ss.user = delta_user;

    HttpResp *r = post(cfg, body, stream ? sse_on_chunk : NULL, stream ? &ss : NULL);
    free(body);

    if (r->status < 0) {
        *err = r->err ? xstrdup(r->err) : xstrdup("transport error");
        http_resp_free(r);
        stream_state_free(&ss);
        return -1;
    }
    if (r->status < 200 || r->status >= 300) {
        Buf m;
        buf_init(&m);
        buf_printf(&m, "HTTP %ld: %.300s", r->status, r->body.s ? r->body.s : "");
        *err = m.s;
        http_resp_free(r);
        stream_state_free(&ss);
        return -1;
    }

    int rc;
    if (cfg->api_kind == API_ANTHROPIC) {
        rc = parse_anthropic_reply(r->body.s, out, err);
        if (rc == 0 && on_delta && out->content && out->content[0])
            on_delta(delta_user, out->content, strlen(out->content));
    } else if (stream && ss.mode == 1) {
        /* assembled from SSE deltas */
        int ncall = 0, i;
        for (i = 0; i < MAX_STREAM_CALLS; i++) if (ss.calls[i].used) ncall++;
        out->content = xstrdup(ss.content.s);
        if (ncall) {
            out->calls = xmalloc((size_t)ncall * sizeof *out->calls);
            memset(out->calls, 0, (size_t)ncall * sizeof *out->calls);
            out->ncalls = ncall;
            int k = 0;
            for (i = 0; i < MAX_STREAM_CALLS; i++) {
                if (!ss.calls[i].used) continue;
                out->calls[k].id = xstrdup(ss.calls[i].id.len ? ss.calls[i].id.s : "call_0");
                out->calls[k].name = xstrdup(ss.calls[i].name.len ? ss.calls[i].name.s : "");
                out->calls[k].args = xstrdup(ss.calls[i].args.len ? ss.calls[i].args.s : "{}");
                k++;
            }
        }
        rc = 0;
    } else if (stream && ss.mode == 2) {
        /* server ignored stream:true — plain JSON body */
        rc = parse_openai_reply(ss.raw_json.len ? ss.raw_json.s : r->body.s, out, err);
        if (rc == 0 && on_delta && out->content && out->content[0])
            on_delta(delta_user, out->content, strlen(out->content));
    } else {
        rc = parse_openai_reply(r->body.s, out, err);
        if (rc == 0 && on_delta && out->content && out->content[0])
            on_delta(delta_user, out->content, strlen(out->content));
    }
    http_resp_free(r);
    stream_state_free(&ss);
    return rc;
}
