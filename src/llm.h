/* clawc - llm.h: model-agnostic chat client.
   Supports OpenAI-compatible APIs (OpenAI, Ollama, llama.cpp, DeepSeek, Groq...)
   and the Anthropic Messages API. Internally everything is normalized to Msg. */
#ifndef CLAWC_LLM_H
#define CLAWC_LLM_H

#include "util.h"

#define API_OPENAI    0
#define API_ANTHROPIC 1

typedef struct {
    char *id;      /* tool call id */
    char *name;
    char *args;    /* JSON object as text */
} ToolCall;

typedef struct {
    char *role;        /* system | user | assistant | tool */
    char *content;
    char *img_path;    /* optional image attached to this msg (vision input) */
    ToolCall *calls;   /* assistant tool calls */
    int ncalls;
    char *tool_call_id; /* for role=tool */
} Msg;

typedef struct {
    int api_kind;
    char *api_url;     /* base, e.g. https://api.openai.com/v1 */
    char *api_key;
    char *model;
    int max_tokens;
    int timeout_s;
    long context_chars; /* rough prompt budget in chars; 0 = unlimited */
    int insecure;      /* skip TLS verification (self-signed gateways) */
} LlmCfg;

void msg_free(Msg *m);
void msg_clear(Msg *m);

/* Streaming callback: called with each text delta as it arrives. */
typedef void (*LlmDeltaCb)(void *user, const char *text, size_t n);

/* Run one chat turn. On success returns 0 and fills *out (assistant msg:
   content and/or tool calls). On failure returns -1 and *err is malloc'd.
   on_delta: optional; enables SSE streaming (OpenAI dialect). Other dialects
   fall back to a single response (callback fires once with the full text). */
int llm_chat(const LlmCfg *cfg, const Msg *msgs, int nmsgs, Msg *out, char **err,
             LlmDeltaCb on_delta, void *delta_user);

/* Same, with control over the tool list (0 = plain chat, e.g. summarization). */
int llm_chat_ex(const LlmCfg *cfg, const Msg *msgs, int nmsgs, Msg *out, char **err,
                LlmDeltaCb on_delta, void *delta_user, int with_tools);

#endif
