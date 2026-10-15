/* clawc - telegram.h: built-in Telegram Bot API channel (long polling). */
#ifndef CLAWC_TELEGRAM_H
#define CLAWC_TELEGRAM_H

#include "agent.h"

typedef struct {
    char *token;        /* bot token from @BotFather */
    char *api_base;     /* default https://api.telegram.org */
    long *allow;        /* optional chat IDs; users list is always required */
    int nallow;
    const char *users; /* comma-separated authorized Telegram user IDs */
    int once;           /* process one message and exit (for tests/scripts) */
    int poll_timeout;   /* long-poll seconds (default 50) */
    int insecure;       /* skip TLS verify (self-signed local Bot API servers) */
    int auto_approve;   /* skip confirmations for risky tools (--yes) */
} TgCfg;

/* Run the bot loop: getUpdates -> agent -> sendMessage. Never returns unless
   once is set or a fatal error occurs. Returns exit code. */
int telegram_run(Agent *tmpl, const TgCfg *tg);

/* Fire-and-forget plain text message (used by daemon heartbeat -> Telegram). */
int tg_send_text(const TgCfg *tg, long long chat_id, const char *text);

#endif
