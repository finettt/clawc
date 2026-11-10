/* clawc - history.c */
#define _XOPEN_SOURCE 700
#include "history.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define AUDIT_TRUNC 240

static void ensure_dir_for(const char *path) {
    /* mkdir -p for the dirname of path (best effort) */
    char *copy = xstrdup(path);
    char *slash = strrchr(copy, '/');
    if (slash && slash != copy) {
        *slash = 0;
        char *p = copy;
        for (; *p; p++) {
            if (*p == '/') {
                *p = 0;
                if (copy[0]) mkdir(copy, 0755);
                *p = '/';
            }
        }
        mkdir(copy, 0755);
    }
    free(copy);
}

static void msg_to_json(const Msg *m, Buf *out) {
    buf_puts(out, "{\"role\":");
    json_quote(out, m->role ? m->role : "user");
    buf_puts(out, ",\"content\":");
    json_quote(out, m->content ? m->content : "");
    if (m->tool_call_id && m->tool_call_id[0]) {
        buf_puts(out, ",\"tool_call_id\":");
        json_quote(out, m->tool_call_id);
    }
    if (m->ncalls > 0) {
        buf_puts(out, ",\"tool_calls\":[");
        int i;
        for (i = 0; i < m->ncalls; i++) {
            if (i) buf_puts(out, ",");
            buf_puts(out, "{\"id\":");
            json_quote(out, m->calls[i].id ? m->calls[i].id : "");
            buf_puts(out, ",\"name\":");
            json_quote(out, m->calls[i].name ? m->calls[i].name : "");
            buf_puts(out, ",\"args\":");
            if (m->calls[i].args && m->calls[i].args[0]) {
                JVal *v = json_parse(m->calls[i].args);
                if (v) { json_dump(v, out); json_free(v); }
                else buf_puts(out, "{}");
            } else buf_puts(out, "{}");
            buf_puts(out, "}");
        }
        buf_puts(out, "]");
    }
    buf_puts(out, "}\n");
}

static int line_to_msg(const char *line, Msg *m) {
    JVal *v = json_parse(line);
    if (!v || v->type != J_OBJ) { json_free(v); return -1; }
    const char *role = json_get_str(v, "role");
    const char *content = json_get_str(v, "content");
    if (!role) { json_free(v); return -1; }
    memset(m, 0, sizeof *m);
    m->role = xstrdup(role);
    m->content = xstrdup(content ? content : "");
    const char *tcid = json_get_str(v, "tool_call_id");
    if (tcid) m->tool_call_id = xstrdup(tcid);
    JVal *tcs = json_obj_get(v, "tool_calls");
    if (tcs && tcs->type == J_ARR && tcs->nitems > 0) {
        m->calls = xmalloc((size_t)tcs->nitems * sizeof *m->calls);
        memset(m->calls, 0, (size_t)tcs->nitems * sizeof *m->calls);
        m->ncalls = tcs->nitems;
        int i;
        for (i = 0; i < tcs->nitems; i++) {
            JVal *tc = tcs->items[i];
            m->calls[i].id = xstrdup(json_get_str(tc, "id") ? json_get_str(tc, "id") : "");
            m->calls[i].name = xstrdup(json_get_str(tc, "name") ? json_get_str(tc, "name") : "");
            JVal *args = json_obj_get(tc, "args");
            Buf ab;
            buf_init(&ab);
            if (args) json_dump(args, &ab);
            else buf_puts(&ab, "{}");
            m->calls[i].args = ab.s;
        }
    }
    json_free(v);
    return 0;
}

int hist_load(const char *path, Msg **out, int *nout) {
    *out = NULL;
    *nout = 0;
    char *data = read_file(path);
    if (!data) return 0;
    int cap = 0;
    Msg *arr = NULL;
    int n = 0;
    char *line = data;
    while (*line) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (len > 0) {
            char *tmp = xmalloc(len + 1);
            memcpy(tmp, line, len);
            tmp[len] = 0;
            Msg m;
            if (line_to_msg(tmp, &m) == 0) {
                if (n + 1 > cap) {
                    cap = cap ? cap * 2 : 16;
                    arr = xrealloc(arr, (size_t)cap * sizeof *arr);
                }
                arr[n++] = m;
            }
            free(tmp);
        }
        if (!nl) break;
        line = nl + 1;
    }
    free(data);
    *out = arr;
    *nout = n;
    return 0;
}

int hist_append(const char *path, const Msg *m) {
    ensure_dir_for(path);
    FILE *f = fopen(path, "ab");
    if (!f) return -1;
    Buf b;
    buf_init(&b);
    msg_to_json(m, &b);
    int ok = fwrite(b.s, 1, b.len, f) == b.len;
    buf_free(&b);
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

int hist_truncate(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    return fclose(f) == 0 ? 0 : -1;
}

int hist_write(const char *path, const Msg *msgs, int n, int flag) {
    ensure_dir_for(path);
    FILE *f = fopen(path, flag ? "ab" : "wb");
    if (!f) return -1;
    int ok = 1;
    for (int i = 0; i < n; i++) {
        Buf b;
        buf_init(&b);
        msg_to_json(&msgs[i], &b);
        if (fwrite(b.s, 1, b.len, f) != b.len) ok = 0;
        buf_free(&b);
    }
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

void audit_log(const char *workspace, const char *session, const char *tool,
               const char *args, const char *result) {
    Buf path;
    buf_init(&path);
    buf_printf(&path, "%s/history/audit.jsonl", workspace);
    ensure_dir_for(path.s);

    char stamp[32];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    stamp_fmt(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", &tmv);

    FILE *f = fopen(path.s, "ab");
    buf_free(&path);
    if (!f) return;

    Buf line;
    buf_init(&line);
    buf_printf(&line, "{\"ts\":\"%s\",\"session\":", stamp);
    json_quote(&line, session ? session : "?");
    buf_puts(&line, ",\"tool\":");
    json_quote(&line, tool ? tool : "?");
    buf_puts(&line, ",\"args\":");
    {
        Buf t;
        buf_init(&t);
        buf_printf(&t, "%.*s", AUDIT_TRUNC, args ? args : "");
        json_quote(&line, t.s);
        buf_free(&t);
    }
    buf_puts(&line, ",\"result\":");
    {
        Buf t;
        buf_init(&t);
        buf_printf(&t, "%.*s", AUDIT_TRUNC, result ? result : "");
        json_quote(&line, t.s);
        buf_free(&t);
    }
    buf_puts(&line, "}\n");
    fwrite(line.s, 1, line.len, f);
    fclose(f);
    buf_free(&line);
}
