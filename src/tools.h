/* clawc - tools.h: agent tools (shell, files, memory, skills). */
#ifndef CLAWC_TOOLS_H
#define CLAWC_TOOLS_H

#include "util.h"
#include "json.h"

typedef struct {
    char *workspace;   /* root for memory/skills; default files base */
    char *sandbox;     /* if non-NULL, file tools confined to this dir */
    int os_sandbox;    /* 1 = OS sandbox ACTIVE (applied at startup; the
                          kernel confines the whole process, so safe_path
                          skips dir checks).  main.c refuses to start with
                          this set unless a real backend took effect. */
    int timeout_s;     /* shell tool timeout */
    int insecure_tls;  /* skip TLS verification in fetch_url */
    char *a2a_peers;   /* "name=host:port,..." peer clawc instances */
    char *bridge_token; /* shared bridge bearer credential */
    char *a2a_ca;
    int a2a_tls;
} ToolsCtx;

/* Number of tools and their definitions (OpenAI-style function specs). */
int tools_count(void);
const char *tools_name(int i);
const char *tools_desc(int i);
const char *tools_schema(int i);      /* JSON schema string */
const char *tools_openai_json(void);  /* full "tools": [...] array for OpenAI API */
const char *tools_anthropic_json(void); /* full "tools": [...] array for Anthropic API */

/* Execute one tool by name with a JSON object of arguments.
   Result is appended to out. Returns 0 if the tool existed. */
int tools_exec(const ToolsCtx *ctx, const char *name, const JVal *args, Buf *out);

/* 1 if this tool call is potentially destructive and should be confirmed. */
int tools_is_risky(const char *name, const JVal *args);

/* Plain-string helpers used by agent bootstrap */
char *tools_read_memory(const ToolsCtx *ctx);   /* MEMORY.md contents or "" */
char *tools_read_soul(const ToolsCtx *ctx);     /* SOUL.md contents or "" */
char *tools_skills_listing(const ToolsCtx *ctx);/* "name - desc" lines */

/* GOALS.md operations (shared by the goal tools and the /goal command).
   Result text is appended to out. Returns 0 on success. */
int goal_add_text(const ToolsCtx *ctx, const char *task, const char *due,
                  const char *prio, Buf *out);
int goal_done_text(const ToolsCtx *ctx, const char *id, Buf *out);
int goal_rm_text(const ToolsCtx *ctx, const char *id, Buf *out);

#endif
