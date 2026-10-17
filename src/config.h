/* clawc - config.h: JSON configuration file + environment resolution.
   Precedence (high to low): CLI flags > environment > config file > defaults. */
#ifndef CLAWC_CONFIG_H
#define CLAWC_CONFIG_H

#include <stdio.h>

/* setting keys (index into Cfg.src[]) */
enum {
    CK_WORKSPACE = 0,
    CK_API_URL,
    CK_API_KEY,
    CK_API_KIND,
    CK_MODEL,
    CK_MAX_STEPS,
    CK_TIMEOUT,
    CK_SANDBOX,
    CK_INSECURE,
    CK_AUTO_APPROVE,
    CK_NOTIFY,
    CK_TG_TOKEN,
    CK_TG_API,
    CK_TG_ALLOW, CK_TG_USERS,
    CK_TG_POLL,
    CK_SERVE_PORT,
    CK_SERVE_BIND,
    CK_CONTEXT_CHARS,
    CK_HB_INTERVAL,
    CK_AUTONOMOUS,
    CK_AUTO_LEARN,
    CK_MX_HS, CK_MX_TOKEN, CK_MX_USER, CK_MX_ROOMS, CK_MX_USERS,
    CK_A2A_PEERS, CK_BRIDGE_TOKEN, CK_SERVE_TLS, CK_SERVE_CERT, CK_SERVE_KEY, CK_A2A_TLS, CK_A2A_CA,
    CK_DC_TOKEN, CK_DC_GUILDS, CK_DC_CHANNELS, CK_DC_USERS,
    CK_COUNT
};

typedef struct {
    /* strings: owned, NULL = unset */
    char *workspace, *api_url, *api_key, *api_kind, *model, *sandbox, *notify;
    char *tg_token, *tg_api, *tg_allow, *tg_users, *serve_bind, *serve_cert, *serve_key, *a2a_ca;
    char *mx_hs, *mx_token, *mx_user, *mx_rooms, *mx_users, *a2a_peers, *bridge_token;
    char *dc_token, *dc_guilds, *dc_channels, *dc_users;
    /* ints: max_steps/timeout/tg_poll carry built-in defaults from cfg_init;
       serve_port/hb_interval use -1 = unset (disabled) */
    int max_steps, timeout, insecure, auto_approve, tg_poll, context_chars, auto_learn;
    int serve_port, hb_interval, autonomous_interval, serve_tls, a2a_tls;
    const char *src[CK_COUNT]; /* "default" | "file" | "env" | "flag" */
    char *path;                /* config file actually loaded, or NULL */
} Cfg;

void cfg_init(Cfg *c);   /* defaults; strings NULL, serve_port/hb_interval -1 */

/* Load a JSON config file ({ "key": value, ... }; "_"-prefixed keys are notes).
   Returns 0 loaded, 1 file not found, -1 parse error (message on stderr).
   Explicitly-set empty values are ignored (same as unset). */
int cfg_load_file(Cfg *c, const char *path);

/* Apply CLAWC_* / OPENAI_API_KEY / ANTHROPIC_API_KEY over the file values. */
void cfg_load_env(Cfg *c);

/* Setters used by the CLI flag layer (src is e.g. "flag"). */
void cfg_set_str(Cfg *c, int key, char *val, const char *src); /* takes ownership */
void cfg_free(Cfg *c);   /* free all owned strings */
void cfg_set_int(Cfg *c, int key, long v, const char *src);
void cfg_set_bool(Cfg *c, int key, int v, const char *src);

/* `clawc config`: effective values + sources + daemon gateway plan. */
void cfg_dump(const Cfg *c, FILE *f);

#endif
