/* clawc - agent.c */
#define _XOPEN_SOURCE 700
#include <pthread.h>
#include "agent.h"
#include "history.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BUDGET_CHARS_DEFAULT 48000
#define BUDGET_MSGS_DEFAULT  60

void agent_init(Agent *a) {
    memset(a, 0, sizeof *a);
    a->max_steps = 8;
    a->tools.timeout_s = 60;
}

void agent_free(Agent *a) {
    int i;
    for (i = 0; i < a->nhist; i++) msg_clear(&a->hist[i]);
    free(a->hist);
    free(a->session_path);
    free(a->session_name);
    memset(a, 0, sizeof *a);
}

void agent_reset(Agent *a) {
    int i;
    for (i = 0; i < a->nhist; i++) msg_clear(&a->hist[i]);
    free(a->hist);
    a->hist = NULL;
    a->nhist = a->histcap = 0;
}

static Msg *hist_push(Agent *a) {
    if (a->nhist + 1 > a->histcap) {
        a->histcap = a->histcap ? a->histcap * 2 : 16;
        a->hist = xrealloc(a->hist, (size_t)a->histcap * sizeof *a->hist);
    }
    Msg *m = &a->hist[a->nhist++];
    memset(m, 0, sizeof *m);
    return m;
}

static void hist_add(Agent *a, const char *role, const char *content) {
    Msg *m = hist_push(a);
    m->role = xstrdup(role);
    m->content = xstrdup(content ? content : "");
    if (a->session_path) hist_append(a->session_path, m);
}

/* ---- sanitization: drop tool messages without a matching pending tool_call,
   and strip tool_calls left unanswered (they break both API dialects). ---- */

static void strip_calls(Msg *m, const int *dead) {
    if (m->ncalls <= 0) return;
    ToolCall *fresh = NULL;
    int n = 0, i;
    for (i = 0; i < m->ncalls; i++)
        if (!dead[i]) n++;
    if (n == m->ncalls) return;
    if (n > 0) {
        fresh = xmalloc((size_t)n * sizeof *fresh);
        int k = 0;
        for (i = 0; i < m->ncalls; i++) {
            if (!dead[i]) fresh[k++] = m->calls[i];
            else {
                free(m->calls[i].id);
                free(m->calls[i].name);
                free(m->calls[i].args);
            }
        }
    } else {
        for (i = 0; i < m->ncalls; i++) {
            free(m->calls[i].id);
            free(m->calls[i].name);
            free(m->calls[i].args);
        }
    }
    free(m->calls);
    m->calls = fresh;
    m->ncalls = n;
}

static void sanitize(Agent *a) {
    int *pm = NULL, *pc = NULL; /* pending: msg index, call index */
    int np = 0, pcap = 0;

    int i, w = 0;
    for (i = 0; i < a->nhist; i++) {
        Msg *m = &a->hist[i];
        if (str_eq(m->role, "tool")) {
            int j, found = -1;
            for (j = 0; j < np; j++) {
                Msg *own = &a->hist[pm[j]];
                if (own->calls[pc[j]].id && str_eq(own->calls[pc[j]].id, m->tool_call_id)) {
                    found = j;
                    break;
                }
            }
            if (found < 0) {
                msg_clear(m);
                continue; /* orphan tool result */
            }
            memmove(&pm[found], &pm[found + 1], (size_t)(np - found - 1) * sizeof *pm);
            memmove(&pc[found], &pc[found + 1], (size_t)(np - found - 1) * sizeof *pc);
            np--;
        } else if (str_eq(m->role, "assistant") && m->ncalls > 0) {
            /* every call becomes pending */
            int j;
            for (j = 0; j < m->ncalls; j++) {
                if (np + 1 > pcap) {
                    pcap = pcap ? pcap * 2 : 8;
                    pm = xrealloc(pm, (size_t)pcap * sizeof *pm);
                    pc = xrealloc(pc, (size_t)pcap * sizeof *pc);
                }
                pm[np] = w;
                pc[np] = j;
                np++;
            }
        } else if (str_eq(m->role, "user")) {
            /* unanswered calls before a new user turn must be stripped */
            if (np > 0) {
                /* mark dead: group by msg */
                int j;
                for (j = 0; j < np; j++) {
                    Msg *own = &a->hist[pm[j]];
                    int *dead = xmalloc((size_t)own->ncalls * sizeof(int));
                    int k;
                    for (k = 0; k < own->ncalls; k++) dead[k] = 0;
                    /* mark all pending calls of this msg */
                    int j2;
                    for (j2 = 0; j2 < np; j2++)
                        if (pm[j2] == pm[j]) dead[pc[j2]] = 1;
                    strip_calls(own, dead);
                    free(dead);
                }
                np = 0;
            }
        }
        if (w != i) a->hist[w] = *m;
        w++;
    }
    /* trailing unanswered calls */
    if (np > 0) {
        int j;
        for (j = 0; j < np; j++) {
            Msg *own = &a->hist[pm[j]];
            int *dead = xmalloc((size_t)own->ncalls * sizeof(int));
            int k, j2;
            for (k = 0; k < own->ncalls; k++) dead[k] = 0;
            for (j2 = 0; j2 < np; j2++)
                if (pm[j2] == pm[j]) dead[pc[j2]] = 1;
            strip_calls(own, dead);
            free(dead);
        }
    }
    free(pm);
    free(pc);
    a->nhist = w;
}

/* ---- turn-aware budget trim ---- */

void agent_trim_budget(Agent *a) {
    const char *e1 = getenv("CLAWC_HISTORY_CHARS");
    const char *e2 = getenv("CLAWC_HISTORY_MSGS");
    long max_chars = e1 && e1[0] ? atol(e1) : BUDGET_CHARS_DEFAULT;
    int max_msgs = e2 && e2[0] ? atoi(e2) : BUDGET_MSGS_DEFAULT;
    if (max_chars < 4000) max_chars = 4000;
    if (max_msgs < 4) max_msgs = 4;

    if (a->nhist <= 1) return;

    for (;;) {
        /* measure */
        size_t total = 0;
        int i;
        for (i = 0; i < a->nhist; i++) {
            total += a->hist[i].content ? strlen(a->hist[i].content) : 0;
            int j;
            for (j = 0; j < a->hist[i].ncalls; j++)
                total += a->hist[i].calls[j].args ? strlen(a->hist[i].calls[j].args) : 0;
        }
        if (total <= (size_t)max_chars && a->nhist <= max_msgs) break;

        /* find end of first turn = just before the 2nd user message */
        int turn2 = -1;
        int seen_user = 0;
        for (i = 0; i < a->nhist; i++) {
            if (str_eq(a->hist[i].role, "user")) {
                seen_user++;
                if (seen_user == 2) { turn2 = i; break; }
            }
        }
        if (turn2 <= 0) break; /* only one turn left: keep it */
        for (i = 0; i < turn2; i++) msg_clear(&a->hist[i]);
        memmove(a->hist, a->hist + turn2, (size_t)(a->nhist - turn2) * sizeof *a->hist);
        a->nhist -= turn2;
    }

    /* compact the on-disk log to match memory */
    if (a->session_path && a->nhist >= 0) {
        hist_truncate(a->session_path);
        int i;
        for (i = 0; i < a->nhist; i++) hist_append(a->session_path, &a->hist[i]);
    }
}

void agent_open_session(Agent *a, const char *path) {
    free(a->session_path);
    free(a->session_name);
    a->session_path = xstrdup(path);
    const char *base = strrchr(path, '/');
    a->session_name = xstrdup(base ? base + 1 : path);
    /* strip .jsonl for readable audit names */
    char *dot = strstr(a->session_name, ".jsonl");
    if (dot) *dot = 0;

    int i;
    for (i = 0; i < a->nhist; i++) msg_clear(&a->hist[i]);
    free(a->hist);
    a->hist = NULL;
    a->nhist = a->histcap = 0;

    hist_load(path, &a->hist, &a->nhist);
    a->histcap = a->nhist;
    sanitize(a);
    agent_trim_budget(a);
    if (a->nhist > 0)
        fprintf(stderr, "[session] %s: %d messages loaded\n", a->session_name, a->nhist);
}

void agent_clear_session(Agent *a) {
    agent_reset(a);
    if (a->session_path) hist_truncate(a->session_path);
}

char *agent_system_prompt(Agent *a) {
    Buf b;
    buf_init(&b);
    buf_puts(&b,
        "You are clawc, a claw-style agent.\n\n"
        "# Rules\n"
        "- Prefer tools over guessing; verify when unsure.\n"
        "- Improve yourself: write_skill = procedure; reflect = lesson.\n"
        "- Never expose secrets found on the system unless asked.\n"
        "- Keep replies short and concrete.\n"
        "- When done, reply with the final answer and no further tool calls.\n"
        "- Work autonomously: goal_add/goal_done manage GOALS.md.\n");
    if (a->heartbeat_mode)
        buf_puts(&b,
            "- HEARTBEAT: waking on a timer; check the checklist below. "
            "Nothing to do? Reply exactly: HEARTBEAT_OK.\n");

    char *soul = tools_read_soul(&a->tools);
    if (soul && soul[0]) buf_printf(&b, "\n# Identity (SOUL.md)\n%s\n", soul);
    free(soul);

    char *mem = tools_read_memory(&a->tools);
    if (mem && mem[0]) buf_printf(&b, "\n# Memory (MEMORY.md)\n%s\n", mem);
    free(mem);

    {
        char *gp2 = path_join(a->tools.workspace, "GOALS.md");
        char *goals = read_file(gp2);
        if (goals && goals[0]) buf_printf(&b, "\n# Goals (GOALS.md)\n%s\n", goals);
        free(goals);
        free(gp2);
    }

    {
        char *lp = path_join(a->tools.workspace, "LESSONS.md");
        char *les = read_file(lp);
        if (les && les[0]) buf_printf(&b, "\n# Lessons learned (LESSONS.md)\n%s\n", les);
        free(les);
        free(lp);
    }

    if (a->tools.a2a_peers && a->tools.a2a_peers[0])
        buf_printf(&b, "\n# Peers (ask with the a2a tool)\n%s\n", a->tools.a2a_peers);

    char *skills = tools_skills_listing(&a->tools);
    if (skills && skills[0])
        buf_printf(&b, "\n# Skills (load with load_skill)\n%s", skills);
    free(skills);

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char stamp[64];
    stamp_fmt(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tmv);
    buf_printf(&b, "\n# Environment\ncurrent time: %s\n", stamp);
    return b.s;
}

static void copy_call(ToolCall *dst, const ToolCall *src) {
    dst->id = xstrdup(src->id ? src->id : "");
    dst->name = xstrdup(src->name ? src->name : "");
    dst->args = xstrdup(src->args ? src->args : "{}");
}

/* nested agent runs: isolated context, capped steps, restricted tools.
   "task" runs one; "tasks" runs up to 4 in parallel (one thread each). */
typedef struct {
    const Agent *parent;
    const char *task;
    const char *allow;
    Buf out;
} SubTask;

static void sub_run_one(const Agent *parent, const char *task, const char *allow,
                        Buf *out) {
    Agent sub;
    agent_init(&sub);
    sub.llm = parent->llm;           /* config strings are shared, immutable */
    sub.tools = parent->tools;
    sub.max_steps = 6;
    sub.confirm = parent->confirm;   /* confirm UX (e.g. telegram buttons) works here too */
    sub.cb_user = parent->cb_user;
    sub.auto_approve = parent->auto_approve;
    sub.depth = parent->depth + 1;
    sub.tools_allow = allow;
    buf_init(out);
    agent_prompt(&sub, task, out);
    agent_free(&sub);
}

static void *sub_thread(void *p) {
    SubTask *t = p;
    sub_run_one(t->parent, t->task, t->allow, &t->out);
    return NULL;
}

#define MAX_SUBTASKS 4

static void run_subagent(Agent *parent, const JVal *args, Buf *out) {
    if (parent->depth >= 1) {
        buf_puts(out, "error: subagents cannot nest");
        return;
    }
    char allows[MAX_SUBTASKS][256];
    const char *defallow = "read_file,list_dir,fetch_url,load_skill,remember,search_memory";
    int i;
    for (i = 0; i < MAX_SUBTASKS; i++) snprintf(allows[i], sizeof allows[i], "%s", defallow);
    JVal *tl = json_obj_get((JVal *)args, "tools");
    if (tl && tl->type == J_ARR && tl->nitems > 0) {
        Buf a;
        buf_init(&a);
        for (i = 0; i < tl->nitems; i++) {
            const char *nm = (tl->items[i]->type == J_STR) ? tl->items[i]->str : NULL;
            if (!nm || str_eq(nm, "subagent")) continue;
            if (a.len) buf_puts(&a, ",");
            buf_puts(&a, nm);
        }
        if (a.len)
            for (i = 0; i < MAX_SUBTASKS; i++) snprintf(allows[i], sizeof allows[i], "%s", a.s);
        buf_free(&a);
    }

    SubTask jobs[MAX_SUBTASKS];
    int n = 0;
    const char *one = json_get_str(args, "task");
    if (one && one[0]) {
        jobs[n].parent = parent;
        jobs[n].task = one;
        jobs[n].allow = allows[n];
        n++;
    }
    JVal *arr = json_obj_get((JVal *)args, "tasks");
    if (arr && arr->type == J_ARR) {
        for (i = 0; i < arr->nitems && n < MAX_SUBTASKS; i++) {
            JVal *it = arr->items[i];
            const char *t = (it->type == J_STR) ? it->str : json_get_str(it, "task");
            if (!t || !t[0]) continue;
            if (it->type != J_STR) {
                JVal *w = json_obj_get(it, "tools");
                if (w && w->type == J_ARR && w->nitems > 0) {
                    Buf a;
                    buf_init(&a);
                    int k;
                    for (k = 0; k < w->nitems; k++) {
                        const char *nm = (w->items[k]->type == J_STR) ? w->items[k]->str : NULL;
                        if (!nm || str_eq(nm, "subagent")) continue;
                        if (a.len) buf_puts(&a, ",");
                        buf_puts(&a, nm);
                    }
                    if (a.len) snprintf(allows[n], sizeof allows[n], "%s", a.s);
                    buf_free(&a);
                }
            }
            jobs[n].parent = parent;
            jobs[n].task = t;
            jobs[n].allow = allows[n];
            n++;
        }
    }
    if (n == 0) {
        buf_puts(out, "error: subagent needs a \"task\" or \"tasks\" list");
        return;
    }

    int par = n > 1;
    if (par) {
        pthread_t th[MAX_SUBTASKS];
        int started = 0;
        for (i = 0; i < n; i++)
            if (pthread_create(&th[i], NULL, sub_thread, &jobs[i]) == 0) started++;
            else { jobs[i].out.s = NULL; jobs[i].out.len = 0; }
        if (started == n) {
            for (i = 0; i < n; i++) pthread_join(th[i], NULL);
        } else {
            /* thread shortage: finish everything inline */
            for (i = 0; i < n; i++) {
                pthread_join(th[i], NULL);
                if (!jobs[i].out.s) sub_run_one(parent, jobs[i].task, jobs[i].allow, &jobs[i].out);
            }
        }
    } else {
        sub_run_one(parent, jobs[0].task, jobs[0].allow, &jobs[0].out);
    }

    for (i = 0; i < n; i++) {
        if (n > 1) buf_printf(out, "subagent report [%d/%d]: %.4000s\n", i + 1, n,
                              jobs[i].out.s ? jobs[i].out.s : "(no output)");
        else buf_printf(out, "subagent report: %.4000s", jobs[i].out.s ? jobs[i].out.s : "(no output)");
        buf_free(&jobs[i].out);
    }
    if (arr && arr->type == J_ARR && arr->nitems > MAX_SUBTASKS)
        buf_printf(out, "(note: %d extra tasks skipped, max %d)\n",
                   arr->nitems - (MAX_SUBTASKS - 1), MAX_SUBTASKS);
}

/* ---- compaction: when history outgrows the context budget, replace older
   turns with an LLM-written summary instead of dropping them. The summary is
   persisted to the session like any other turn. ---- */

static size_t hist_est(const Msg *m) {
    size_t n = 24 + strlen(m->role);
    if (m->content) n += strlen(m->content);
    if (m->img_path) n += 64;
    for (int i = 0; i < m->ncalls; i++) {
        n += 16 + (m->calls[i].name ? strlen(m->calls[i].name) : 0)
              + (m->calls[i].args ? strlen(m->calls[i].args) : 0);
    }
    return n;
}

static void agent_compact(Agent *a) {
    long budget = a->llm.context_chars;
    if (budget <= 0 || a->nhist <= 2) return;
    int i;
    /* the generated system prompt is part of every request - count it */
    size_t total = 0;
    {
        char *sys = agent_system_prompt(a);
        if (sys) total += strlen(sys) + 32;
        free(sys);
    }
    for (i = 0; i < a->nhist; i++) total += hist_est(&a->hist[i]);
    if (total <= (size_t)budget) return;

    /* leading system messages stay verbatim */
    int head = 0;
    while (head < a->nhist && str_eq(a->hist[head].role, "system")) head++;

    /* turn groups: a user message opens a group (tool chains stay together) */
    int starts[512], ng = 0;
    if (head < a->nhist && !str_eq(a->hist[head].role, "user")) starts[ng++] = head;
    for (i = head; i < a->nhist && ng < 512; i++)
        if (str_eq(a->hist[i].role, "user")) starts[ng++] = i;
    if (ng < 2) return;

    /* keep the newest groups that fit (budget minus system minus room for the
       summary); compact everything older */
    long long avail; /* recompute below with sys only */
    {
        char *sys = agent_system_prompt(a);
        size_t sys_est = sys ? strlen(sys) + 32 : 0;
        free(sys);
        avail = (long long)budget - (long long)sys_est - 256;
    }
    if (avail < 0) avail = 0;
    int keep_from = ng;
    size_t keep_bytes = 0;
    while (keep_from > 1) {
        int gi = keep_from - 1;
        int s0 = starts[gi];
        int e0 = (gi + 1 < ng) ? starts[gi + 1] : a->nhist;
        size_t g = 0;
        for (i = s0; i < e0; i++) g += hist_est(&a->hist[i]);
        if ((long long)(keep_bytes + g) > avail) break;
        keep_bytes += g;
        keep_from = gi;
    }
    if (keep_from >= ng) keep_from = ng - 1;
    int cut = starts[keep_from];
    if (cut <= head) return;

    /* render the old segment as text for the summarizer */
    Buf seg;
    buf_init(&seg);
    for (i = head; i < cut && seg.len < 12000; i++) {
        const Msg *m = &a->hist[i];
        buf_printf(&seg, "%s: %.300s\n",
                   str_eq(m->role, "assistant") ? "assistant"
                   : str_eq(m->role, "tool") ? "tool" : "user",
                   m->content ? m->content : "");
        int k;
        for (k = 0; k < m->ncalls; k++)
            buf_printf(&seg, "  (tool call: %s %.200s)\n",
                       m->calls[k].name ? m->calls[k].name : "?",
                       m->calls[k].args ? m->calls[k].args : "");
    }
    Msg in[2];
    memset(in, 0, sizeof in);
    in[0].role = (char *)"system";
    in[0].content = (char *)"Compress this conversation for continuity: "
        "facts, preferences, decisions, open tasks, paths.";
    in[1].role = (char *)"user";
    in[1].content = seg.s;
    Msg out;
    memset(&out, 0, sizeof out);
    char *err = NULL;
    if (llm_chat_ex(&a->llm, in, 2, &out, &err, NULL, NULL, 0) == 0 &&
        out.content && out.content[0]) {
        for (i = head; i < cut; i++) msg_clear(&a->hist[i]);
        Msg *sum = &a->hist[head];
        memset(sum, 0, sizeof *sum);
        sum->role = xstrdup("assistant");
        Buf sp;
        buf_init(&sp);
        buf_printf(&sp, "[compacted earlier conversation]\n%s", out.content);
        sum->content = sp.s;
        int w = head + 1;
        for (i = cut; i < a->nhist; i++) a->hist[w++] = a->hist[i];
        a->nhist = w;
        if (a->session_path) hist_truncate(a->session_path);
    }
    free(err);
    msg_clear(&out);
    buf_free(&seg);
}

int agent_prompt(Agent *a, const char *user, Buf *reply) {
    return agent_prompt_img(a, user, NULL, reply);
}

int agent_prompt_img(Agent *a, const char *user, const char *img_path, Buf *reply) {
    agent_compact(a); /* summarize old turns before the request is built */
    /* OS confinement is applied once at startup (main.c) and is permanent;
       with "sandbox": "os" the process either got confined there or refused
       to start.  Nothing to do per prompt. */

    /* working message list: fresh system prompt each call (memory may change) */
    Msg *msgs = NULL;
    int n = 0, cap = 0;
    Msg *push = NULL;

#define PUSH() do { \
        if (n + 1 > cap) { cap = cap ? cap * 2 : 16; msgs = xrealloc(msgs, (size_t)cap * sizeof *msgs); } \
        push = &msgs[n++]; memset(push, 0, sizeof *push); \
    } while (0)

    char *sys = agent_system_prompt(a);
    PUSH();
    push->role = xstrdup("system");
    push->content = sys;

    int i;
    for (i = 0; i < a->nhist; i++) {
        PUSH();
        push->role = xstrdup(a->hist[i].role);
        push->content = xstrdup(a->hist[i].content ? a->hist[i].content : "");
        if (a->hist[i].img_path) push->img_path = xstrdup(a->hist[i].img_path);
        push->tool_call_id = xstrdup(a->hist[i].tool_call_id ? a->hist[i].tool_call_id : "");
        if (a->hist[i].ncalls) {
            push->calls = xmalloc((size_t)a->hist[i].ncalls * sizeof *push->calls);
            memset(push->calls, 0, (size_t)a->hist[i].ncalls * sizeof *push->calls);
            push->ncalls = a->hist[i].ncalls;
            int j;
            for (j = 0; j < a->hist[i].ncalls; j++)
                copy_call(&push->calls[j], &a->hist[i].calls[j]);
        }
    }
    hist_add(a, "user", user);
    PUSH();
    push->role = xstrdup("user");
    push->content = xstrdup(user);
    if (img_path && img_path[0]) push->img_path = xstrdup(img_path);

    int step, rc = 0;
    int calls_this_run = 0, used_write_skill = 0;
    a->streamed_reply = 0;
    for (step = 0; step <= a->max_steps; step++) {
        Msg out;
        char *err = NULL;
        if (a->on_status) a->on_status(a->cb_user, "thinking");
        if (llm_chat(&a->llm, msgs, n, &out, &err, a->on_delta, a->cb_user) != 0) {
            /* one retry on transient failures (network/5xx/rate limit) */
            if (err && (strstr(err, "connect") || strstr(err, "HTTP 5") ||
                        strstr(err, "429") || strstr(err, "timed out") ||
                        strstr(err, "reset"))) {
                free(err);
                err = NULL;
                msg_clear(&out);
                if (a->on_status) a->on_status(a->cb_user, "retrying");
                sleep(1);
                if (llm_chat(&a->llm, msgs, n, &out, &err, a->on_delta, a->cb_user) == 0)
                    goto llm_ok;
            }
            buf_printf(reply, "[agent error] %s", err ? err : "unknown");
            free(err);
            msg_clear(&out);
            rc = -1;
            break;
        }
llm_ok:
        /* move assistant msg into working list + history */
        PUSH();
        *push = out; /* struct copy incl. owned pointers */

        Msg *h = hist_push(a);
        h->role = xstrdup("assistant");
        h->content = xstrdup(out.content ? out.content : "");
        if (out.ncalls) {
            h->calls = xmalloc((size_t)out.ncalls * sizeof *h->calls);
            memset(h->calls, 0, (size_t)out.ncalls * sizeof *h->calls);
            h->ncalls = out.ncalls;
            int j;
            for (j = 0; j < out.ncalls; j++)
                copy_call(&h->calls[j], &out.calls[j]);
        }
        if (a->session_path) hist_append(a->session_path, h);

        if (out.ncalls == 0) {
            buf_puts(reply, out.content ? out.content : "");
            if (a->on_delta && out.content && out.content[0]) a->streamed_reply = 1;
            break;
        }
        if (step == a->max_steps) {
            buf_puts(reply, "[agent stopped: max tool steps reached]");
            rc = -1;
            break;
        }
        /* execute tools */
        int j;
        for (j = 0; j < out.ncalls; j++) {
            Buf result;
            buf_init(&result);
            JVal *args = json_parse(out.calls[j].args);
            const char *tname = out.calls[j].name;
            int executed = 1;
            if (!args && out.calls[j].args && out.calls[j].args[0] &&
                !str_eq(out.calls[j].args, "{}")) {
                buf_puts(&result, "error: malformed JSON arguments - re-issue "
                                  "this tool call with a valid JSON object");
                executed = 0;
            }

            if (tools_is_risky(tname, args) && !a->auto_approve) {
                char desc[320];
                snprintf(desc, sizeof desc, "%.240s %.60s", tname ? tname : "?",
                         out.calls[j].args ? out.calls[j].args : "");
                int approved;
                if (a->confirm) approved = a->confirm(a->cb_user, desc);
                else approved = 0; /* no way to ask (heartbeat): refuse */
                if (!approved) {
                    buf_puts(&result, "refused: needs confirmation and was not "
                                      "approved. Do not retry; ask the user.");
                    executed = 0;
                }
            }
            if (executed && str_eq(tname, "subagent")) {
                if (a->on_status) a->on_status(a->cb_user, "tool: subagent");
                run_subagent(a, args, &result);   /* handles nesting itself */
            } else if (executed && a->tools_allow && a->tools_allow[0] &&
                       !csv_has(a->tools_allow, tname)) {
                buf_free(&result);
                buf_init(&result);
                buf_puts(&result, "error: tool not available to this subagent");
            } else if (executed) {
                if (a->on_status && tname) {
                    char phase[160];
                    snprintf(phase, sizeof phase, "tool: %s", tname);
                    a->on_status(a->cb_user, phase);
                }
                tools_exec(&a->tools, tname, args, &result);
            }
            json_free(args);

            if (tname) {
                calls_this_run++;
                if (str_eq(tname, "write_skill")) used_write_skill = 1;
            }
            audit_log(a->tools.workspace, a->session_name ? a->session_name : "ephemeral",
                      tname, out.calls[j].args, result.s);

            char tid[128];
            snprintf(tid, sizeof tid, "%s",
                     out.calls[j].id && out.calls[j].id[0] ? out.calls[j].id : "call_0");

            PUSH();
            push->role = xstrdup("tool");
            push->content = xstrdup(result.s);
            push->tool_call_id = xstrdup(tid);

            Msg *ht = hist_push(a);
            ht->role = xstrdup("tool");
            ht->content = xstrdup(result.s);
            ht->tool_call_id = xstrdup(tid);
            if (a->session_path) hist_append(a->session_path, ht);
            buf_free(&result);
        }
    }
#undef PUSH

    for (i = 0; i < n; i++) msg_clear(&msgs[i]);
    free(msgs);
    agent_trim_budget(a);

    /* auto-learn (Hermes-style): after a complex task, extract a skill or
       lesson while the experience is fresh */
    if (rc == 0 && !a->learning_pass && a->auto_learn &&
        calls_this_run >= 5 && !used_write_skill) {
        Agent lp;
        memset(&lp, 0, sizeof lp);
        agent_init(&lp);
        lp.llm = a->llm;
        lp.tools = a->tools;
        lp.max_steps = 3;
        lp.learning_pass = 1;
        lp.auto_learn = 0;
        lp.session_name = xstrdup("learn");
        Buf task;
        buf_init(&task);
        buf_printf(&task, "Learning pass. Task done: %.600s\n"
                          "(%d calls.) Reusable? write_skill. Lesson? reflect. "
                          "Nothing? Reply exactly: DONE",
                   user, calls_this_run);
        Buf lr;
        buf_init(&lr);
        agent_prompt(&lp, task.s, &lr);
        buf_free(&task);
        buf_free(&lr);
        agent_free(&lp);
    }
    return rc;
}
