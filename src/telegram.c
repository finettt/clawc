/* clawc - telegram.c: claw channel over the Telegram Bot API.
   Long polling (getUpdates), per-chat agent sessions, chat allowlist,
   live status + streamed replies, confirmations for risky tools. */
#define _XOPEN_SOURCE 700
#include "telegram.h"
#include "json.h"
#include "http.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TG_MSG_LIMIT 4096
#define TG_STREAM_EDIT_MIN_S 2    /* seconds between message edits */
#define TG_MAX_SESSIONS 64
#define TG_MAX_PENDING 32
#define TG_CONFIRM_SECS 120

typedef struct {
    long long chat_id;
    Agent agent;
    int used;
} Session;

static Session sessions[TG_MAX_SESSIONS];

/* run state shared between the main poll loop and the confirmation loop */
typedef struct {
    TgCfg tg;
    const char *workspace; /* for the persistent update offset */
    long long offset;
    struct {
        long long chat_id, sender_id;
        char *text;        /* message text, or callback data when is_cb */
        int is_cb;         /* 1 = inline button press (callback_query) */
        char cb_id[64];    /* callback query id for answerCallbackQuery */
        long long msg_id;  /* message the press was on (0 = unknown) */
        char *media_path;  /* saved photo/voice/document (vision = photos) */
    } pending[TG_MAX_PENDING];
    int npending;
    char bot_user[64];   /* our @username, for group mention gating */
} TgRun;

/* per-reply context for UX callbacks */
typedef struct {
    TgRun *run;
    long long chat_id, sender_id;
    int status_msg_id;   /* the single working message: placeholder -> reply */
    time_t last_status_edit;
    Buf stream_text;
    Buf displayed;    /* last text actually sent to Telegram */
    time_t last_edit;
    int edits;
    int ever_streamed;
} TgCtx;

static Session *session_for(const Agent *tmpl, long long chat_id) {
    int i, free_slot = -1;
    for (i = 0; i < TG_MAX_SESSIONS; i++) {
        if (sessions[i].used && sessions[i].chat_id == chat_id) return &sessions[i];
        if (!sessions[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {
        fprintf(stderr, "[telegram] session table full, resetting oldest\n");
        free_slot = 0;
        agent_free(&sessions[0].agent);
        sessions[0].used = 0;
    }
    Session *s = &sessions[free_slot];
    s->used = 1;
    s->chat_id = chat_id;
    agent_init(&s->agent);
    s->agent.llm = tmpl->llm;             /* share config strings (immutable) */
    s->agent.tools = tmpl->tools;
    s->agent.max_steps = tmpl->max_steps;
    if (tmpl->tools.workspace) {
        Buf p;
        buf_init(&p);
        buf_printf(&p, "%s/history/tg-%lld.jsonl", tmpl->tools.workspace, chat_id);
        agent_open_session(&s->agent, p.s);
        buf_free(&p);
    }
    return s;
}

/* ---- Telegram API helpers ---- */

static HttpResp *tg_call(TgRun *run, const char *method, const char *body, int timeout_s) {
    Buf url;
    buf_init(&url);
    buf_printf(&url, "%s/bot%s/%s", run->tg.api_base, run->tg.token, method);
    HttpResp *r = http_post_json(url.s, NULL, 0, body, timeout_s, run->tg.insecure);
    buf_free(&url);
    return r;
}

static long long tg_send_message(TgRun *run, long long chat_id, const char *text) {
    Buf body;
    buf_init(&body);
    buf_printf(&body, "{\"chat_id\":%lld,\"text\":", chat_id);
    json_quote(&body, text);
    buf_puts(&body, "}");
    HttpResp *r = tg_call(run, "sendMessage", body.s, 60);
    long long mid = -1;
    if (r->status >= 200 && r->status < 300) {
        JVal *root = json_parse(r->body.s);
        JVal *res = root ? json_obj_get(root, "result") : NULL;
        mid = (long long)json_get_num(res, "message_id", -1);
        json_free(root);
    } else {
        fprintf(stderr, "[telegram] sendMessage failed: %s\n",
                r->err ? r->err : r->body.s);
    }
    http_resp_free(r);
    buf_free(&body);
    return mid;
}

int tg_send_text(const TgCfg *tg, long long chat_id, const char *text) {
    TgRun run;
    memset(&run, 0, sizeof run);
    run.tg = *tg; /* shallow copy: strings stay owned by the caller */
    return tg_send_message(&run, chat_id, text) >= 0 ? 0 : -1;
}

static void tg_send_chunked(TgRun *run, long long chat_id, const char *text) {
    size_t len = strlen(text), off = 0;
    if (len == 0) return;
    while (off < len) {
        size_t n = len - off;
        if (n > TG_MSG_LIMIT - 64) {
            n = TG_MSG_LIMIT - 64;
            size_t cut = n;
            while (cut > 0 && text[off + cut] != '\n') cut--;
            if (cut > 0) n = cut + 1;
        }
        char *chunk = xmalloc(n + 1);
        memcpy(chunk, text + off, n);
        chunk[n] = 0;
        tg_send_message(run, chat_id, chunk);
        free(chunk);
        off += n;
    }
}

static void tg_edit_message(TgRun *run, long long chat_id, long long msg_id, const char *text) {
    Buf body;
    buf_init(&body);
    buf_printf(&body, "{\"chat_id\":%lld,\"message_id\":%lld,\"text\":", chat_id, msg_id);
    json_quote(&body, text);
    buf_puts(&body, "}");
    HttpResp *r = tg_call(run, "editMessageText", body.s, 30);
    if (r->status < 200 || r->status >= 300)
        fprintf(stderr, "[telegram] editMessageText failed: HTTP %ld\n", r->status);
    http_resp_free(r);
    buf_free(&body);
}

static void tg_chat_action(TgRun *run, long long chat_id) {
    Buf body;
    buf_init(&body);
    buf_printf(&body, "{\"chat_id\":%lld,\"action\":\"typing\"}", chat_id);
    HttpResp *r = tg_call(run, "sendChatAction", body.s, 15);
    http_resp_free(r);
    buf_free(&body);
}

static void tg_answer_callback(TgRun *run, const char *cb_id, const char *text) {
    Buf body;
    buf_init(&body);
    buf_puts(&body, "{\"callback_query_id\":");
    json_quote(&body, cb_id ? cb_id : "");
    buf_puts(&body, ",\"text\":");
    json_quote(&body, text ? text : "");
    buf_puts(&body, "}");
    HttpResp *r = tg_call(run, "answerCallbackQuery", body.s, 15);
    http_resp_free(r);
    buf_free(&body);
}

/* send with an inline keyboard; markup is raw JSON for reply_markup (or NULL) */
static long long tg_send_message_markup(TgRun *run, long long chat_id, const char *text,
                                        const char *markup) {
    Buf body;
    buf_init(&body);
    buf_printf(&body, "{\"chat_id\":%lld,\"text\":", chat_id);
    json_quote(&body, text);
    if (markup && markup[0]) {
        buf_printf(&body, ",\"reply_markup\":%s", markup);
    }
    buf_puts(&body, "}");
    HttpResp *r = tg_call(run, "sendMessage", body.s, 60);
    long long mid = -1;
    if (r->status >= 200 && r->status < 300) {
        JVal *root = json_parse(r->body.s);
        JVal *res = root ? json_obj_get(root, "result") : NULL;
        mid = (long long)json_get_num(res, "message_id", -1);
        json_free(root);
    } else {
        fprintf(stderr, "[telegram] sendMessage failed: %s\n",
                r->err ? r->err : r->body.s);
    }
    http_resp_free(r);
    buf_free(&body);
    return mid;
}

static int chat_allowed(const TgCfg *tg, long long chat_id, const char *chat_type);
static long long tg_num(JVal *o, const char *key) {
    double d = json_get_num(o, key, 0);
    if (!(d >= -9007199254740991.0 && d <= 9007199254740991.0)) return 0;
    long long v = (long long)d;
    return (double)v == d ? v : 0;
}
/* Telegram IDs are JSON numbers; reject absent, fractional or inexact values. */
static long long tg_sender(TgRun *run, JVal *object) {
    JVal *from = json_obj_get(object, "from");
    long long v = from ? tg_num(from, "id") : 0;
    if (v <= 0) return 0;
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", v);
    return csv_has(run->tg.users, buf) ? v : 0;
}

/* ---- update intake: everything goes through a small pending queue ---- */

static void pending_push(TgRun *run, long long chat_id, long long sender_id, const char *text,
                         int is_cb, const char *cb_id, long long msg_id,
                         const char *media_path) {
    if (run->npending >= TG_MAX_PENDING) return;
    run->pending[run->npending].chat_id = chat_id;
    run->pending[run->npending].sender_id = sender_id;
    run->pending[run->npending].text = xstrdup(text);
    run->pending[run->npending].is_cb = is_cb;
    snprintf(run->pending[run->npending].cb_id, sizeof run->pending[run->npending].cb_id,
             "%s", cb_id ? cb_id : "");
    run->pending[run->npending].msg_id = msg_id;
    run->pending[run->npending].media_path =
        media_path && media_path[0] ? xstrdup(media_path) : NULL;
    run->npending++;
}

static void pending_remove(TgRun *run, int idx) {
    free(run->pending[idx].text);
    free(run->pending[idx].media_path);
    memmove(&run->pending[idx], &run->pending[idx + 1],
            (size_t)(run->npending - idx - 1) * sizeof run->pending[0]);
    run->npending--;
}

/* ---- persistent getUpdates offset (at-most-once: restarts never replay) ---- */

static unsigned tok_hash(const char *s) {
    unsigned h = 5381;
    if (!s) return h;
    for (; *s; s++) h = h * 33 + (unsigned char)*s;
    return h;
}

static void offset_path(const TgRun *run, Buf *p, const char *suffix) {
    buf_init(p);
    buf_printf(p, "%s/history/tg-offset-%08x%s", run->workspace ? run->workspace : ".",
               tok_hash(run->tg.token), suffix ? suffix : "");
}

static long long offset_load(TgRun *run) {
    if (!run->workspace) return 0;
    Buf p;
    offset_path(run, &p, NULL);
    char *s = read_file(p.s);
    buf_free(&p);
    if (!s) return 0;
    long long v = 0;
    v = atoll(s);
    if (v < 0) v = 0;
    free(s);
    return v;
}

static void offset_save(TgRun *run) {
    if (!run->workspace) return;
    char *hd = path_join(run->workspace, "history");
    mkdir(hd, 0755); /* best-effort */
    free(hd);
    Buf p;
    offset_path(run, &p, NULL);
    char b[32];
    snprintf(b, sizeof b, "%lld\n", run->offset);
    cursor_save(p.s, b);
    buf_free(&p);
}

/* save one Telegram file into workspace/media/; returns malloc'd path or NULL */
static char *tg_download(TgRun *run, const char *file_id, const char *ext) {
    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\"file_id\":");
    json_quote(&b, file_id);
    buf_puts(&b, "}");
    HttpResp *r = tg_call(run, "getFile", b.s, 30);
    buf_free(&b);
    char *out = NULL;
    if (r->status == 200) {
        JVal *root = json_parse(r->body.s);
        JVal *res = root ? json_obj_get(root, "result") : NULL;
        const char *fp = res ? json_get_str(res, "file_path") : NULL;
        if (fp) {
            char *dir = path_join(run->workspace, "media");
            mkdir(dir, 0755);
            char name[128];
            snprintf(name, sizeof name, "%s%s", file_id, ext ? ext : "");
            out = path_join(dir, name);
            free(dir);
            Buf url;
            buf_init(&url);
            buf_printf(&url, "%s/file/bot%s/%s",
                       run->tg.api_base && run->tg.api_base[0] ? run->tg.api_base
                           : "https://api.telegram.org",
                       run->tg.token, fp);
            HttpResp *f = http_request("GET", url.s, NULL, 0, NULL,
                                       60, run->tg.insecure);
            if (f->status == 200 && f->body.len > 0) {
                write_file(out, f->body.s);
            } else {
                free(out);
                out = NULL;
            }
            http_resp_free(f);
            buf_free(&url);
        }
        json_free(root);
    }
    http_resp_free(r);
    return out;
}

/* poll once; incoming messages land in the pending queue. 0=ok, -1=error */
static int tg_poll_updates(TgRun *run, int timeout_s) {
    int rc = 0;
    long long off_before = run->offset;
    Buf body;
    buf_init(&body);
    buf_printf(&body, "{\"timeout\":%d,\"offset\":%lld,"
                      "\"allowed_updates\":[\"message\",\"callback_query\"]}",
               timeout_s, run->offset);
    HttpResp *r = tg_call(run, "getUpdates", body.s, timeout_s + 15);
    buf_free(&body);

    if (r->status == 429) {
        JVal *root = json_parse(r->body.s);
        JVal *params = root ? json_obj_get(root, "parameters") : NULL;
        double retry = params ? json_get_num(params, "retry_after", 5) : 5;
        json_free(root);
        fprintf(stderr, "[telegram] rate limited, sleeping %.0fs\n", retry);
        sleep((unsigned)(retry > 0 ? retry : 5));
        rc = -1;
    } else if (r->status < 200 || r->status >= 300) {
        fprintf(stderr, "[telegram] getUpdates HTTP %ld: %.200s\n", r->status,
                r->err ? r->err : r->body.s ? r->body.s : "");
        rc = -1;
    } else {
        JVal *root = json_parse(r->body.s);
        JVal *result = root ? json_obj_get(root, "result") : NULL;
        if (result && result->type == J_ARR) {
            int i;
            for (i = 0; i < result->nitems; i++) {
                JVal *u = result->items[i];
                double uid = json_get_num(u, "update_id", -1);
                if (uid >= 0) run->offset = (long long)uid + 1;
                JVal *msg = json_obj_get(u, "message");
                if (msg) {
                    const char *text = json_get_str(msg, "text");
                    const char *caption = json_get_str(msg, "caption");
                    JVal *chat = json_obj_get(msg, "chat");
                    long long cid = chat ? tg_num(chat, "id") : 0;
                    const char *ctype = json_get_str(chat, "type");
                    long long sender = tg_sender(run, msg);
                    if (!cid || !sender || !chat_allowed(&run->tg, (long long)cid, ctype)) continue;
                    long long mid = tg_num(msg, "message_id");
                    int is_group = ctype &&
                        (str_eq(ctype, "group") || str_eq(ctype, "supergroup"));

                    /* group gating: only when mentioned (or replying to us) */
                    char stripped[4096];
                    stripped[0] = 0;
                    const char *say = text ? text : caption;
                    if (is_group) {
                        int mentioned = 0;
                        char at[80];
                        snprintf(at, sizeof at, "@%s", run->bot_user);
                        if (say && run->bot_user[0] && strstr(say, at)) {
                            mentioned = 1;
                            const char *p = say;
                            char *o = stripped;
                            while (*p && o < stripped + sizeof stripped - 1) {
                                if (p == strstr(p, at)) p += strlen(at);
                                else *o++ = *p++;
                            }
                            *o = 0;
                            say = stripped;
                        }
                        JVal *rto = json_obj_get(msg, "reply_to_message");
                        JVal *rfrom = rto ? json_obj_get(rto, "from") : NULL;
                        JVal *bot = rfrom ? json_obj_get(rfrom, "is_bot") : NULL;
                        int is_bot = bot && ((bot->type == J_BOOL && bot->boolean) ||
                                             (bot->type == J_NUM && bot->num != 0));
                        if (!mentioned && !(is_bot > 0)) continue;
                    }

                    /* media: save photo/voice/document into workspace/media */
                    char *media_path = NULL;
                    JVal *photo = json_obj_get(msg, "photo");
                    JVal *voice = json_obj_get(msg, "voice");
                    JVal *doc = json_obj_get(msg, "document");
                    const char *fid = NULL;
                    const char *ext = "";
                    const char *kind = NULL;
                    if (photo && photo->type == J_ARR && photo->nitems > 0) {
                        fid = json_get_str(photo->items[photo->nitems - 1], "file_id");
                        ext = ".jpg"; kind = "photo";
                    } else if (voice) {
                        fid = json_get_str(voice, "file_id");
                        ext = ".ogg"; kind = "voice note";
                    } else if (doc) {
                        fid = json_get_str(doc, "file_id");
                        ext = ""; kind = "document";
                    }
                    if (fid && fid[0]) media_path = tg_download(run, fid, ext);

                    if (!say || !say[0]) {
                        static char fallback[128];
                        snprintf(fallback, sizeof fallback, "[%s sent: %s]",
                                 kind ? kind : "file", "see attached path below");
                        say = fallback;
                    }
                    if (media_path) {
                        char full[4200];
                        snprintf(full, sizeof full, "%s\n[attached %s: %s]",
                                 say, kind ? kind : "file", media_path);
                        pending_push(run, (long long)cid, sender, full, 0, NULL, mid, media_path);
                        free(media_path);
                    } else if (say && say[0]) {
                        pending_push(run, (long long)cid, sender, say, 0, NULL, mid, NULL);
                    }
                    continue;
                }
                JVal *cq = json_obj_get(u, "callback_query");
                if (cq) {
                    const char *data = json_get_str(cq, "data");
                    const char *cqid = json_get_str(cq, "id");
                    JVal *cmsg = json_obj_get(cq, "message");
                    JVal *chat = cmsg ? json_obj_get(cmsg, "chat") : NULL;
                    long long cid = chat ? tg_num(chat, "id") : 0;
                    long long cmid = cmsg ? tg_num(cmsg, "message_id") : 0;
                    const char *ctype = json_get_str(chat, "type");
                    long long sender = tg_sender(run, cq);
                    if (!data || !cid || !sender || !chat_allowed(&run->tg, (long long)cid, ctype)) {
                        if (cqid) tg_answer_callback(run, cqid, "unauthorized");
                        continue;
                    }
                    pending_push(run, (long long)cid, sender, data, 1, cqid, (long long)cmid, NULL);
                }
            }
        } else {
            JVal *err = root ? json_obj_get(root, "error") : NULL;
            if (err)
                fprintf(stderr, "[telegram] API error: %s\n",
                        json_get_str(err, "description") ? json_get_str(err, "description") : "?");
            rc = -1;
        }
        json_free(root);
    }
    http_resp_free(r);
    if (run->offset != off_before && run->offset > 0) offset_save(run);
    return rc;
}

/* ---- UX callbacks ---- */

static void cb_on_status(void *user, const char *phase) {
    TgCtx *ctx = user;
    tg_chat_action(ctx->run, ctx->chat_id);
    if (ctx->stream_text.len > 0) return; /* reply flowing: placeholder must not overwrite it */
    time_t now = time(NULL);
    if (ctx->status_msg_id == 0) {
        Buf t;
        buf_init(&t);
        buf_printf(&t, "\xe2\x8f\xb3 %s\xe2\x80\xa6", phase); /* hourglass + phase */
        ctx->status_msg_id = tg_send_message(ctx->run, ctx->chat_id, t.s);
        ctx->last_status_edit = now;
        buf_free(&t);
    } else if (now - ctx->last_status_edit >= 2) {
        Buf t;
        buf_init(&t);
        buf_printf(&t, "\xe2\x8f\xb3 %s\xe2\x80\xa6", phase);
        tg_edit_message(ctx->run, ctx->chat_id, ctx->status_msg_id, t.s);
        ctx->last_status_edit = now;
        buf_free(&t);
    }
}

static void ctx_snapshot(TgCtx *ctx) {
    ctx->displayed.len = 0;   /* reuse the already-allocated buffer */
    ctx->displayed.s[0] = 0;
    buf_add(&ctx->displayed, ctx->stream_text.s, ctx->stream_text.len);
}

static void cb_on_delta(void *user, const char *text, size_t n) {
    TgCtx *ctx = user;
    if (ctx->stream_text.len > 4000) return; /* display cap */
    buf_add(&ctx->stream_text, text, n);
    ctx->ever_streamed = 1;
    time_t now = time(NULL);
    if (ctx->status_msg_id == 0) {
        ctx->status_msg_id = tg_send_message(ctx->run, ctx->chat_id, ctx->stream_text.s);
        ctx->last_edit = now;
        ctx->edits++;
        ctx_snapshot(ctx);
    } else if (ctx->edits == 0 ||
               (now - ctx->last_edit >= TG_STREAM_EDIT_MIN_S && ctx->edits < 24)) {
        /* first token turns the working message into the reply immediately */
        tg_edit_message(ctx->run, ctx->chat_id, ctx->status_msg_id, ctx->stream_text.s);
        ctx->last_edit = now;
        ctx->edits++;
        ctx_snapshot(ctx);
    }
}

static int cb_confirm(void *user, const char *risk_desc) {
    TgCtx *ctx = user;
    TgRun *run = ctx->run;
    Buf t;
    buf_init(&t);
    buf_printf(&t, "\xe2\x9a\xa0\xef\xb8\x8f Confirm action:\n%.900s", risk_desc);
    static const char *confirm_kb =
        "{\"inline_keyboard\":[["
        "{\"text\":\"\\u2705 Approve\",\"callback_data\":\"conf:yes\"},"
        "{\"text\":\"\\u274c Cancel\",\"callback_data\":\"conf:no\"}"
        "]]}";
    long long msg_id = tg_send_message_markup(run, ctx->chat_id, t.s, confirm_kb);
    buf_free(&t);
    tg_chat_action(run, ctx->chat_id);

    int window = TG_CONFIRM_SECS;
    {
        const char *cs = getenv("CLAWC_CONFIRM_SECS");
        if (cs && atoi(cs) > 0) window = atoi(cs);
    }
    time_t deadline = time(NULL) + window;
    int decided = -1;
    while (decided < 0 && time(NULL) < deadline) {
        int i;
        for (i = 0; i < run->npending; i++) {
            if (run->pending[i].chat_id != ctx->chat_id) continue;
            int yes = 0, no = 0;
            if (run->pending[i].is_cb) {
                yes = str_eq(run->pending[i].text, "conf:yes");
                no = str_eq(run->pending[i].text, "conf:no");
                if (!yes && !no) continue; /* unrelated button: leave it */
                /* press from a different (older) confirm message, or one we
                   cannot tie to this prompt (unknown ids): never decides */
                if (msg_id <= 0 || run->pending[i].msg_id <= 0 ||
                    run->pending[i].msg_id != msg_id) {
                    tg_answer_callback(run, run->pending[i].cb_id, "expired");
                    pending_remove(run, i);
                    i--;
                    continue;
                }
            } else {
                yes = str_starts(run->pending[i].text, "/yes");
                no = str_starts(run->pending[i].text, "/no");
                if (!yes && !no) continue;
            }
            /* Text must have been sent after the prompt; a different authorized
               user must never decide the requester's confirmation. */
            if (run->pending[i].sender_id != ctx->sender_id ||
                (!run->pending[i].is_cb &&
                 (msg_id <= 0 || run->pending[i].msg_id <= msg_id))) {
                if (run->pending[i].is_cb)
                    tg_answer_callback(run, run->pending[i].cb_id, "unauthorized or expired");
                pending_remove(run, i);
                i--;
                continue;
            }
            if (run->pending[i].is_cb)
                tg_answer_callback(run, run->pending[i].cb_id,
                                   yes ? "approved" : "cancelled");
            pending_remove(run, i);
            decided = yes;
            break;
        }
        if (decided < 0) tg_poll_updates(run, 10);
    }

    if (decided < 0) {
        if (msg_id > 0)
            tg_edit_message(run, ctx->chat_id, msg_id,
                            "(timed out waiting for confirmation - cancelled)");
        tg_send_message(run, ctx->chat_id, "(cancelled)");
        return 0;
    }
    if (msg_id > 0)
        tg_edit_message(run, ctx->chat_id, msg_id,
                        decided ? "(approved \xe2\x9c\x85)" : "(cancelled \xe2\x9d\x8c)");
    return decided;
}

static int chat_allowed(const TgCfg *tg, long long chat_id, const char *chat_type) {
    int i;
    for (i = 0; i < tg->nallow; i++)
        if (tg->allow[i] == chat_id) return 1;
    if (tg->nallow > 0) return 0;
    /* no allowlist: private chats + groups (group traffic is mention-gated at
       intake: the bot only hears @mentions and replies-to-bot there) */
    return chat_type && (str_eq(chat_type, "private") ||
                         str_eq(chat_type, "group") ||
                         str_eq(chat_type, "supergroup"));
}

/* ---- commands & message handling ---- */

static void cmd_status(TgRun *run, long long chat_id, Agent *agent, Session *s) {
    Buf t;
    buf_init(&t);
    buf_printf(&t, "clawc " CLAWC_VERSION "\n"
                   "model: %s (%s)\n"
                   "api: %.80s\n"
                   "workspace: %.120s\n"
                   "session: %s (%d messages)\n"
                   "sandbox: %s\n"
                   "confirmations: %s\n",
               agent->llm.model ? agent->llm.model : "?",
               agent->llm.api_kind == API_ANTHROPIC ? "anthropic" : "openai-compatible",
               agent->llm.api_url ? agent->llm.api_url : "?",
               agent->tools.workspace ? agent->tools.workspace : "?",
               s->agent.session_name ? s->agent.session_name : "?",
               s->agent.nhist,
               agent->tools.sandbox ? agent->tools.sandbox : "off",
               run->tg.auto_approve ? "off (--yes)" : "on");
    tg_send_message(run, chat_id, t.s);
    buf_free(&t);
}

static void cmd_goal(TgRun *run, long long chat_id, Agent *agent, const char *args) {
    Buf out;
    buf_init(&out);
    while (*args == ' ') args++;
    if (!args[0]) {
        char *gp = path_join(agent->tools.workspace, "GOALS.md");
        char *g = read_file(gp);
        free(gp);
        if (g && g[0]) buf_printf(&out, "%.1400s", g);
        else buf_puts(&out, "(no goals yet - /goal add <task>)");
        free(g);
    } else if (str_starts(args, "add ")) {
        const char *task = args + 4;
        while (*task == ' ') task++;
        if (task[0]) goal_add_text(&agent->tools, task, NULL, NULL, &out);
        else buf_puts(&out, "usage: /goal add <task>");
    } else if (str_starts(args, "done ")) {
        goal_done_text(&agent->tools, args + 5, &out);
    } else if (str_starts(args, "rm ")) {
        goal_rm_text(&agent->tools, args + 3, &out);
    } else {
        buf_puts(&out, "usage: /goal | /goal add <task> | /goal done <id> | /goal rm <id>");
    }
    tg_send_message(run, chat_id, out.s);
    buf_free(&out);
}

static void cmd_memory(TgRun *run, long long chat_id, Agent *agent) {
    char *mem = tools_read_memory(&agent->tools);
    Buf t;
    buf_init(&t);
    if (mem && mem[0]) {
        buf_printf(&t, "%.1200s", mem);
        if (strlen(mem) > 1200) buf_puts(&t, "\n[... truncated]");
    } else {
        buf_puts(&t, "(memory is empty)");
    }
    tg_send_message(run, chat_id, t.s);
    buf_free(&t);
    free(mem);
}

static const char *START_TEXT =
    "clawc agent online - local-first claw-style agent.\n\n"
    "Commands: /goal (list/add/done/rm) manages your autonomous goals.\n"
    "Commands:\n"
    "/status - what I am and how I am configured\n"
    "/memory - show what I remember\n"
    "/model  - current model\n"
    "/reset  - clear this chat's history\n"
    "/help   - this message\n\n"
    "Dangerous actions ask for confirmation (/yes or /no).";

static void tg_reply(TgRun *run, Agent *tmpl, long long chat_id, long long sender_id,
                     const char *text,
                     const char *media_path) {
    Session *s = session_for(tmpl, chat_id);
    fprintf(stderr, "[telegram] chat %lld: %.80s\n", chat_id, text);

    if (str_starts(text, "/reset")) {
        agent_clear_session(&s->agent);
        tg_send_message(run, chat_id, "(history cleared)");
        return;
    }
    if (str_starts(text, "/start") || str_starts(text, "/help")) {
        tg_send_message(run, chat_id, START_TEXT);
        return;
    }
    if (str_starts(text, "/status")) {
        cmd_status(run, chat_id, tmpl, s);
        return;
    }
    if (str_starts(text, "/goal")) {
        const char *p = text + 5;
        if (*p == '@') { while (*p && *p != ' ') p++; }
        cmd_goal(run, chat_id, &s->agent, p);
        return;
    }
    if (str_starts(text, "/memory")) {
        cmd_memory(run, chat_id, tmpl);
        return;
    }
    if (str_starts(text, "/model")) {
        Buf t;
        buf_init(&t);
        buf_printf(&t, "model: %s\napi: %s (%s)",
                   tmpl->llm.model ? tmpl->llm.model : "?",
                   tmpl->llm.api_kind == API_ANTHROPIC ? "anthropic" : "openai-compatible",
                   tmpl->llm.api_url ? tmpl->llm.api_url : "?");
        tg_send_message(run, chat_id, t.s);
        buf_free(&t);
        return;
    }

    TgCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    buf_init(&ctx.stream_text);
    buf_init(&ctx.displayed);   /* shown text; freed at end of tg_reply */
    ctx.run = run;
    ctx.chat_id = chat_id;
    ctx.sender_id = sender_id;

    Agent *a = &s->agent;
    a->cb_user = &ctx;
    a->on_status = cb_on_status;
    a->on_delta = cb_on_delta;
    a->auto_approve = run->tg.auto_approve;
    a->confirm = run->tg.auto_approve ? NULL : cb_confirm;

    Buf reply;
    buf_init(&reply);
    int rc = agent_prompt_img(a, text, media_path, &reply);
    buf_trim(&reply);
    if (rc != 0 && reply.len == 0) buf_puts(&reply, "(agent error)");

    /* finalize: the working message becomes the reply (no checkmark, no second message) */
    if (ctx.status_msg_id > 0) {
        Buf final;
        buf_init(&final);
        buf_printf(&final, "%.3800s%s", reply.s, reply.len > 3800 ? "\n[...]" : "");
        if (!ctx.ever_streamed || strcmp(ctx.displayed.s, final.s) != 0)
            tg_edit_message(run, chat_id, ctx.status_msg_id, final.s);
        buf_free(&final);
        if (reply.len > 3800) tg_send_chunked(run, chat_id, reply.s + 3800);
    } else {
        tg_send_chunked(run, chat_id, reply.s);
    }

    /* detach stack callbacks before the session outlives this frame */
    a->cb_user = NULL;
    a->on_status = NULL;
    a->on_delta = NULL;
    a->confirm = NULL;
    buf_free(&ctx.stream_text);
    buf_free(&ctx.displayed);
    buf_free(&reply);
}

int telegram_run(Agent *tmpl, const TgCfg *tg) {
    if (!sender_ready("telegram", tg->users)) return 2;
    if (!tg->token || !tg->token[0]) {
        fprintf(stderr, "[telegram] no bot token (use -token or CLAWC_TELEGRAM_TOKEN)\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);

    TgRun run;
    memset(&run, 0, sizeof run);
    run.tg = *tg;
    run.workspace = tmpl->tools.workspace;
    run.offset = offset_load(&run);
    if (run.offset > 0)
        fprintf(stderr, "[telegram] resuming after update %lld\n", run.offset - 1);

    /* learn our @username for group mention gating */
    {
        HttpResp *r = tg_call(&run, "getMe", "{}", 30);
        if (r->status == 200) {
            JVal *root = json_parse(r->body.s);
            JVal *res = root ? json_obj_get(root, "result") : NULL;
            const char *un = res ? json_get_str(res, "username") : NULL;
            if (un) snprintf(run.bot_user, sizeof run.bot_user, "%s", un);
            json_free(root);
        }
        http_resp_free(r);
    }

    fprintf(stderr, "[telegram] bot online, polling %s (Ctrl-C to stop)\n", tg->api_base);

    int handled = 0, failures = 0;
    for (;;) {
        int before = run.npending;
        if (tg_poll_updates(&run, tg->poll_timeout > 0 ? tg->poll_timeout : 50) == 0)
            failures = 0;
        else
            failures++;

        while (run.npending > 0) {
            long long cid = run.pending[0].chat_id, sender = run.pending[0].sender_id;
            int is_cb = run.pending[0].is_cb;
            char cb_id[64];
            snprintf(cb_id, sizeof cb_id, "%s", run.pending[0].cb_id);
            char *text = run.pending[0].text;
            run.pending[0].text = NULL;
            char *mp = run.pending[0].media_path;
            run.pending[0].media_path = NULL;
            pending_remove(&run, 0);

            if (is_cb) {
                /* confirm buttons are consumed inside cb_confirm; anything
                   reaching here is a stale/unknown button press */
                tg_answer_callback(&run, cb_id, "expired");
                free(text);
                continue;
            }
            tg_reply(&run, tmpl, cid, sender, text, mp);
            free(mp);
            free(text);
            handled++;
            if (tg->once) return 0;
        }
        (void)before;

        if (failures > 0) {
            unsigned backoff = failures > 6 ? 60 : (unsigned)(1 << (failures > 5 ? 5 : failures));
            fprintf(stderr, "[telegram] backing off %us\n", backoff);
            sleep(backoff);
        }
    }
}
