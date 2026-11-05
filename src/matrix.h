/* clawc - matrix.h: Matrix channel (client-server API over HTTPS long-poll). */
#ifndef CLAWC_MATRIX_H
#define CLAWC_MATRIX_H

#include "agent.h"

typedef struct {
    char *homeserver;  /* e.g. https://matrix.example.org (no trailing slash) */
    char *token;       /* access token (Element: Settings -> Help -> Access token) */
    char *user;        /* our @user:server (own messages are ignored) */
    char *rooms;       /* comma list of !room:server to answer in; empty = all */
    const char *users; /* authorized Matrix sender IDs */
    int once;          /* handle one message and exit (tests/scripts) */
} MxCfg;

/* Poll forever: sync -> answer messages with the agent. Returns on --once. */
int matrix_run(Agent *tmpl, const MxCfg *mx);

#endif
