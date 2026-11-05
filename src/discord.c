/* clawc - discord.c: Discord channel (gateway WebSocket + REST), v2.
   v2 adds: component buttons on confirmations (edit-in-place like telegram),
   slash commands (/claw, /goal), interaction stale guard. Buttons appear ONLY
   on confirm messages. Text /yes //no still works. Sessions per channel;
   at-most-once via gateway seq + resume. */
#define _XOPEN_SOURCE 700
#include "discord.h"
#include "http.h"
#include "json.h"
#include "util.h"
#include "ws.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DC_MAX_PEND 8

typedef struct {
    char channel[128];
    char sender[72], msg_id[72];
    char *body;
    char *img_path;  /* saved attachment image for vision (or NULL) */
} DcEvent;

typedef struct {
    char inter_id[72];
    char inter_token[300];
    char custom_id[40];
    char msg_id[72];
    char sender[72];
} DcInter;

typedef struct {
    Agent *tmpl;
    DcCfg cfg;
    WsConn ws;
    long long seq;
    char session_id[256];
    long hb_ms;
    time_t next_hb;
    DcEvent pend[DC_MAX_PEND];
    int npend;
    DcInter conf[8];
    int nconf;
    char app_id[72];
    int cmds_registered;
    int running;
} DcRun;

static int dc_allow(const char *list, const char *id) {
    if (!list || !list[0]) return 1;
    return csv_has(list, id);
}

/* REST helper; returns malloc'd response body or NULL */
static char *dc_rest(DcRun *r, const char *path, const char *json) {
    Buf url;
    buf_init(&url);
    const char *api = (r->cfg.api && r->cfg.api[0]) ? r->cfg.api
                     : "https://discord.com/api/v10";
    buf_printf(&url, "%s%s", api, path);
    char auth[300];
    snprintf(auth, sizeof auth, "Authorization: Bot %s", r->cfg.token);
    const char *hdrs[3];
    hdrs[0] = auth;
    hdrs[1] = "Content-Type: application/json";
    hdrs[2] = NULL;
    HttpResp *res = http_request("POST", url.s, hdrs, 2, json, 30,
                                 r->tmpl->tools.insecure_tls);
    char *body = NULL;
    if (res->status < 200 || res->status >= 300)
        fprintf(stderr, "[discord] REST %s HTTP %ld: %.120s\n", path,
                res->status, res->err ? res->err : res->body.s ? res->body.s : "");
    else if (res->body.s)
        body = xstrdup(res->body.s);
    http_resp_free(res);
    buf_free(&url);
    return body;
}

static char *dc_send_full(DcRun *r, const char *channel, const char *text,
                          const char *components) {
    char path[200];
    snprintf(path, sizeof path, "/channels/%s/messages", channel);
    Buf body;
    buf_init(&body);
    buf_puts(&body, "{\"content\":");
    json_quote(&body, text);
    if (components && components[0])
        buf_printf(&body, ",\"components\":%s", components);
    buf_puts(&body, "}");
    char *resp = dc_rest(r, path, body.s);
    buf_free(&body);
    char *id = NULL;
    if (resp) {
        JVal *root = json_parse(resp);
        const char *mid = root ? json_get_str(root, "id") : NULL;
        if (mid) id = xstrdup(mid);
        json_free(root);
        free(resp);
    }
    return id;
}

static void dc_send(DcRun *r, const char *channel, const char *text) {
    free(dc_send_full(r, channel, text, NULL));
}

static void dc_gateway_send(DcRun *r, int op, const char *d) {
    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\"op\":%d,\"d\":%s}", op, d ? d : "null");
    ws_send_text(&r->ws, b.s, b.len);
    buf_free(&b);
}

/* interaction callback: type 4 = reply, type 7 = edit the prompt in place */
static void dc_inter_ack(DcRun *r, const char *iid, const char *tok,
                         int type, const char *content, int with_components) {
    char path[420];
    snprintf(path, sizeof path, "/interactions/%s/%s/callback", iid, tok);
    Buf body;
    buf_init(&body);
    buf_printf(&body, "{\"type\":%d,\"data\":{\"content\":", type);
    json_quote(&body, content);
    if (!with_components) buf_puts(&body, ",\"components\":[]");
    if (type == 4) buf_puts(&body, ",\"flags\":64");
    buf_puts(&body, "}}");
    free(dc_rest(r, path, body.s));
    buf_free(&body);
}

static void dc_cursor_path(DcRun *r, char *dst, size_t n) {
    unsigned long h = 5381;
    const char *p = r->cfg.token;
    while (*p) h = h * 33 + (unsigned char)*p++;
    snprintf(dst, n, "%s/history/discord-cursor-%08lx", r->tmpl->tools.workspace, h);
}

static void dc_cursor_save(DcRun *r) {
    char p[640];
    dc_cursor_path(r, p, sizeof p);
    Buf v;
    buf_init(&v);
    buf_printf(&v, "%lld %s", r->seq, r->session_id);
    cursor_save(p, v.s);
    buf_free(&v);
}

static void dc_cursor_load(DcRun *r) {
    char p[640];
    dc_cursor_path(r, p, sizeof p);
    char *v = read_file(p);
    if (v) {
        long long seq = 0;
        char sid[256];
        sid[0] = 0;
        if (sscanf(v, "%lld %255s", &seq, sid) >= 1) {
            r->seq = seq;
            snprintf(r->session_id, sizeof r->session_id, "%s", sid);
        }
        free(v);
    }
}

/* save one attachment URL into workspace/media/; returns malloc'd path or NULL */
static char *dc_download(DcRun *r, const char *url, const char *filename) {
    HttpResp *f = http_request("GET", url, NULL, 0, NULL, 60,
                               r->tmpl->tools.insecure_tls);
    char *out = NULL;
    if (f->status == 200 && f->body.len > 0) {
        const char *base = filename && filename[0] ? filename : "file";
        const char *sl = strrchr(base, '/');
        const char *bs = strrchr(base, '\\');
        if (sl) base = sl + 1;
        if (bs && bs + 1 > base) base = bs + 1;
        if (!base[0]) base = "file";
        char *dir = path_join(r->tmpl->tools.workspace, "media");
        mkdir(dir, 0755);
        out = path_join(dir, base);
        free(dir);
        FILE *fp = fopen(out, "wb");
        if (fp) {
            fwrite(f->body.s, 1, f->body.len, fp);
            fclose(fp);
        } else {
            free(out);
            out = NULL;
        }
    }
    http_resp_free(f);
    return out;
}

static int dc_is_img(const char *fn, const char *ct) {
    if (ct && str_starts(ct, "image/")) return 1;
    return fn && (str_ends(fn, ".png") || str_ends(fn, ".jpg") ||
                  str_ends(fn, ".jpeg") || str_ends(fn, ".gif") ||
                  str_ends(fn, ".webp"));
}

static void dc_dispatch(DcRun *r, JVal *d, const char *t, long long s) {
    if (s > 0) r->seq = s;
    if (t && str_eq(t, "READY")) {
        const char *sid = json_get_str(d, "session_id");
        if (sid) snprintf(r->session_id, sizeof r->session_id, "%s", sid);
        JVal *app = json_obj_get(d, "application");
        const char *aid = app ? json_get_str(app, "id") : NULL;
        if (aid) snprintf(r->app_id, sizeof r->app_id, "%s", aid);
        fprintf(stderr, "[discord] ready (session %s)\n", r->session_id);
    } else if (t && str_eq(t, "INTERACTION_CREATE")) {
        int type = (int)json_get_num(d, "type", 0);
        const char *iid = json_get_str(d, "id");
        const char *itok = json_get_str(d, "token");
        const char *chan = json_get_str(d, "channel_id");
        const char *guild = json_get_str(d, "guild_id");
        JVal *member = json_obj_get(d, "member");
        JVal *actor = guild ? (member ? json_obj_get(member, "user") : NULL)
                            : json_obj_get(d, "user");
        const char *sender = actor ? json_get_str(actor, "id") : NULL;
        /* Slash commands and buttons must obey the same allowlists as messages. */
        if (!chan || strlen(chan) >= sizeof r->pend[0].channel ||
            !sender || strlen(sender) >= sizeof r->pend[0].sender ||
            !csv_has(r->cfg.users, sender) ||
            !dc_allow(r->cfg.guilds, guild ? guild : "dm") ||
            !dc_allow(r->cfg.channels, chan)) return;
        JVal *data = json_obj_get(d, "data");
        const char *cid = data ? json_get_str(data, "custom_id") : NULL;
        if (type == 3 && iid && itok && cid &&
            (str_eq(cid, "conf:yes") || str_eq(cid, "conf:no"))) {
            JVal *m = json_obj_get(d, "message");
            const char *mid = m ? json_get_str(m, "id") : NULL;
            if (r->nconf < 8) {
                DcInter *x = &r->conf[r->nconf++];
                snprintf(x->inter_id, sizeof x->inter_id, "%s", iid);
                snprintf(x->inter_token, sizeof x->inter_token, "%s", itok);
                snprintf(x->custom_id, sizeof x->custom_id, "%s", cid);
                snprintf(x->msg_id, sizeof x->msg_id, "%s",
                         mid && strlen(mid) < sizeof x->msg_id ? mid : "");
                snprintf(x->sender, sizeof x->sender, "%s", sender);
            }
        } else if (type == 2 && iid && itok) {
            const char *name = data ? json_get_str(data, "name") : NULL;
            /* defer immediately (3s rule); the real reply follows as a message */
            dc_inter_ack(r, iid, itok, 4, "\xe2\x8f\xb3 thinking\xe2\x80\xa6", 1);
            JVal *opts = data ? json_obj_get(data, "options") : NULL;
            const char *task = NULL;
            if (opts && opts->type == J_ARR && opts->nitems > 0)
                task = json_get_str(opts->items[0], "value");
            const char *chan = json_get_str(d, "channel_id");
            if (name && str_eq(name, "claw") && task && chan) {
                if (r->npend < DC_MAX_PEND) {
                    snprintf(r->pend[r->npend].channel, sizeof r->pend[0].channel,
                             "%s", chan);
                    snprintf(r->pend[r->npend].sender, sizeof r->pend[0].sender, "%s", sender);
                    r->pend[r->npend].msg_id[0] = 0;
                    r->pend[r->npend].body = xstrdup(task);
                    r->pend[r->npend].img_path = NULL;
                    r->npend++;
                }
            } else if (name && str_eq(name, "goal") && chan) {
                Buf out;
                buf_init(&out);
                if (task && task[0])
                    goal_add_text(&r->tmpl->tools, task, NULL, NULL, &out);
                else {
                    char *gp = path_join(r->tmpl->tools.workspace, "GOALS.md");
                    char *g = read_file(gp);
                    free(gp);
                    if (g && g[0]) buf_printf(&out, "%.1200s", g);
                    else buf_puts(&out, "(no goals yet)");
                    free(g);
                }
                dc_send(r, chan, out.s);
                buf_free(&out);
            }
        }
    } else if (t && str_eq(t, "MESSAGE_CREATE")) {
        const char *chan = json_get_str(d, "channel_id");
        const char *content = json_get_str(d, "content");
        JVal *author = json_obj_get(d, "author");
        const char *sender = author ? json_get_str(author, "id") : NULL;
        const char *mid = json_get_str(d, "id");
        JVal *bot = author ? json_obj_get(author, "bot") : NULL;
        int is_bot = bot && ((bot->type == J_BOOL && bot->boolean) ||
                             (bot->type == J_NUM && bot->num != 0));
        const char *gid = json_get_str(d, "guild_id");
        JVal *atts = json_obj_get(d, "attachments");
        int natts = (atts && atts->type == J_ARR) ? atts->nitems : 0;
        if (is_bot > 0 || !chan || strlen(chan) >= sizeof r->pend[0].channel ||
            !sender || strlen(sender) >= sizeof r->pend[0].sender ||
            !csv_has(r->cfg.users, sender)) return;
        if ((!content || !content[0]) && natts == 0) return;
        if (!dc_allow(r->cfg.guilds, gid ? gid : "dm")) return;
        if (!dc_allow(r->cfg.channels, chan)) return;

        /* media: save attachments into workspace/media; images become vision */
        Buf full;
        buf_init(&full);
        if (content && content[0]) buf_puts(&full, content);
        char *img_path = NULL;
        int i;
        for (i = 0; i < natts && i < 8; i++) {
            JVal *at = atts->items[i];
            const char *url = json_get_str(at, "url");
            const char *fn = json_get_str(at, "filename");
            const char *ct = json_get_str(at, "content_type");
            if (!url || !url[0]) continue;
            char *path = dc_download(r, url, fn);
            if (!path) continue;
            int img = dc_is_img(fn, ct);
            if (img && !img_path) img_path = xstrdup(path);
            if (full.len > 0) buf_puts(&full, "\n");
            buf_printf(&full, "[attached %s: %s]", img ? "image" : "file", path);
            free(path);
        }
        if (r->npend < DC_MAX_PEND) {
            snprintf(r->pend[r->npend].channel, sizeof r->pend[0].channel, "%s", chan);
            snprintf(r->pend[r->npend].sender, sizeof r->pend[0].sender, "%s", sender);
            snprintf(r->pend[r->npend].msg_id, sizeof r->pend[0].msg_id, "%s",
                     mid && strlen(mid) < sizeof r->pend[0].msg_id ? mid : "");
            r->pend[r->npend].body = xstrdup(full.s);
            r->pend[r->npend].img_path = img_path;
            img_path = NULL;
            r->npend++;
        }
        free(img_path);
        buf_free(&full);
    }
}

/* confirm with buttons (edit-in-place on decision) + /yes //no fallback */
typedef struct {
    DcRun *run;
    const char *channel;
    const char *sender;
} DcCtx;

/* Snowflakes are decimal and ordered by creation time. */
static int dc_after(const char *a, const char *b) {
    if (!a || !*a || !b || !*b) return 0;
    const char *p;
    for (p = a; *p; p++) if (*p < '0' || *p > '9') return 0;
    for (p = b; *p; p++) if (*p < '0' || *p > '9') return 0;
    errno = 0;
    unsigned long long av = strtoull(a, NULL, 10);
    if (errno) return 0;
    unsigned long long bv = strtoull(b, NULL, 10);
    if (errno) return 0;
    /* Within a millisecond, worker bits need not preserve ordering. */
    return (av >> 22) > (bv >> 22);
}

static int dc_confirm(void *user, const char *desc) {
    DcCtx *ctx = user;
    DcRun *r = ctx->run;
    static const char *KB =
        "[{\"type\":1,\"components\":["
        "{\"type\":2,\"style\":3,\"label\":\"\\u2705 Approve\",\"custom_id\":\"conf:yes\"},"
        "{\"type\":2,\"style\":4,\"label\":\"\\u274c Cancel\",\"custom_id\":\"conf:no\"}]}]";
    Buf t;
    buf_init(&t);
    buf_printf(&t, "\xe2\x9a\xa0\xef\xb8\x8f %.850s", desc);
    char *prompt_id = dc_send_full(r, ctx->channel, t.s, KB);
    buf_free(&t);

    int window = 120;
    const char *cs = getenv("CLAWC_CONFIRM_SECS");
    if (cs && atoi(cs) > 0) window = atoi(cs);
    time_t deadline = time(NULL) + window;
    int answer = -1;
    while (answer < 0 && time(NULL) < deadline) {
        int i;
        for (i = 0; i < r->nconf; i++) {
            DcInter *x = &r->conf[i];
            int yes = str_eq(x->custom_id, "conf:yes");
            int stale = (!prompt_id || !prompt_id[0] || !x->msg_id[0] ||
                         !str_eq(x->msg_id, prompt_id) || !str_eq(x->sender, ctx->sender));
            if (stale) {
                dc_inter_ack(r, x->inter_id, x->inter_token, 4,
                             "(expired)", 1);
            } else {
                answer = yes ? 1 : 0;
                dc_inter_ack(r, x->inter_id, x->inter_token, 7,
                             answer ? "(approved \xe2\x9c\x85)"
                                    : "(cancelled \xe2\x9d\x8c)", 0);
            }
            memmove(&r->conf[i], &r->conf[i + 1],
                    (size_t)(r->nconf - i - 1) * sizeof r->conf[0]);
            r->nconf--;
            if (answer >= 0) break;
            i--;
        }
        for (i = 0; i < r->npend && answer < 0; i++) {
            if (!str_eq(r->pend[i].channel, ctx->channel)) continue;
            const char *b = r->pend[i].body;
            int yes = str_starts(b, "/yes"), no = str_starts(b, "/no");
            if (!yes && !no) continue;
            if (str_eq(r->pend[i].sender, ctx->sender) &&
                dc_after(r->pend[i].msg_id, prompt_id)) answer = yes ? 1 : 0;
            free(r->pend[i].body);
            free(r->pend[i].img_path);
            memmove(&r->pend[i], &r->pend[i + 1],
                    (size_t)(r->npend - i - 1) * sizeof r->pend[0]);
            r->npend--;
            i--;
        }
        if (answer < 0) {
            Buf msg;
            buf_init(&msg);
            if (ws_recv_msg(&r->ws, &msg, 10) == 0) {
                JVal *root = json_parse(msg.s);
                if (root) {
                    long long s2 = (long long)json_get_num(root, "s", 0);
                    int op = (int)json_get_num(root, "op", -1);
                    JVal *d2 = json_obj_get(root, "d");
                    const char *t2 = json_get_str(root, "t");
                    if (op == 0) dc_dispatch(r, d2, t2, s2);
                    else if (op == 1) {
                        char dta[64];
                        snprintf(dta, sizeof dta, "%lld", r->seq > 0 ? r->seq : 0);
                        dc_gateway_send(r, 1, r->seq > 0 ? dta : "null");
                    }
                    json_free(root);
                }
            }
            buf_free(&msg);
        }
    }
    if (answer < 0) {
        answer = 0;
        if (prompt_id) {
            Buf body;
            buf_init(&body);
            buf_printf(&body, "{\"content\":\"(timed out - cancelled)\","
                              "\"components\":[]}");
            char path[200];
            snprintf(path, sizeof path, "/channels/%s/messages/%s",
                     ctx->channel, prompt_id);
            /* PATCH semantics via our POST-only helper is not available;
               send the notice as a follow-up message instead */
            free(dc_rest(r, path, body.s));
            buf_free(&body);
        }
        dc_send(r, ctx->channel, "(cancelled)");
    }
    free(prompt_id);
    return answer;
}

/* per-channel sessions */
#define DC_MAX_SESS 16
typedef struct {
    char channel[128];
    Agent agent;
} DcSession;

static Agent *dc_agent_for(DcRun *r, const char *channel) {
    static DcSession sessions[DC_MAX_SESS];
    static int nsess;
    int i;
    for (i = 0; i < nsess; i++)
        if (str_eq(sessions[i].channel, channel)) return &sessions[i].agent;
    if (nsess >= DC_MAX_SESS) {
        agent_free(&sessions[0].agent);
        memmove(&sessions[0], &sessions[1], (size_t)(nsess - 1) * sizeof sessions[0]);
        nsess--;
    }
    DcSession *s = &sessions[nsess++];
    snprintf(s->channel, sizeof s->channel, "%s", channel);
    agent_init(&s->agent);
    s->agent.llm = r->tmpl->llm;
    s->agent.tools = r->tmpl->tools;
    s->agent.max_steps = r->tmpl->max_steps;
    s->agent.auto_approve = r->tmpl->auto_approve;
    s->agent.auto_learn = r->tmpl->auto_learn;
    unsigned long h = 5381;
    const char *p = channel;
    while (*p) h = h * 33 + (unsigned char)*p++;
    char path[640];
    snprintf(path, sizeof path, "%s/history/dc-%08lx.jsonl",
             r->tmpl->tools.workspace, h);
    agent_open_session(&s->agent, path);
    return &s->agent;
}

int discord_run(Agent *tmpl, const DcCfg *dc) {
    if (!dc->token || !dc->token[0]) {
        fprintf(stderr, "[discord] needs discord_token\n");
        return 2;
    }
    if (!sender_ready("discord", dc->users)) return 2;
    DcRun r;
    memset(&r, 0, sizeof r);
    r.tmpl = tmpl;
    r.cfg = *dc;
    dc_cursor_load(&r);

    const char *gw = (dc->gateway && dc->gateway[0]) ? dc->gateway
                     : "wss://gateway.discord.gg/?v=10&encoding=json";
    for (;;) {
        char *err = NULL;
        if (ws_connect(&r.ws, gw, tmpl->tools.insecure_tls, &err) != 0) {
            fprintf(stderr, "[discord] gateway connect failed: %s\n",
                    err ? err : "?");
            free(err);
            sleep(5);
            continue;
        }
        fprintf(stderr, "[discord] gateway connected (seq %lld)\n", r.seq);
        r.running = 1;
        int identified = 0;
        while (r.running) {
            if (identified && r.hb_ms > 0 && time(NULL) >= r.next_hb) {
                char d[64];
                snprintf(d, sizeof d, "%lld", r.seq > 0 ? r.seq : 0);
                dc_gateway_send(&r, 1, r.seq > 0 ? d : "null");
                r.next_hb = time(NULL) + (r.hb_ms / 1000 ? r.hb_ms / 1000 : 10);
            }
            Buf msg;
            buf_init(&msg);
            int rc = ws_recv_msg(&r.ws, &msg, 10);
            if (rc == 1) {
                buf_free(&msg);
                break;
            }
            if (rc != 0) {
                buf_free(&msg);
                continue;
            }
            JVal *root = json_parse(msg.s);
            buf_free(&msg);
            if (!root) continue;
            long long s = (long long)json_get_num(root, "s", 0);
            int op = (int)json_get_num(root, "op", -1);
            JVal *d = json_obj_get(root, "d");
            const char *t = json_get_str(root, "t");
            if (op == 10) {
                r.hb_ms = (long)json_get_num(d, "heartbeat_interval", 10000);
                r.next_hb = time(NULL) + (r.hb_ms / 2000 ? r.hb_ms / 2000 : 5);
                if (r.session_id[0]) {
                    Buf dr;
                    buf_init(&dr);
                    buf_printf(&dr, "{\"token\":");
                    json_quote(&dr, dc->token);
                    buf_printf(&dr, ",\"session_id\":");
                    json_quote(&dr, r.session_id);
                    buf_printf(&dr, ",\"seq\":%lld}", r.seq);
                    dc_gateway_send(&r, 6, dr.s);
                    buf_free(&dr);
                } else {
                    Buf dr;
                    buf_init(&dr);
                    buf_printf(&dr, "{\"token\":");
                    json_quote(&dr, dc->token);
                    buf_puts(&dr, ",\"intents\":33281,\"properties\":{}}");
                    dc_gateway_send(&r, 2, dr.s);
                    buf_free(&dr);
                }
                identified = 1;
            } else if (op == 0) {
                if (s > 0 && s <= r.seq && t && str_eq(t, "MESSAGE_CREATE")) {
                    json_free(root);
                    continue;
                }
                dc_dispatch(&r, d, t, s);
                dc_cursor_save(&r);
            } else if (op == 1) {
                char dta[64];
                snprintf(dta, sizeof dta, "%lld", r.seq > 0 ? r.seq : 0);
                dc_gateway_send(&r, 1, r.seq > 0 ? dta : "null");
            } else if (op == 7) {
                r.running = 0;
            } else if (op == 9) {
                r.session_id[0] = 0;
                r.seq = 0;
                r.running = 0;
            }
            json_free(root);

            /* register slash commands once READY gave us the application id */
            if (r.app_id[0] && !r.cmds_registered) {
                /* v2: registration is optional; only when explicitly enabled */
                const char *reg = getenv("CLAWC_DISCORD_REGISTER");
                if (reg && reg[0] && str_eq(reg, "1")) {
                    char path[160];
                    snprintf(path, sizeof path, "/applications/%s/commands", r.app_id);
                    static const char *CMDS =
                        "[{\"name\":\"claw\",\"description\":\"Ask clawc to do a task\","
                        "\"options\":[{\"name\":\"task\",\"type\":3,\"required\":true}]}]";
                    free(dc_rest(&r, path, CMDS));
                }
                r.cmds_registered = 1;
            }

            while (r.npend > 0) {
                char channel[128], sender[72];
                snprintf(channel, sizeof channel, "%s", r.pend[0].channel);
                snprintf(sender, sizeof sender, "%s", r.pend[0].sender);
                char *text = r.pend[0].body;
                char *img = r.pend[0].img_path;
                r.pend[0].body = NULL;
                r.pend[0].img_path = NULL;
                free(r.pend[0].body);
                free(r.pend[0].img_path);
                memmove(&r.pend[0], &r.pend[1],
                        (size_t)(r.npend - 1) * sizeof r.pend[0]);
                r.npend--;
                fprintf(stderr, "[discord] %s: %.80s\n", channel, text);
                if (str_starts(text, "/reset")) {
                    agent_clear_session(dc_agent_for(&r, channel));
                    dc_send(&r, channel, "(history cleared)");
                } else if (str_starts(text, "/help") || str_starts(text, "/start")) {
                    dc_send(&r, channel, "clawc online. send a task (or /claw "
                                         "<task>); /reset clears history. "
                                         "risky actions: buttons or /yes /no.");
                } else {
                    Agent *a = dc_agent_for(&r, channel);
                    DcCtx ctx;
                    ctx.run = &r;
                    ctx.channel = channel;
                    ctx.sender = sender;
                    a->confirm = tmpl->auto_approve ? NULL : dc_confirm;
                    a->cb_user = &ctx;
                    Buf reply;
                    buf_init(&reply);
                    agent_prompt_img(a, text, img, &reply);
                    dc_send(&r, channel,
                            reply.s && reply.s[0] ? reply.s : "(no output)");
                    buf_free(&reply);
                }
                free(text);
                free(img);
                if (dc->once) {
                    ws_close(&r.ws);
                    return 0;
                }
            }
        }
        ws_close(&r.ws);
        sleep(2);
    }
}
