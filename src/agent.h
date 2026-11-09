/* clawc - agent.h: ReAct-style agent loop with persistent history. */
#ifndef CLAWC_AGENT_H
#define CLAWC_AGENT_H

#include "llm.h"
#include "tools.h"

typedef struct {
    LlmCfg llm;
    ToolsCtx tools;
    int max_steps;      /* max tool-calling iterations per prompt (default 8) */
    int heartbeat_mode; /* enable HEARTBEAT_OK convention */
    Msg *hist;
    int nhist, histcap;
    char *session_path; /* JSONL history file; NULL = ephemeral (run/heartbeat) */
    char *session_name; /* for audit log */

    /* UX callbacks (optional; set by channels) */
    void *cb_user;
    void (*on_status)(void *user, const char *phase);         /* "thinking", "tool:name" */
    void (*on_delta)(void *user, const char *text, size_t n); /* streamed reply text */
    int  (*confirm)(void *user, const char *risk_desc);       /* 1=approve 0=decline */
    int  auto_approve;   /* skip confirmations for risky tools (run/serve) */
    int  depth;          /* 0 = top level; subagents run at depth 1 (no nesting) */
    int  auto_learn;     /* after complex tasks, run a learning pass */
    int  learning_pass;  /* inside the learning pass (blocks recursion) */
    const char *tools_allow; /* comma list of tool names, NULL = all */
    int  persistence_error; /* a history operation failed */
    int  streamed_reply; /* out: 1 if the last reply arrived via on_delta */
} Agent;

void agent_init(Agent *a);
void agent_free(Agent *a);
void agent_reset(Agent *a);   /* clear conversation history (memory only) */

/* Open a persistent session: load history from path (if any), append future
   messages there. path's basename becomes the session name in the audit log. */
void agent_open_session(Agent *a, const char *path);

/* /reset: clear history AND truncate the session file. */
int agent_clear_session(Agent *a);

/* Turn-aware trim to budget (CLAWC_HISTORY_CHARS / CLAWC_HISTORY_MSGS envs).
   Runs automatically at the end of each prompt and after loading history. */
void agent_trim_budget(Agent *a);

/* Send a user message, run the tool loop, return assistant reply in *reply.
   Returns 0 on success. */
int agent_prompt(Agent *a, const char *user, Buf *reply);
int agent_prompt_img(Agent *a, const char *user, const char *img_path, Buf *reply);

/* Build the system prompt (identity+memory+skills+env) — exposed for debugging */
char *agent_system_prompt(Agent *a);

#endif
