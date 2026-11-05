/* clawc - matrix.c: Matrix channel.
   Same shape as the Telegram channel: HTTPS long-poll (sync), a persisted
   next_batch cursor (at-most-once), per-room sessions, /yes //no confirm.
   Text rooms only - encrypted rooms are out of scope (documented). */
#define _XOPEN_SOURCE 700
#include "matrix.h"
#include "http.h"
#include "json.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MX_MAX_PEND 8

typedef struct {
    char room[288];
    char sender[256];
    char reply_to[288];
    char *body;
} MxEvent;

typedef struct {
    Agent *tmpl;
    MxCfg cfg;
    char next_batch[288];
    MxEvent pend[MX_MAX_PEND];
    int npend;
    /* confirm state: room currently awaiting /yes //no */
    char conf_room[288];
    int conf_answer; /* -1 pending, 0 no, 1 yes */
} MxRun;

/* ---- transport ---- */

static HttpResp *mx_call(MxRun *r, const char *method, const char *path,
                         const char *body, int timeout_s) {
    Buf url;
    buf_init(&url);
    buf_printf(&url, "%s/_matrix/client/v3%s", r->cfg.homeserver, path);
    const char *hdrs[2];
    char auth[400];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", r->cfg.token);
    hdrs[0] = auth;
    hdrs[1] = NULL;
    HttpResp *res = http_request(method, url.s, hdrs, 1, body, timeout_s,
                                 r->tmpl->tools.insecure_tls);
    buf_free(&url);
    return res;
}

static void mx_send_id(MxRun *r, const char *room, const char *text, char *event_id, size_t cap) {
    static unsigned txn;
    char path[700];
    snprintf(path, sizeof path, "/rooms/%s/send/m.room.message/m%ld-%ld-%u",
             room, (long)time(NULL), (long)getpid(), ++txn);
    Buf body;
    buf_init(&body);
    buf_puts(&body, "{\"msgtype\":\"m.text\",\"body\":");
    json_quote(&body, text);
    buf_puts(&body, "}");
    HttpResp *res = mx_call(r, "PUT", path, body.s, 30);
    if (res->status < 200 || res->status >= 300)
        fprintf(stderr, "[matrix] send HTTP %ld: %.120s\n", res->status,
                res->err ? res->err : res->body.s ? res->body.s : "");
    if (event_id && cap) {
        event_id[0] = 0;
        if (res->status >= 200 && res->status < 300 && res->body.s) {
            JVal *root = json_parse(res->body.s);
            const char *id = root ? json_get_str(root, "event_id") : NULL;
            if (id && id[0] && strlen(id) < cap) snprintf(event_id, cap, "%s", id);
            json_free(root);
        }
    }
    http_resp_free(res);
    buf_free(&body);
}
static void mx_send(MxRun *r, const char *room, const char *text) {
    mx_send_id(r, room, text, NULL, 0);
}

/* ---- cursor persistence (at-most-once, like telegram offsets) ---- */

static void mx_cursor_path(MxRun *r, char *dst, size_t n) {
    unsigned long h = 5381;
    const char *p = r->cfg.token;
    while (*p) h = h * 33 + (unsigned char)*p++;
    snprintf(dst, n, "%s/history/matrix-sync-%08lx", r->tmpl->tools.workspace, h);
}

static void mx_cursor_save(MxRun *r) {
    char p[640];
    mx_cursor_path(r, p, sizeof p);
    cursor_save(p, r->next_batch);
}

static void mx_cursor_load(MxRun *r) {
    char p[640];
    mx_cursor_path(r, p, sizeof p);
    char *v = read_file(p);
    if (v) {
        snprintf(r->next_batch, sizeof r->next_batch, "%s", v);
        free(v);
    }
}

/* ---- rooms allowlist ---- */

static int mx_room_ok(MxRun *r, const char *room) {
    if (!r->cfg.rooms || !r->cfg.rooms[0]) return 1;
    return csv_has(r->cfg.rooms, room);
}

/* ---- sync: long-poll and enqueue text messages ---- */

static int mx_sync(MxRun *r, int timeout_s) {
    char path[700];
    snprintf(path, sizeof path, "/sync?timeout=%d&since=%s", timeout_s * 1000,
             r->next_batch[0] ? r->next_batch : "0");
    HttpResp *res = mx_call(r, "GET", path, NULL, timeout_s + 20);
    if (res->status < 200 || res->status >= 300) {
        fprintf(stderr, "[matrix] sync HTTP %ld: %.120s\n", res->status,
                res->err ? res->err : "");
        http_resp_free(res);
        return -1;
    }
    JVal *root = json_parse(res->body.s);
    http_resp_free(res);
    if (!root) return -1;
    const char *nb = json_get_str(root, "next_batch");
    if (nb && nb[0]) snprintf(r->next_batch, sizeof r->next_batch, "%s", nb);
    JVal *join = json_obj_get(json_obj_get(root, "rooms"), "join");
    if (join && join->type == J_OBJ) {
        int i;
        for (i = 0; i < join->nkv && r->npend < MX_MAX_PEND; i++) {
            const char *room = join->keys ? join->keys[i] : NULL;
            JVal *roomv = join->vals[i];
            if (!room || strlen(room) >= sizeof r->pend[0].room || !roomv || !mx_room_ok(r, room)) continue;
            JVal *events = json_obj_get(json_obj_get(roomv, "timeline"), "events");
            if (!events || events->type != J_ARR) continue;
            int k;
            for (k = 0; k < events->nitems && r->npend < MX_MAX_PEND; k++) {
                JVal *ev = events->items[k];
                if (!ev) continue;
                const char *type = json_get_str(ev, "type");
                const char *sender = json_get_str(ev, "sender");
                if (!type || !str_eq(type, "m.room.message")) continue;
                if (!sender || strlen(sender) >= sizeof r->pend[0].sender ||
                    !csv_has(r->cfg.users, sender) ||
                    (r->cfg.user && str_eq(sender, r->cfg.user))) continue;
                JVal *content = json_obj_get(ev, "content");
                const char *mt = content ? json_get_str(content, "msgtype") : NULL;
                const char *body = content ? json_get_str(content, "body") : NULL;
                if (!mt || !str_eq(mt, "m.text") || !body || !body[0]) continue;
                snprintf(r->pend[r->npend].room, sizeof r->pend[0].room, "%s", room);
                snprintf(r->pend[r->npend].sender, sizeof r->pend[0].sender, "%s",
                         sender ? sender : "");
                JVal *rel = json_obj_get(content, "m.relates_to");
                JVal *reply = rel ? json_obj_get(rel, "m.in_reply_to") : NULL;
                const char *ref = reply ? json_get_str(reply, "event_id") : NULL;
                r->pend[r->npend].reply_to[0] = 0;
                if (ref && strlen(ref) < sizeof r->pend[0].reply_to)
                    snprintf(r->pend[r->npend].reply_to, sizeof r->pend[0].reply_to, "%s", ref);
                r->pend[r->npend].body = xstrdup(body);
                r->npend++;
            }
        }
    }
    json_free(root);
    if (r->next_batch[0]) mx_cursor_save(r);
    return 0;
}

static void mx_pend_shift(MxRun *r, int idx) {
    free(r->pend[idx].body);
    memmove(&r->pend[idx], &r->pend[idx + 1],
            (size_t)(r->npend - idx - 1) * sizeof r->pend[0]);
    r->npend--;
}

/* ---- confirm: text /yes //no within the window ---- */

typedef struct {
    MxRun *run;
    const char *room;
    const char *sender;
} MxCtx;

static int mx_confirm(void *user, const char *desc) {
    MxCtx *ctx = user;
    MxRun *r = ctx->run;
    Buf t;
    buf_init(&t);
    buf_printf(&t, "\xe2\x9a\xa0\xef\xb8\x8f %.900s\n(reply to this message with /yes or /no)", desc);
    char prompt_id[288];
    mx_send_id(r, ctx->room, t.s, prompt_id, sizeof prompt_id);
    buf_free(&t);

    int window = 120;
    const char *cs = getenv("CLAWC_CONFIRM_SECS");
    if (cs && atoi(cs) > 0) window = atoi(cs);
    time_t deadline = time(NULL) + window;
    snprintf(r->conf_room, sizeof r->conf_room, "%s", ctx->room);
    r->conf_answer = -1;
    while (r->conf_answer < 0 && time(NULL) < deadline) {
        int i;
        for (i = 0; i < r->npend; i++) {
            if (!str_eq(r->pend[i].room, ctx->room)) continue;
            const char *b = r->pend[i].body;
            /* Matrix clients may prepend a quoted fallback to reply bodies. */
            if (str_starts(b, "> ")) {
                const char *sep = strstr(b, "\n\n");
                if (sep) b = sep + 2;
            }
            int yes = str_starts(b, "/yes"), no = str_starts(b, "/no");
            if (!yes && !no) continue;
            if (prompt_id[0] && str_eq(r->pend[i].sender, ctx->sender) &&
                str_eq(r->pend[i].reply_to, prompt_id))
                r->conf_answer = yes ? 1 : 0;
            mx_pend_shift(r, i);
            if (r->conf_answer >= 0) break;
            i--;
        }
        if (r->conf_answer < 0) mx_sync(r, 10);
    }
    if (r->conf_answer < 0) r->conf_answer = 0;
    mx_send(r, ctx->room, r->conf_answer ? "(approved \xe2\x9c\x85)"
                                         : "(cancelled \xe2\x9d\x8c)");
    r->conf_room[0] = 0;
    return r->conf_answer;
}

/* ---- per-room agents ---- */

#define MX_MAX_ROOMS 16

typedef struct {
    char room[288];
    Agent agent;
} MxSession;

static Agent *mx_agent_for(MxRun *r, const char *room) {
    static MxSession sessions[MX_MAX_ROOMS];
    static int nsess;
    int i;
    for (i = 0; i < nsess; i++)
        if (str_eq(sessions[i].room, room)) return &sessions[i].agent;
    if (nsess >= MX_MAX_ROOMS) {
        /* evict the oldest (slot 0) */
        agent_free(&sessions[0].agent);
        memmove(&sessions[0], &sessions[1], (size_t)(nsess - 1) * sizeof sessions[0]);
        nsess--;
    }
    MxSession *s = &sessions[nsess++];
    snprintf(s->room, sizeof s->room, "%s", room);
    agent_init(&s->agent);
    s->agent.llm = r->tmpl->llm;
    s->agent.tools = r->tmpl->tools;
    s->agent.max_steps = r->tmpl->max_steps;
    s->agent.auto_approve = r->tmpl->auto_approve;
    s->agent.auto_learn = r->tmpl->auto_learn;
    /* session file: history/mx-<hash>.jsonl */
    unsigned long h = 5381;
    const char *p = room;
    while (*p) h = h * 33 + (unsigned char)*p++;
    char path[640];
    snprintf(path, sizeof path, "%s/history/mx-%08lx.jsonl",
             r->tmpl->tools.workspace, h);
    agent_open_session(&s->agent, path);
    return &s->agent;
}

/* ---- main loop ---- */

int matrix_run(Agent *tmpl, const MxCfg *mx) {
    if (!mx->homeserver || !mx->homeserver[0] || !mx->token || !mx->token[0]) {
        fprintf(stderr, "[matrix] needs matrix_homeserver and matrix_token\n");
        return 2;
    }
    if (!sender_ready("matrix", mx->users)) return 2;
    MxRun r;
    memset(&r, 0, sizeof r);
    r.tmpl = tmpl;
    r.cfg = *mx;
    mx_cursor_load(&r);
    if (r.next_batch[0])
        fprintf(stderr, "[matrix] resuming after %s\n", r.next_batch);
    fprintf(stderr, "[matrix] bot online, syncing %s (Ctrl-C to stop)\n",
            mx->homeserver);

    for (;;) {
        if (r.npend == 0) mx_sync(&r, 30);
        while (r.npend > 0) {
            char room[288], sender[256];
            snprintf(room, sizeof room, "%s", r.pend[0].room);
            snprintf(sender, sizeof sender, "%s", r.pend[0].sender);
            char *text = r.pend[0].body;
            r.pend[0].body = NULL;
            mx_pend_shift(&r, 0);

            fprintf(stderr, "[matrix] %s: %.80s\n", room, text);
            if (str_starts(text, "/reset")) {
                agent_clear_session(mx_agent_for(&r, room));
                mx_send(&r, room, "(history cleared)");
            } else if (str_starts(text, "/help") || str_starts(text, "/start")) {
                mx_send(&r, room, "clawc online. send a task; /reset clears "
                                  "history. risky actions: reply to the prompt with /yes or /no.");
            } else {
                Agent *a = mx_agent_for(&r, room);
                MxCtx ctx;
                ctx.run = &r;
                ctx.room = room;
                ctx.sender = sender;
                a->confirm = tmpl->auto_approve ? NULL : mx_confirm;
                a->cb_user = &ctx;
                Buf reply;
                buf_init(&reply);
                agent_prompt(a, text, &reply);
                mx_send(&r, room, reply.s && reply.s[0] ? reply.s : "(no output)");
                buf_free(&reply);
            }
            free(text);
            if (mx->once) return 0;
        }
    }
}
