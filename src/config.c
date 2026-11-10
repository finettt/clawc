/* clawc - config.c: configuration file parser and env/flag merge. */
#define _XOPEN_SOURCE 700
#include "config.h"
#ifndef CLAWC_NO_CRON
#include "cron.h"
#endif
#include "json.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Compact relocation-free key names and parallel identifiers. */
#define CONFIG_KEYS(X) \
    X("workspace", CK_WORKSPACE) \
    X("api_url", CK_API_URL) \
    X("api_key", CK_API_KEY) \
    X("api", CK_API_KIND) \
    X("model", CK_MODEL) \
    X("max_steps", CK_MAX_STEPS) \
    X("timeout", CK_TIMEOUT) \
    X("sandbox", CK_SANDBOX) \
    X("insecure", CK_INSECURE) \
    X("auto_approve", CK_AUTO_APPROVE) \
    X("notify", CK_NOTIFY) \
    X("telegram_token", CK_TG_TOKEN) \
    X("telegram_api", CK_TG_API) \
    X("telegram_allow", CK_TG_ALLOW) \
    X("telegram_users", CK_TG_USERS) \
    X("telegram_poll", CK_TG_POLL) \
    X("serve_port", CK_SERVE_PORT) \
    X("serve_bind", CK_SERVE_BIND) \
    X("context_chars", CK_CONTEXT_CHARS) \
    X("heartbeat_interval", CK_HB_INTERVAL) \
    X("autonomous_interval", CK_AUTONOMOUS) \
    X("auto_learn", CK_AUTO_LEARN) \
    X("matrix_homeserver", CK_MX_HS) \
    X("matrix_token", CK_MX_TOKEN) \
    X("matrix_user", CK_MX_USER) \
    X("matrix_rooms", CK_MX_ROOMS) \
    X("matrix_users", CK_MX_USERS) \
    X("a2a_peers", CK_A2A_PEERS) \
    X("bridge_token", CK_BRIDGE_TOKEN) \
    X("serve_tls", CK_SERVE_TLS) \
    X("serve_cert", CK_SERVE_CERT) \
    X("serve_key", CK_SERVE_KEY) \
    X("a2a_tls", CK_A2A_TLS) \
    X("a2a_ca", CK_A2A_CA) \
    X("discord_token", CK_DC_TOKEN) \
    X("discord_guilds", CK_DC_GUILDS) \
    X("discord_channels", CK_DC_CHANNELS) \
    X("discord_users", CK_DC_USERS)
#define KEY_NAME(name, id) name "\0"
static const char KEY_NAMES[] = CONFIG_KEYS(KEY_NAME);
#undef KEY_NAME
#define KEY_ID(name, id) id,
static const unsigned char KEY_IDS[] = { CONFIG_KEYS(KEY_ID) };
#undef KEY_ID
#undef CONFIG_KEYS

static const char *D = "default";

void cfg_init(Cfg *c) {
    int i;
    memset(c, 0, sizeof *c);
    c->max_steps = 8;
    c->timeout = 60;
    c->tg_poll = 50;
    c->context_chars = 16384;
    c->auto_learn = 1;
#ifndef CLAWC_NO_SERVE
    c->serve_tls = c->a2a_tls = 1;
#endif
    c->serve_port = -1;
    c->serve_workers = 8;
    c->hb_interval = -1;
    c->autonomous_interval = -1;
    for (i = 0; i < CK_COUNT; i++) c->src[i] = D;
}

void cfg_set_str(Cfg *c, int key, char *val, const char *src) {
    char **slot = NULL;
    switch (key) {
    case CK_WORKSPACE:  slot = &c->workspace; break;
    case CK_API_URL:    slot = &c->api_url; break;
    case CK_API_KEY:    slot = &c->api_key; break;
    case CK_API_KIND:   slot = &c->api_kind; break;
    case CK_MODEL:      slot = &c->model; break;
    case CK_SANDBOX:    slot = &c->sandbox; break;
    case CK_NOTIFY:     slot = &c->notify; break;
    case CK_TG_TOKEN:   slot = &c->tg_token; break;
    case CK_TG_API:     slot = &c->tg_api; break;
    case CK_TG_ALLOW:   slot = &c->tg_allow; break;
    case CK_TG_USERS:   slot = &c->tg_users; break;
    case CK_SERVE_BIND: slot = &c->serve_bind; break;
#ifndef CLAWC_NO_MATRIX
    case CK_MX_HS:      slot = &c->mx_hs; break;
#endif
#ifndef CLAWC_NO_MATRIX
    case CK_MX_TOKEN:   slot = &c->mx_token; break;
#endif
#ifndef CLAWC_NO_MATRIX
    case CK_MX_USER:    slot = &c->mx_user; break;
#endif
#ifndef CLAWC_NO_MATRIX
    case CK_MX_ROOMS:   slot = &c->mx_rooms; break;
#endif
#ifndef CLAWC_NO_MATRIX
    case CK_MX_USERS:   slot = &c->mx_users; break;
#endif
#ifndef CLAWC_NO_SERVE
    case CK_A2A_PEERS:  slot = &c->a2a_peers; break;
#endif
#ifndef CLAWC_NO_SERVE
    case CK_BRIDGE_TOKEN: slot = &c->bridge_token; break;
#endif
#ifndef CLAWC_NO_SERVE
    case CK_SERVE_CERT: slot = &c->serve_cert; break;
    case CK_SERVE_KEY: slot = &c->serve_key; break;
    case CK_A2A_CA: slot = &c->a2a_ca; break;
#endif
#ifndef CLAWC_NO_DISCORD
    case CK_DC_TOKEN:   slot = &c->dc_token; break;
#endif
#ifndef CLAWC_NO_DISCORD
    case CK_DC_GUILDS:  slot = &c->dc_guilds; break;
#endif
#ifndef CLAWC_NO_DISCORD
    case CK_DC_CHANNELS: slot = &c->dc_channels; break;
#endif
#ifndef CLAWC_NO_DISCORD
    case CK_DC_USERS: slot = &c->dc_users; break;
#endif
    default: return;
    }
    free(*slot);   /* drop the overridden value */
    *slot = val;
    c->src[key] = src;
}

void cfg_free(Cfg *c) {
    char **s[] = { &c->workspace, &c->api_url, &c->api_key, &c->api_kind,
                   &c->model, &c->sandbox, &c->notify, &c->tg_token, &c->tg_api,
                   &c->tg_allow, &c->tg_users, &c->serve_bind,
#ifndef CLAWC_NO_SERVE
                   &c->serve_cert, &c->serve_key, &c->a2a_ca, &c->a2a_peers, &c->bridge_token,
#endif
                   &c->mx_hs, &c->mx_token, &c->mx_user, &c->mx_rooms, &c->mx_users, &c->dc_token,
                   &c->dc_guilds, &c->dc_channels, &c->dc_users, &c->path };
    size_t i;
    for (i = 0; i < sizeof s / sizeof s[0]; i++) {
        free(*s[i]);
        *s[i] = NULL;
    }
}

void cfg_set_int(Cfg *c, int key, long v, const char *src) {
    switch (key) {
    case CK_MAX_STEPS:   c->max_steps = (int)v; break;
    case CK_TIMEOUT:     c->timeout = (int)v; break;
    case CK_TG_POLL:     c->tg_poll = (int)v; break;
    case CK_SERVE_PORT:  c->serve_port = (int)v; break;
    case CK_CONTEXT_CHARS: c->context_chars = (int)v; break;
    case CK_HB_INTERVAL: c->hb_interval = (int)v; break;
    case CK_AUTONOMOUS: c->autonomous_interval = (int)v; break;
    default: return;
    }
    c->src[key] = src;
}

void cfg_set_bool(Cfg *c, int key, int v, const char *src) {
    switch (key) {
    case CK_INSECURE:     c->insecure = v; break;
    case CK_AUTO_APPROVE: c->auto_approve = v; break;
    case CK_AUTO_LEARN:   c->auto_learn = v; break;
#ifndef CLAWC_NO_SERVE
    case CK_SERVE_TLS: c->serve_tls = v; break;
    case CK_A2A_TLS: c->a2a_tls = v; break;
#endif
    default: return;
    }
    c->src[key] = src;
}

static int parse_long(const char *v, long *out) {
    char *end;
    errno = 0;
    long x = strtol(v, &end, 10);
    if (end == v || *end != 0 || errno != 0) return -1;
    *out = x;
    return 0;
}

static int parse_bool(const char *v, int *out) {
    char b[16];
    size_t i, n = strlen(v);
    if (n >= sizeof b) return -1;
    for (i = 0; i < n; i++) b[i] = (char)tolower((unsigned char)v[i]);
    b[n] = 0;
    if (!strcmp(b, "1") || !strcmp(b, "yes") || !strcmp(b, "true") || !strcmp(b, "on")) {
        *out = 1;
        return 0;
    }
    if (!strcmp(b, "0") || !strcmp(b, "no") || !strcmp(b, "false") || !strcmp(b, "off")) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int key_id(const char *name) {
    const char *p = KEY_NAMES;
    size_t i;
    for (i = 0; i < sizeof KEY_IDS; i++) {
        if (str_eq(p, name)) return KEY_IDS[i];
        p += strlen(p) + 1;
    }
    return -1;
}

/* Apply one raw value; returns 0 ok, -1 bad value. */
static int cfg_apply(Cfg *c, int key, const char *val, const char *src) {
    long n;
    int b;
    switch (key) {
#ifdef CLAWC_NO_SERVE
    case CK_SERVE_TLS: case CK_SERVE_CERT: case CK_SERVE_KEY:
    case CK_A2A_TLS: case CK_A2A_CA: case CK_BRIDGE_TOKEN: case CK_A2A_PEERS: return 0;
#endif
    case CK_MAX_STEPS:
    case CK_TIMEOUT:
    case CK_TG_POLL:
    case CK_SERVE_PORT:
    case CK_CONTEXT_CHARS:
    case CK_HB_INTERVAL:
    case CK_AUTONOMOUS:
        if (parse_long(val, &n) != 0) return -1;
        cfg_set_int(c, key, n, src);
        return 0;
    case CK_INSECURE:
    case CK_AUTO_APPROVE:
    case CK_AUTO_LEARN:
#ifndef CLAWC_NO_SERVE
    case CK_SERVE_TLS:
    case CK_A2A_TLS:
#endif
        if (parse_bool(val, &b) != 0) return -1;
        cfg_set_bool(c, key, b, src);
        return 0;
    default:
        if (!val[0]) return 0; /* empty string = leave unset */
        cfg_set_str(c, key, xstrdup(val), src);
        return 0;
    }
}

int cfg_load_file(Cfg *c, const char *path) {
    char *data = read_file(path);
    if (!data) return 1;
    if (c->path) free(c->path);
    c->path = xstrdup(path);
    int rc = 0;

    /* JSON config: { "key": value, ... } (keys starting with "_" are notes) */
    char *probe = data;
    while (*probe == ' ' || *probe == '\t' || *probe == '\r' || *probe == '\n') probe++;
    if (*probe == '{') {
        JVal *root = json_parse(data);
        if (!root || root->type != J_OBJ) {
            fprintf(stderr, "clawc: %s: bad JSON\n", path);
            json_free(root);
            free(data);
            return -1;
        }
        for (int i = 0; i < root->nkv && rc == 0; i++) {
            const char *k = root->keys[i];
            JVal *v = root->vals[i];
            if (k[0] == '_') continue;                 /* commentary key */
            int id = key_id(k);
            if (id < 0) {
                fprintf(stderr, "clawc: %s: unknown key '%s'\n", path, k);
                rc = -1;
                break;
            }
            if ((id == CK_TG_USERS || id == CK_MX_USERS || id == CK_DC_USERS) &&
                v->type != J_STR && v->type != J_NULL) {
                fprintf(stderr, "%s must be a string\n", k);
                rc = -1;
                break;
            }
            char numbuf[64];
            const char *vs;
            if (v->type == J_STR) vs = v->str;
            else if (v->type == J_NUM) {
                if (v->num == (double)(long long)v->num)
                    snprintf(numbuf, sizeof numbuf, "%lld", (long long)v->num);
                else
                    snprintf(numbuf, sizeof numbuf, "%g", v->num);
                vs = numbuf;
            } else if (v->type == J_BOOL) vs = v->boolean ? "on" : "off";
            else if (v->type == J_NULL) continue;
            else {
                fprintf(stderr, "clawc: %s: bad value for '%s'\n", path, k);
                rc = -1;
                break;
            }
            if (cfg_apply(c, id, vs, "file") != 0) {
                fprintf(stderr, "clawc: %s: bad value for '%s'\n", path, k);
                rc = -1;
            }
        }
        json_free(root);
        free(data);
        return rc;
    }

    /* JSON only — the legacy key = value format is gone */
    fprintf(stderr, "clawc: %s: not a JSON config object\n", path);
    free(data);
    return -1;
}

static void env_str(Cfg *c, int key, const char *name) {
    const char *v = getenv(name);
    if (v && v[0]) cfg_set_str(c, key, xstrdup(v), "env");
}

static void env_int(Cfg *c, int key, const char *name) {
    const char *v = getenv(name);
    long n;
    if (v && v[0] && parse_long(v, &n) == 0) cfg_set_int(c, key, n, "env");
}

static void env_bool(Cfg *c, int key, const char *name) {
    const char *v = getenv(name);
    int b;
    if (v && v[0] && parse_bool(v, &b) == 0) cfg_set_bool(c, key, b, "env");
}

void cfg_load_env(Cfg *c) {
    env_str(c, CK_WORKSPACE, "CLAWC_WORKSPACE");
    env_str(c, CK_API_URL, "CLAWC_API_URL");
    env_str(c, CK_API_KEY, "CLAWC_API_KEY");
    if (!c->api_key) env_str(c, CK_API_KEY, "OPENAI_API_KEY");
    if (!c->api_key) env_str(c, CK_API_KEY, "ANTHROPIC_API_KEY");
    env_str(c, CK_API_KIND, "CLAWC_API_KIND");
    env_str(c, CK_MODEL, "CLAWC_MODEL");
    env_str(c, CK_SANDBOX, "CLAWC_SANDBOX");
    env_str(c, CK_NOTIFY, "CLAWC_NOTIFY");
    env_int(c, CK_MAX_STEPS, "CLAWC_MAX_STEPS");
    env_int(c, CK_TIMEOUT, "CLAWC_TIMEOUT");
    env_bool(c, CK_INSECURE, "CLAWC_INSECURE");
    env_bool(c, CK_AUTO_APPROVE, "CLAWC_AUTO_APPROVE");
    env_bool(c, CK_AUTO_LEARN, "CLAWC_AUTO_LEARN");
    env_str(c, CK_TG_TOKEN, "CLAWC_TELEGRAM_TOKEN");
    env_str(c, CK_TG_API, "CLAWC_TELEGRAM_API");
    env_str(c, CK_TG_ALLOW, "CLAWC_TELEGRAM_ALLOW");
    env_str(c, CK_TG_USERS, "CLAWC_TELEGRAM_USERS");
    env_int(c, CK_TG_POLL, "CLAWC_TELEGRAM_POLL");
    env_int(c, CK_SERVE_PORT, "CLAWC_SERVE_PORT");
    env_str(c, CK_SERVE_BIND, "CLAWC_SERVE_BIND");
#ifndef CLAWC_NO_MATRIX
    env_str(c, CK_MX_HS, "CLAWC_MATRIX_HOMESERVER");
#endif
#ifndef CLAWC_NO_MATRIX
    env_str(c, CK_MX_TOKEN, "CLAWC_MATRIX_TOKEN");
#endif
#ifndef CLAWC_NO_MATRIX
    env_str(c, CK_MX_USER, "CLAWC_MATRIX_USER");
#endif
#ifndef CLAWC_NO_MATRIX
    env_str(c, CK_MX_ROOMS, "CLAWC_MATRIX_ROOMS");
#endif
#ifndef CLAWC_NO_MATRIX
    env_str(c, CK_MX_USERS, "CLAWC_MATRIX_USERS");
#endif
#ifndef CLAWC_NO_SERVE
    env_str(c, CK_A2A_PEERS, "CLAWC_A2A_PEERS");
    env_str(c, CK_BRIDGE_TOKEN, "CLAWC_BRIDGE_TOKEN");
    env_bool(c, CK_SERVE_TLS, "CLAWC_SERVE_TLS");
    env_str(c, CK_SERVE_CERT, "CLAWC_SERVE_CERT");
    env_str(c, CK_SERVE_KEY, "CLAWC_SERVE_KEY");
    env_bool(c, CK_A2A_TLS, "CLAWC_A2A_TLS");
    env_str(c, CK_A2A_CA, "CLAWC_A2A_CA");
#endif
#ifndef CLAWC_NO_DISCORD
    env_str(c, CK_DC_TOKEN, "CLAWC_DISCORD_TOKEN");
#endif
#ifndef CLAWC_NO_DISCORD
    env_str(c, CK_DC_GUILDS, "CLAWC_DISCORD_GUILDS");
#endif
#ifndef CLAWC_NO_DISCORD
    env_str(c, CK_DC_CHANNELS, "CLAWC_DISCORD_CHANNELS");
#endif
#ifndef CLAWC_NO_DISCORD
    env_str(c, CK_DC_USERS, "CLAWC_DISCORD_USERS");
#endif
    env_int(c, CK_CONTEXT_CHARS, "CLAWC_CONTEXT_CHARS");
    env_int(c, CK_HB_INTERVAL, "CLAWC_HEARTBEAT_INTERVAL");
    env_int(c, CK_AUTONOMOUS, "CLAWC_AUTONOMOUS_INTERVAL");
}

static void dump_str(const Cfg *c, FILE *f, int key, const char *label,
                     const char *unset_text) {
    const char *v = NULL;
    switch (key) {
    case CK_WORKSPACE: v = c->workspace; break;
    case CK_API_URL:   v = c->api_url; break;
    case CK_API_KIND:  v = c->api_kind; break;
    case CK_MODEL:     v = c->model; break;
    case CK_SANDBOX:   v = c->sandbox; break;
    case CK_NOTIFY:    v = c->notify; break;
    case CK_TG_TOKEN:  v = c->tg_token; break;
    case CK_TG_API:    v = c->tg_api; break;
    case CK_TG_ALLOW:  v = c->tg_allow; break;
    case CK_TG_USERS:  v = c->tg_users; break;
#ifndef CLAWC_NO_MATRIX
    case CK_MX_USERS:  v = c->mx_users; break;
#endif
#ifndef CLAWC_NO_DISCORD
    case CK_DC_USERS:  v = c->dc_users; break;
#endif
    case CK_SERVE_BIND: v = c->serve_bind; break;
#ifndef CLAWC_NO_SERVE
    case CK_A2A_PEERS: v = c->a2a_peers; break;
    case CK_SERVE_CERT: v = c->serve_cert; break;
    case CK_SERVE_KEY: v = c->serve_key; break;
    case CK_A2A_CA: v = c->a2a_ca; break;
#endif
    default: break;
    }
    fprintf(f, "%s: %s (%s)\n", label,
            (v && v[0]) ? v : unset_text, c->src[key]);
}

static void dump_int(const Cfg *c, FILE *f, int key, const char *label, int v) {
    if (v < 0) fprintf(f, "%s: (disabled) (%s)\n", label, c->src[key]);
    else fprintf(f, "%s: %d (%s)\n", label, v, c->src[key]);
}

void cfg_dump(const Cfg *c, FILE *f) {
    int tg_on = c->tg_token && c->tg_token[0];
    int sv_on = c->serve_port > 0;
    int hb_on = c->hb_interval > 0;
#ifndef CLAWC_NO_CRON
    int cr_on = c->workspace && c->workspace[0] && cron_active(c->workspace);
#else
    int cr_on = 0;
#endif
    int au_on = c->autonomous_interval > 0;
#ifndef CLAWC_NO_MATRIX
    int mx_on = c->mx_hs && c->mx_hs[0] && c->mx_token && c->mx_token[0];
#else
    int mx_on = 0;
#endif
#ifndef CLAWC_NO_DISCORD
    int dc_on = c->dc_token && c->dc_token[0];
#else
    int dc_on = 0;
#endif

    fprintf(f, "config file: %s\n", c->path ? c->path : "(none)");
    dump_str(c, f, CK_WORKSPACE, "workspace", "(unset)");
    dump_str(c, f, CK_API_URL, "api_url", "(unset)");
    fprintf(f, "api_key: %s (%s)\n", (c->api_key && c->api_key[0]) ? "(set)" : "(unset)",
            c->src[CK_API_KEY]);
    dump_str(c, f, CK_API_KIND, "api", "(auto from url)");
    dump_str(c, f, CK_MODEL, "model", "(unset)");
    fprintf(f, "max_steps: %d (%s)\n", c->max_steps, c->src[CK_MAX_STEPS]);
    fprintf(f, "timeout: %d (%s)\n", c->timeout, c->src[CK_TIMEOUT]);
    dump_str(c, f, CK_SANDBOX, "sandbox", "(off)");
    fprintf(f, "insecure: %s (%s)\n", c->insecure ? "on" : "off", c->src[CK_INSECURE]);
    fprintf(f, "auto_approve: %s (%s)\n", c->auto_approve ? "on" : "off",
            c->src[CK_AUTO_APPROVE]);
    fprintf(f, "auto_learn: %s (%s)\n", c->auto_learn ? "on" : "off",
            c->src[CK_AUTO_LEARN]);
    dump_str(c, f, CK_NOTIFY, "notify", "(unset)");
    fprintf(f, "telegram_token: %s (%s)\n", tg_on ? "(set)" : "(unset)", c->src[CK_TG_TOKEN]);
    dump_str(c, f, CK_TG_API, "telegram_api", "(unset)");
    dump_str(c, f, CK_TG_ALLOW, "telegram_allow", "(no chat filter)");
    dump_str(c, f, CK_TG_USERS, "telegram_users", "(required)");
#ifndef CLAWC_NO_MATRIX
    dump_str(c, f, CK_MX_USERS, "matrix_users", "(required)");
#endif
#ifndef CLAWC_NO_DISCORD
    dump_str(c, f, CK_DC_USERS, "discord_users", "(required)");
#endif
    fprintf(f, "telegram_poll: %d (%s)\n", c->tg_poll, c->src[CK_TG_POLL]);
#ifndef CLAWC_NO_SERVE
    dump_int(c, f, CK_SERVE_PORT, "serve_port", c->serve_port);
#endif
#ifndef CLAWC_NO_SERVE
    dump_str(c, f, CK_SERVE_BIND, "serve_bind", "(127.0.0.1)");
#endif
#ifndef CLAWC_NO_MATRIX
    dump_str(c, f, CK_MX_HS, "matrix_homeserver", "(off)");
#endif
#ifndef CLAWC_NO_MATRIX
    fprintf(f, "matrix_token: %s (%s)\n", (c->mx_token && c->mx_token[0]) ? "(set)" : "(unset)",
            c->src[CK_MX_TOKEN]);
#endif
#ifndef CLAWC_NO_MATRIX
    dump_str(c, f, CK_MX_USER, "matrix_user", "(unset)");
#endif
#ifndef CLAWC_NO_MATRIX
    dump_str(c, f, CK_MX_ROOMS, "matrix_rooms", "(all rooms)");
#endif
#ifndef CLAWC_NO_SERVE
    dump_str(c, f, CK_A2A_PEERS, "a2a_peers", "(none)");
#endif
#ifndef CLAWC_NO_SERVE
    fprintf(f, "bridge_token: %s (%s)\n", c->bridge_token && c->bridge_token[0] ? "(set)" : "(unset)", c->src[CK_BRIDGE_TOKEN]);
    fprintf(f, "serve_tls: %s (%s)\n", c->serve_tls ? "on" : "off", c->src[CK_SERVE_TLS]);
    dump_str(c, f, CK_SERVE_CERT, "serve_cert", "(unset)");
    dump_str(c, f, CK_SERVE_KEY, "serve_key", "(unset)");
    fprintf(f, "a2a_tls: %s (%s)\n", c->a2a_tls ? "on" : "off", c->src[CK_A2A_TLS]);
    dump_str(c, f, CK_A2A_CA, "a2a_ca", "(system trust)");
#endif
#ifndef CLAWC_NO_DISCORD
    fprintf(f, "discord_token: %s (%s)\n", (c->dc_token && c->dc_token[0]) ? "(set)" : "(unset)",
            c->src[CK_DC_TOKEN]);
#endif
#ifndef CLAWC_NO_DISCORD
    dump_str(c, f, CK_DC_GUILDS, "discord_guilds", "(all)");
#endif
#ifndef CLAWC_NO_DISCORD
    dump_str(c, f, CK_DC_CHANNELS, "discord_channels", "(all)");
#endif
    dump_int(c, f, CK_CONTEXT_CHARS, "context_chars", c->context_chars);
    dump_int(c, f, CK_HB_INTERVAL, "heartbeat_interval", c->hb_interval);
    dump_int(c, f, CK_AUTONOMOUS, "autonomous_interval", c->autonomous_interval);
    fprintf(f, "gateways:");
    if (tg_on) fprintf(f, " telegram");
    if (sv_on) fprintf(f, " serve");
    if (hb_on) fprintf(f, " heartbeat");
    if (cr_on) fprintf(f, " cron");
    if (au_on) fprintf(f, " autonomous");
    if (mx_on) fprintf(f, " matrix");
    if (dc_on) fprintf(f, " discord");
    if (!tg_on && !sv_on && !hb_on && !cr_on && !au_on && !mx_on && !dc_on) fprintf(f, " (none)");
    fprintf(f, "\n");
}
