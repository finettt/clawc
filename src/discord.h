/* clawc - discord.h: Discord channel. */
#ifndef CLAWC_DISCORD_H
#define CLAWC_DISCORD_H

#include "agent.h"

typedef struct {
    char *token;     /* bot token */
    char *guilds;    /* allowlist of guild ids (empty = all incl. DMs) */
    char *channels;  /* allowlist of channel ids (empty = all) */
    char *gateway;   /* ws:// override (tests); default wss://gateway.discord.gg/... */
    char *api;       /* REST base override (tests); default https://discord.com/api/v10 */
    const char *users; /* authorized Discord user IDs */
    int once;
} DcCfg;

int discord_run(Agent *tmpl, const DcCfg *dc);

#endif
