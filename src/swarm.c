/* clawc swarm: a bounded process scheduler for local and remote agents. */
#define _XOPEN_SOURCE 700
#include "packed_text.h"
#include "swarm.h"
#include "json.h"
#include "tools.h"
#include "util.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct { const char *id, *role, *peer; int busy; } Member;
typedef struct Job { int dst; char *from, *room, *text; struct Job *next; } Job;
typedef struct { pid_t pid; int fd, dst; Buf data; } Worker;
typedef struct {
    Agent *base; JVal *spec; Member *members, **index; int nm; Buf targets;
    Job *head, *tail; size_t queued, qmax, emitted, max;
    Worker *workers; int nw; int failed;
} Swarm;

static int valid_id(const char *s) {
    if (!s || !*s || strlen(s) > 26 || str_eq(s, "user")) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
                           (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return 0;
    return 1;
}
/* A sorted pointer index preserves declaration order in prompts and gives
   logarithmic lookup without an arbitrary member-count limit. */
static int member_cmp(const void *a, const void *b) {
    const Member *const *x = a, *const *y = b;
    return strcmp((*x)->id, (*y)->id);
}
static int member(Swarm *s, const char *id) {
    if (!id) return -1;
    int lo = 0, hi = s->nm;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2, c = strcmp(id, s->index[mid]->id);
        if (!c) return (int)(s->index[mid] - s->members);
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return -1;
}
static JVal *group(Swarm *s, const char *id) {
    JVal *a = json_obj_get(s->spec, "groups"); int i;
    if (!a || a->type != J_ARR) return NULL;
    for (i = 0; i < a->nitems; i++)
        if (str_eq(json_get_str(a->items[i], "id"), id)) return a->items[i];
    return NULL;
}
static void job_free(Job *j) { free(j->from); free(j->room); free(j->text); free(j); }
static int enqueue(Swarm *s, int dst, const char *from, const char *room, const char *text) {
    Job *j = xmalloc(sizeof *j);
    j->dst = dst; j->from = xstrdup(from); j->room = xstrdup(room); j->text = xstrdup(text); j->next = NULL;
    if (s->tail) s->tail->next = j; else s->head = j;
    s->tail = j; s->queued++;
    return 0;
}
static void log_send(const char *from, const char *to, const char *text) {
    Buf b; buf_init(&b); json_append_quoted(&b, "{\"from\":", from);
    json_append_quoted(&b, ",\"to\":", to);
    json_append_quoted(&b, ",\"text\":", text); buf_puts(&b, "}\n");
    fputs(b.s, stdout); fflush(stdout); buf_free(&b);
}
/* Destination starts @ for a direct message, # for a group. */
static int route(Swarm *s, const char *from, const char *to, const char *text) {
    if (!from || !to || !text || !text[0]) return -1;
    if (to[0] == '@') {
        int dst = member(s, to + 1);
        if (dst < 0 || s->queued >= s->qmax || (s->max && s->emitted >= s->max)) return -1;
        log_send(from,to,text); s->emitted++; return enqueue(s, dst, from, to, text);
    }
    if (to[0] != '#') return -1;
    JVal *g = group(s, to + 1), *m = g ? json_obj_get(g, "members") : NULL;
    if (!m || m->type != J_ARR) return -1;
    int i, n = 0, sender_is_member = str_eq(from, "user");
    for (i = 0; i < m->nitems; i++) {
        const char *id = m->items[i]->type == J_STR ? m->items[i]->str : NULL;
        if (str_eq(id, from)) sender_is_member = 1;
        if (!id || member(s, id) < 0) return -1;
        if (!str_eq(id, from)) n++;
    }
    if (!sender_is_member || (size_t)n > s->qmax - s->queued ||
        (s->max && (size_t)n > s->max - s->emitted)) return -1;
    log_send(from,to,text);
    for (i = 0; i < m->nitems; i++) {
        const char *id = m->items[i]->str;
        if (!str_eq(id, from)) { s->emitted++; enqueue(s, member(s,id), from, to, text); }
    }
    return 0;
}
static Job *pop_ready(Swarm *s) {
    Job *p = NULL, *j = s->head;
    while (j && s->members[j->dst].busy) { p = j; j = j->next; }
    if (!j) return NULL;
    if (p) p->next = j->next; else s->head = j->next;
    if (s->tail == j) s->tail = p;
    s->queued--; j->next = NULL; s->members[j->dst].busy = 1; return j;
}
static void child_run(Swarm *s, Job *j, int out) {
    Member *m = &s->members[j->dst]; Buf prompt, reply, result;
    buf_init(&prompt); buf_init(&reply); buf_init(&result);
    buf_printf(&prompt, "You are swarm agent %s. Role: %s\n", m->id, m->role);
    buf_puts(&prompt, s->targets.s);
    buf_printf(&prompt, "\nMessage from %s in %s:\n%s\n", j->from, j->room, j->text);
    buf_puts(&prompt, packed_text(TXT_019));
    int rc;
    if (m->peer && m->peer[0]) {
        Buf arg; buf_init(&arg); json_append_quoted(&arg, "{\"peer\":", m->peer);
        json_append_quoted(&arg, ",\"task\":", prompt.s);
        buf_puts(&arg, ",\"from\":");
        Buf sender; buf_init(&sender); buf_printf(&sender, "swarm-%s", m->id); json_quote(&arg,sender.s); buf_free(&sender); buf_puts(&arg,"}");
        JVal *v = json_parse(arg.s); tools_exec(&s->base->tools, "a2a", v, &reply);
        rc = reply.s && !str_starts(reply.s, "error:") ? 0 : -1;
        json_free(v); buf_free(&arg);
    } else {
        Agent a; agent_init(&a); a.llm = s->base->llm; a.tools = s->base->tools;
        a.max_steps = s->base->max_steps; a.auto_approve = s->base->auto_approve; a.auto_learn = 0;
        Buf path; buf_init(&path); buf_printf(&path, "%s/history/swarm-%s.jsonl", a.tools.workspace, m->id);
        agent_open_session(&a, path.s); buf_free(&path);
        rc = agent_prompt(&a, prompt.s, &reply); agent_free(&a);
    }
    buf_puts(&result, rc ? "{\"ok\":false,\"reply\":" : "{\"ok\":true,\"reply\":");
    json_quote(&result, reply.s ? reply.s : ""); buf_puts(&result, "}\n");
    size_t pos = 0; while (pos < result.len) { ssize_t n = write(out, result.s + pos, result.len - pos); if (n < 0 && errno == EINTR) continue; if (n <= 0) break; pos += (size_t)n; }
    close(out); buf_free(&prompt); buf_free(&reply); buf_free(&result); _exit(rc ? 1 : 0);
}
static int launch(Swarm *s, Worker *w, Job *j) {
    int fds[2]; if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (!pid) { int i; close(fds[0]); for (i = 0; i < s->nw; i++) if (s->workers[i].pid) close(s->workers[i].fd); child_run(s,j,fds[1]); }
    close(fds[1]); w->pid = pid; w->fd = fds[0]; w->dst = j->dst; buf_init(&w->data); return 0;
}
static void finish(Swarm *s, Worker *w) {
    int status = 0; close(w->fd); while (waitpid(w->pid, &status, 0) < 0 && errno == EINTR) {}
    JVal *v = w->data.s ? json_parse(w->data.s) : NULL, *ok = v ? json_obj_get(v,"ok") : NULL;
    const char *reply = v ? json_get_str(v,"reply") : NULL;
    const char *id = s->members[w->dst].id;
    if (!ok || ok->type != J_BOOL || !ok->boolean || !reply || !WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr,"[swarm] %s failed: %s\n", id, reply ? reply : "invalid worker output"); s->failed = 1;
    } else {
        JVal *response = json_parse(reply), *messages = response ? json_obj_get(response, "messages") : NULL;
        const char *report = response ? json_get_str(response, "report") : NULL;
        if (!response || response->type != J_OBJ || !messages || messages->type != J_ARR) {
            fprintf(stderr,"[swarm] %s returned invalid message JSON\n",id); s->failed = 1;
        } else {
            Buf line; buf_init(&line); json_append_quoted(&line, "{\"agent\":", id);
            json_append_quoted(&line, ",\"report\":", report ? report : ""); buf_puts(&line,"}\n");
            fputs(line.s,stdout); fflush(stdout); buf_free(&line);
            int i; for (i=0; i<messages->nitems; i++) {
                const char *to=json_get_str(messages->items[i],"to"), *text=json_get_str(messages->items[i],"text");
                if (route(s,id,to,text) != 0) { fprintf(stderr,"[swarm] invalid destination or queue/message limit reached from %s\n",id); s->failed=1; break; }
            }
        }
        json_free(response);
    }
    json_free(v); buf_free(&w->data); s->members[w->dst].busy=0; w->pid=0;
}
int swarm_run(Agent *base, const char *path) {
    Swarm s; memset(&s,0,sizeof s); s.base=base;
    char *raw=read_file(path); if (!raw || !(s.spec=json_parse(raw))) { fprintf(stderr,"[swarm] cannot parse %s\n",path); free(raw); return 1; } free(raw);
    JVal *agents=json_obj_get(s.spec,"agents"), *initial=json_obj_get(s.spec,"messages");
    if (!agents || agents->type!=J_ARR || agents->nitems <= 0 || !initial || initial->type!=J_ARR) { fprintf(stderr,"[swarm] agents and messages arrays required\n"); json_free(s.spec); return 1; }
    s.nm=agents->nitems;
    if ((size_t)s.nm > (size_t)-1 / sizeof *s.members) {
        fprintf(stderr,"[swarm] too many agents for this platform\n"); json_free(s.spec); return 1;
    }
    s.members=xmalloc((size_t)s.nm*sizeof *s.members); memset(s.members,0,(size_t)s.nm*sizeof *s.members);
    s.index=xmalloc((size_t)s.nm*sizeof *s.index);
    int i; for (i=0;i<s.nm;i++) {
        Member *m=&s.members[i]; m->id=json_get_str(agents->items[i],"id"); m->role=json_get_str(agents->items[i],"role"); m->peer=json_get_str(agents->items[i],"peer");
        if (!valid_id(m->id) || !m->role) { fprintf(stderr,"[swarm] invalid or duplicate agent\n"); s.failed=1; break; }
        s.index[i]=m;
    }
    if (!s.failed) {
        qsort(s.index, (size_t)s.nm, sizeof *s.index, member_cmp);
        for (i=1;i<s.nm;i++) if (str_eq(s.index[i-1]->id,s.index[i]->id)) { fprintf(stderr,"[swarm] invalid or duplicate agent\n"); s.failed=1; break; }
    }
    buf_init(&s.targets); buf_puts(&s.targets, packed_text(TXT_020));
    for (i=0; !s.failed && i<s.nm; i++) buf_printf(&s.targets, " @%s", s.members[i].id);
    JVal *groups=json_obj_get(s.spec, "groups");
    if (groups && groups->type==J_ARR) for (i=0; !s.failed && i<groups->nitems; i++) {
        const char *id=json_get_str(groups->items[i],"id"); if (id) buf_printf(&s.targets," #%s",id);
    }
    double nw=json_get_num(s.spec,"workers",4), qm=json_get_num(s.spec,"queue_limit",1024), max=json_get_num(s.spec,"message_limit",1000);
    if (!(nw>=1 && nw<=100000 && (double)(int)nw==nw && qm>=1 && qm<=10000000 && (double)(size_t)qm==qm && max>=0 && max<=10000000 && (double)(size_t)max==max)) s.failed=1;
    if (!s.failed) { s.nw=(int)nw; if (s.nw>s.nm) s.nw=s.nm; s.qmax=(size_t)qm; s.max=(size_t)max; }
    for (i=0;!s.failed && i<initial->nitems;i++) if (route(&s,"user",json_get_str(initial->items[i],"to"),json_get_str(initial->items[i],"text"))!=0) { fprintf(stderr,"[swarm] invalid initial message\n"); s.failed=1; }
    if (!s.failed) { s.workers=xmalloc((size_t)s.nw*sizeof *s.workers); memset(s.workers,0,(size_t)s.nw*sizeof *s.workers); }
    while (!s.failed) {
        int active=0; for (i=0;i<s.nw;i++) {
            Worker *w=&s.workers[i]; if (!w->pid) { Job *j=pop_ready(&s); if (j) { if (launch(&s,w,j)!=0) { s.failed=1; s.members[j->dst].busy=0; fprintf(stderr,"[swarm] cannot start worker\n"); } job_free(j); } }
            if (w->pid) active++;
        }
        if (!active) break;
        struct pollfd *p=xmalloc((size_t)s.nw*sizeof *p);
        for (i=0;i<s.nw;i++) { p[i].fd=s.workers[i].pid?s.workers[i].fd:-1; p[i].events=POLLIN; p[i].revents=0; }
        int r=poll(p,(nfds_t)s.nw,-1); if (r<0 && errno!=EINTR) s.failed=1;
        if (r>0) for (i=0;i<s.nw;i++) if (p[i].revents) {
            Worker *w=&s.workers[i]; char data[4096]; ssize_t n=read(w->fd,data,sizeof data);
            if (n>0) { buf_add(&w->data,data,(size_t)n); if (w->data.len>1024*1024) { fprintf(stderr,"[swarm] worker reply too large\n"); s.failed=1; } }
            else if (n==0) finish(&s,w); else if (errno!=EINTR) s.failed=1;
        }
        free(p);
    }
    if (s.workers) for (i=0;i<s.nw;i++) if (s.workers[i].pid) { kill(s.workers[i].pid,SIGTERM); close(s.workers[i].fd); waitpid(s.workers[i].pid,NULL,0); buf_free(&s.workers[i].data); }
    while (s.head) { Job *j=s.head; s.head=j->next; job_free(j); }
    free(s.workers); free(s.index); free(s.members); buf_free(&s.targets); json_free(s.spec); return s.failed?1:0;
}
