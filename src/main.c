/* clawc - main.c: CLI, config resolution, chat, heartbeat, daemon supervisor,
   TCP bridge server. */
#define _XOPEN_SOURCE 700
#include "packed_text.h"
#include "agent.h"
#include "swarm.h"
#include "config.h"
#include "cron.h"
#include "jail.h"
#include "sandbox.h"
#include "bridge_tls.h"
#ifndef CLAWC_NO_MATRIX
#include "matrix.h"
#endif
#ifndef CLAWC_NO_DISCORD
#include "discord.h"
#endif
#include "daemon.h"
#include "json.h"
#include "telegram.h"
#include "http.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    Agent agent;
    int quiet;
    /* channel settings resolved from config/env/flags */
    TgCfg tg;
    int serve_port;    /* -1 = unset: serve verb defaults to 8848; daemon: off */
    const char *serve_cert, *serve_key;
    int serve_tls, serve_workers;
    const char *serve_bind; /* NULL = loopback only (127.0.0.1); "0.0.0.0" = exposed */
    int hb_interval;   /* -1 = unset: daemon heartbeat gateway off */
    int au_interval;   /* -1 = unset: daemon autonomous gateway off */
    int hb_forward_tg; /* daemon: post heartbeat messages to telegram allowlist */
    char *notify_cmd;  /* command receiving non-OK heartbeat messages */
#ifndef CLAWC_NO_MATRIX
    MxCfg mx;
#endif
#ifndef CLAWC_NO_DISCORD
    DcCfg dc;
#endif
} App;

/* UX callbacks (defined below) */
static int confirm_stdin(void *user, const char *desc);
static void on_status_stderr(void *user, const char *phase);
static void on_delta_stdout(void *user, const char *text, size_t n);

static void usage(FILE *f) {
    fputs(packed_text(TXT_USAGE_HEAD), f);
#ifndef CLAWC_NO_TELEGRAM
    fputs(" telegram", f);
#endif
    fputs(packed_text(TXT_USAGE_TAIL), f);
}

static char *default_workspace(void) {
    const char *ws = getenv("CLAWC_WORKSPACE");
    if (ws && ws[0]) return xstrdup(ws);
    const char *home = getenv("HOME");
    Buf b;
    buf_init(&b);
    buf_printf(&b, "%s/.clawc/workspace", home && home[0] ? home : ".");
    return b.s;
}

static void ws_write_if_missing(const char *ws, const char *name, const char *content) {
    char *path = path_join(ws, name);
    if (!file_exists(path) && write_file(path, content) != 0)
        fprintf(stderr, "clawc: cannot write %s\n", path);
    free(path);
}

static void cmd_init(const char *ws) {
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s/skills'", ws);
    if (system(cmd) != 0) {
        /* fallback: try workspace itself via proc-less mkdir */
        mkdir(ws, 0755);
        char *sk = path_join(ws, "skills");
        mkdir(sk, 0755);
        free(sk);
    }
    ws_write_if_missing(ws, "SOUL.md",
        packed_text(TXT_EXTRA_00));
    ws_write_if_missing(ws, "MEMORY.md", "# Memory\n");
    ws_write_if_missing(ws, "LESSONS.md",
        packed_text(TXT_EXTRA_01));
    ws_write_if_missing(ws, "GOALS.md",
        packed_text(TXT_EXTRA_02));
    ws_write_if_missing(ws, "HEARTBEAT.md",
        packed_text(TXT_EXTRA_03));
    char *p = path_join(ws, "skills");
    char *sk = path_join(p, "example");
    mkdir(sk, 0755);
    free(sk);
    free(p);
    ws_write_if_missing(ws, "skills/example/SKILL.md",
        packed_text(TXT_EXTRA_04));
    printf("workspace ready: %s\n", ws);
}

static void notify_send(App *app, const char *msg) {
    /* daemon heartbeat: deliver to telegram allowlist chats, if configured */
#ifndef CLAWC_NO_TELEGRAM
    if (app->hb_forward_tg && app->tg.token && app->tg.allow && app->tg.nallow > 0) {
        int i;
        for (i = 0; i < app->tg.nallow; i++)
            tg_send_text(&app->tg, app->tg.allow[i], msg);
    }
#endif
    const char *cmd = app->notify_cmd;
    if (!cmd || !cmd[0]) return;
    char tmp[] = "/tmp/clawc-notify-XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) return;
    { ssize_t w = write(fd, msg, strlen(msg)); (void)w; }
    close(fd);
    Buf b;
    buf_init(&b);
    buf_printf(&b, "%s < '%s'", cmd, tmp);
    Buf out;
    buf_init(&out);
    proc_run(b.s, 30, &out, 4096);
    unlink(tmp);
    buf_free(&b);
    buf_free(&out);
}

#ifndef CLAWC_NO_HEARTBEAT
static int cmd_heartbeat(App *app) {
    char *p = path_join(app->agent.tools.workspace, "HEARTBEAT.md");
    char *hb = read_file(p);
    free(p);
    if (!hb) {
        fputs(packed_text(TXT_EXTRA_07), stderr);
        return 1;
    }
    Buf prompt;
    buf_init(&prompt);
    buf_puts(&prompt,
        packed_text(TXT_009));
    buf_puts(&prompt, hb);
    free(hb);

    app->agent.heartbeat_mode = 1;
    Buf reply;
    buf_init(&reply);
    int rc = agent_prompt(&app->agent, prompt.s, &reply);
    buf_trim(&reply);
    if (rc == 0 && !str_eq(reply.s, "HEARTBEAT_OK")) {
        printf("%s\n", reply.s);
        notify_send(app, reply.s);
    } else if (rc != 0) {
        fprintf(stderr, "clawc: heartbeat error: %s\n", reply.s);
    } else {
        fprintf(stderr, "[heartbeat] HEARTBEAT_OK\n");
    }
    buf_free(&prompt);
    buf_free(&reply);
    return rc == 0 ? 0 : 1;
}

#endif /* heartbeat */

static int cmd_run(App *app, int argc, char **argv, int yes_auto) {
    Buf prompt;
    buf_init(&prompt);
    if (argc > 0) {
        int i;
        for (i = 0; i < argc; i++) {
            if (i) buf_puts(&prompt, " ");
            buf_puts(&prompt, argv[i]);
        }
    } else {
        char tmp[4096];
        size_t n;
        while ((n = fread(tmp, 1, sizeof tmp, stdin)) > 0) buf_add(&prompt, tmp, n);
    }
    /* explicit CLI action: auto-approve when non-interactive or --yes */
    app->agent.cb_user = app;
    app->agent.on_status = on_status_stderr;
    app->agent.on_delta = on_delta_stdout;
    app->agent.auto_approve = 1;
    app->agent.confirm = NULL;
    if (!yes_auto && isatty(fileno(stdin))) {
        app->agent.auto_approve = 0;
        app->agent.confirm = confirm_stdin;
    }

    Buf reply;
    buf_init(&reply);
    app->agent.streamed_reply = 0;
    int rc = agent_prompt(&app->agent, prompt.s, &reply);
    if (!app->agent.streamed_reply) printf("%s\n", reply.s);
    else printf("\n");
    buf_free(&prompt);
    buf_free(&reply);
    return rc == 0 ? 0 : 1;
}

/* ---- UX callbacks for the CLI channels ---- */

static int confirm_stdin(void *user, const char *desc) {
    (void)user;
    fprintf(stderr, "\n\342\232\240\357\270\217  risky action: %.280s\nApprove? [y/N] ", desc);
    fflush(stderr);
    char line[16];
    if (!fgets(line, sizeof line, stdin)) return 0;
    return line[0] == 'y' || line[0] == 'Y';
}

static void on_status_stderr(void *user, const char *phase) {
    App *app = user;
    if (!app->quiet) fprintf(stderr, "[%s]\n", phase);
}

static void on_delta_stdout(void *user, const char *text, size_t n) {
    (void)user;
    fwrite(text, 1, n, stdout);
    fflush(stdout);
}

static void print_cmd_status(App *app) {
    Agent *a = &app->agent;
    printf("clawc " CLAWC_VERSION "\n"
           "model: %s (%s)\n"
           "api: %.80s\n"
           "workspace: %.120s\n"
           "session: %s (%d messages)\n"
           "sandbox: %s\n",
           a->llm.model ? a->llm.model : "?",
           a->llm.api_kind == API_ANTHROPIC ? "anthropic" : "openai-compatible",
           a->llm.api_url ? a->llm.api_url : "?",
           a->tools.workspace ? a->tools.workspace : "?",
           a->session_name ? a->session_name : "ephemeral",
           a->nhist,
           a->tools.sandbox ? a->tools.sandbox : "off");
}

static void print_cmd_memory(App *app) {
    char *mem = tools_read_memory(&app->agent.tools);
    if (mem && mem[0]) printf("%.2000s\n", mem);
    else printf("(memory is empty)\n");
    free(mem);
}

static int cmd_chat(App *app, int yes_auto) {
    char *line = NULL;
    size_t cap = 0;
    {
        Buf p;
        buf_init(&p);
        buf_printf(&p, "%s/history/chat.jsonl", app->agent.tools.workspace);
        agent_open_session(&app->agent, p.s);
        buf_free(&p);
    }
    printf("clawc " CLAWC_VERSION " - chat. /help for commands, /quit to exit.\n");
    app->agent.cb_user = app;
    app->agent.on_status = on_status_stderr;
    app->agent.on_delta = on_delta_stdout;
    app->agent.confirm = yes_auto ? NULL : confirm_stdin;
    app->agent.auto_approve = 0;
    for (;;) {
        printf("> ");
        fflush(stdout);
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) break;
        char *t = str_trim(line);
        if (!t[0]) continue;
        if (str_eq(t, "/quit") || str_eq(t, "/exit")) break;
        if (str_eq(t, "/reset")) {
            puts(agent_clear_session(&app->agent) ? "(history storage failed)" : "(history cleared)");
            continue;
        }
        if (str_starts(t, "/help") || str_starts(t, "/start")) {
            fputs(packed_text(TXT_STDOUT_00), stdout);
            continue;
        }
        if (str_starts(t, "/status")) { print_cmd_status(app); continue; }
        if (str_starts(t, "/goal")) {
            const char *p = t + 5;
            while (*p == ' ') p++;
            Buf out;
            buf_init(&out);
            if (!p[0]) {
                char *gp = path_join(app->agent.tools.workspace, "GOALS.md");
                char *g = read_file(gp);
                free(gp);
                if (g && g[0]) buf_printf(&out, "%.1400s", g);
                else buf_puts(&out, packed_text(TXT_010));
                free(g);
            } else if (str_starts(p, "add "))
                goal_add_text(&app->agent.tools, p + 4, NULL, NULL, &out);
            else if (str_starts(p, "done "))
                goal_done_text(&app->agent.tools, p + 5, &out);
            else if (str_starts(p, "rm "))
                goal_rm_text(&app->agent.tools, p + 3, &out);
            else
                buf_puts(&out, packed_text(TXT_011));
            printf("%s\n", out.s);
            buf_free(&out);
            continue;
        }
        if (str_starts(t, "/memory")) { print_cmd_memory(app); continue; }
        if (str_starts(t, "/model")) {
            printf("model: %s (%s, %s)\n",
                   app->agent.llm.model ? app->agent.llm.model : "?",
                   app->agent.llm.api_kind == API_ANTHROPIC ? "anthropic" : "openai-compatible",
                   app->agent.llm.api_url ? app->agent.llm.api_url : "?");
            continue;
        }
        Buf reply;
        buf_init(&reply);
        app->agent.streamed_reply = 0;
        agent_prompt(&app->agent, t, &reply);
        if (!app->agent.streamed_reply) printf("%s", reply.s);
        printf("\n");
        buf_free(&reply);
    }
    free(line);
    return 0;
}

#ifndef CLAWC_NO_SERVE
/* Identity is exact: rejecting ambiguous spellings avoids session collisions.
   Missing/empty identity keeps the existing shared default session. */
static int serve_name(const char *from, char name[33]) {
    size_t n = 0;
    if (!from || !*from) from = "default";
    for (; from[n]; n++) {
        char c = from[n];
        if (n >= 32 || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '-' || c == '_')) return -1;
        name[n] = c;
    }
    name[n] = 0;
    return 0;
}
/* Locks have separate files so opening/closing the history cannot release them.
   Busy same-identity requests get an explicit error rather than tying up all
   workers; clients can retry.  No process-global session cache is needed. */
typedef struct { Agent agent; int lock, tempfd; char *path, *temp; } ServeSession;
static void serve_session_close(ServeSession *s) {
    if (s->tempfd >= 0) close(s->tempfd);
    if (s->temp) unlink(s->temp);
    if (s->lock >= 0) close(s->lock);
    free(s->temp); free(s->path);
}
static int serve_session(App *app, const char *from, ServeSession *session) {
    char name[33];
    memset(session, 0, sizeof *session); session->lock = session->tempfd = -1;
    if (serve_name(from, name) != 0) return -2;
    Buf path, lock, tmp; buf_init(&path); buf_init(&lock); buf_init(&tmp);
    buf_printf(&path, "%s/history", app->agent.tools.workspace);
    if (mkdir(path.s, 0700) != 0 && errno != EEXIST) goto failed;
    buf_printf(&tmp, "%s/.srv-tmp-XXXXXX", path.s);
    buf_printf(&path, "/srv-%s.jsonl", name);
    buf_printf(&lock, "%s.lock", path.s);
    session->lock = open(lock.s, O_CREAT | O_RDWR | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (session->lock < 0) goto failed;
    struct stat st;
    if (fcntl(session->lock, F_SETFD, FD_CLOEXEC) < 0 || fstat(session->lock, &st) < 0 || !S_ISREG(st.st_mode)) goto failed;
    struct flock fl; memset(&fl, 0, sizeof fl); fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET;
    if (fcntl(session->lock, F_SETLK, &fl) < 0) {
        int e = errno; buf_free(&path); buf_free(&lock); buf_free(&tmp); serve_session_close(session);
        return e == EACCES || e == EAGAIN ? -4 : -3;
    }
    int history = open(path.s, O_CREAT | O_RDONLY | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (history < 0) goto failed;
    if (fstat(history, &st) < 0 || !S_ISREG(st.st_mode)) { close(history); goto failed; }
    session->tempfd = mkstemp(tmp.s);
    if (session->tempfd < 0) { close(history); goto failed; }
    session->path = xstrdup(path.s); session->temp = xstrdup(tmp.s);
    if (fcntl(session->tempfd, F_SETFD, FD_CLOEXEC) < 0) { close(history); goto failed; }
    char block[4096]; ssize_t n; int copy_ok = 1;
    while ((n = read(history, block, sizeof block)) != 0) {
        if (n < 0) { if (errno == EINTR) continue; copy_ok = 0; break; }
        if (memchr(block, 0, (size_t)n)) { copy_ok = 0; break; }
        ssize_t pos = 0;
        while (pos < n) { ssize_t w = write(session->tempfd, block + pos, (size_t)(n - pos));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { copy_ok = 0; break; }
            pos += w;
        }
        if (!copy_ok) break;
    }
    close(history);
    if (!copy_ok) goto failed;
    agent_init(&session->agent); session->agent.llm = app->agent.llm; session->agent.tools = app->agent.tools;
    session->agent.max_steps = app->agent.max_steps; session->agent.auto_approve = 1;
    agent_open_session(&session->agent, session->temp);
    free(session->agent.session_name);
    session->agent.session_name = xmalloc(strlen(name) + 5);
    sprintf(session->agent.session_name, "srv-%s", name);
    buf_free(&path); buf_free(&lock); buf_free(&tmp);
    return 0;
failed:
    buf_free(&path); buf_free(&lock); buf_free(&tmp); serve_session_close(session); return -3;
}
static volatile sig_atomic_t serve_stop;
static void serve_stop_handler(int sig) { (void)sig; serve_stop = 1; }

/* Tokens are 64 hex characters (generate using: openssl rand -hex 32). */
static int bridge_valid(const char *s) {
    size_t i;
    if (!s || strlen(s) != 64) return 0;
    for (i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') ||
              (s[i] >= 'A' && s[i] <= 'F'))) return 0;
    return 1;
}

static int bridge_match(const char *a, const char *b) {
    unsigned diff = 0;
    size_t i;
    if (!bridge_valid(b)) return 0;
    for (i = 0; i < 64; i++) diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

static void serve_client(App *app, BridgeTlsCtx *tlsctx, int c) {
        BridgeTls *conn = app->serve_tls ? bridge_tls_accept(tlsctx, c, 5) : NULL;
        if (app->serve_tls && !conn) { close(c); return; }
        Buf line;
        buf_init(&line);
        char ch;
        int complete = 0;
        struct timespec start, now;
        if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
            bridge_tls_free(conn); close(c); buf_free(&line); return;
        }
        for (;;) {
            struct pollfd pfd = { c, POLLIN, 0 };
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) break;
            long long elapsed = (now.tv_sec - start.tv_sec) * 1000LL +
                                (now.tv_nsec - start.tv_nsec) / 1000000LL;
            if (elapsed >= 5000) break; /* absolute deadline, even for trickle input */
            if (conn) {
                if (bridge_tls_read(conn, &ch, 1, (int)((5999 - elapsed) / 1000)) != 1) break;
            } else {
                int ready = poll(&pfd, 1, (int)(5000 - elapsed));
                if (ready < 0 && errno == EINTR) continue;
                if (ready <= 0 || recv(c, &ch, 1, 0) != 1) break;
            }
            if (ch == '\n') { complete = 1; break; }
            if (line.len >= 1024 * 1024) break;
            buf_add(&line, &ch, 1);
        }
        if (!complete || line.len == 0) { bridge_tls_free(conn); close(c); buf_free(&line); return; }
        JVal *req = json_parse(line.s);
        const char *text = req ? json_get_str(req, "text") : NULL;
        const char *from = req ? json_get_str(req, "from") : NULL;
        JVal *from_value = req ? json_obj_get(req, "from") : NULL;
        Buf resp;
        buf_init(&resp);
        if (!bridge_match(app->agent.tools.bridge_token, req ? json_get_str(req, "token") : NULL)) {
            buf_puts(&resp, "{\"ok\":false,\"error\":\"unauthorized\"}\n");
        } else if (!text) {
            buf_puts(&resp, "{\"ok\":false,\"error\":\"expected {\\\"text\\\":...}\"}\n");
        } else if (from_value && from_value->type != J_STR && from_value->type != J_NULL) {
            buf_puts(&resp, "{\"ok\":false,\"error\":\"invalid from\"}\n");
        } else {
            Buf reply;
            buf_init(&reply);
            ServeSession session; int status = serve_session(app, from, &session);
            if (status < 0) {
                buf_puts(&resp, status == -2 ? "{\"ok\":false,\"error\":\"invalid from\"}\n" :
                         status == -4 ? "{\"ok\":false,\"error\":\"session busy\"}\n" :
                                        "{\"ok\":false,\"error\":\"session storage unavailable\"}\n");
            } else {
                int rc = session.agent.persistence_error ? -1 : agent_prompt(&session.agent, text, &reply);
                if (session.agent.persistence_error || (rc == 0 &&
                    (fsync(session.tempfd) != 0 || rename(session.temp, session.path) != 0))) {
                    buf_puts(&resp, "{\"ok\":false,\"error\":\"session storage unavailable\"}\n");
                } else {
                    buf_puts(&resp, rc == 0 ? "{\"ok\":true,\"reply\":" : "{\"ok\":false,\"reply\":");
                    json_quote(&resp, reply.s); buf_puts(&resp, "}\n");
                }
                agent_free(&session.agent); serve_session_close(&session);
            }
            buf_free(&reply);
        }
        if (conn) bridge_tls_write(conn, resp.s, resp.len, 5);
        else { size_t off = 0; while (off < resp.len) { ssize_t n = send(c, resp.s + off, resp.len - off, 0); if (n < 0 && errno == EINTR) continue; if (n <= 0) break; off += (size_t)n; } }
        buf_free(&resp);
        json_free(req);
        bridge_tls_free(conn);
        close(c);
        buf_free(&line);
    }

static int cmd_serve(App *app, int port) {
    if (!bridge_valid(app->agent.tools.bridge_token)) {
        fprintf(stderr, "[serve] set bridge_token (64 hex chars; openssl rand -hex 32)\n");
        return 1;
    }
    if (app->serve_workers < 1 || app->serve_workers > 256) {
        fprintf(stderr, "[serve] serve_workers must be between 1 and 256\n"); return 1;
    }
    BridgeTlsCtx *tlsctx = NULL;
    if (app->serve_tls) {
        tlsctx = bridge_tls_server_new(app->serve_cert, app->serve_key);
        if (!tlsctx) {
            fprintf(stderr, "[serve] TLS unavailable or invalid serve_cert/serve_key\n");
            return 1;
        }
    }
    const char *node = (app->serve_bind && app->serve_bind[0]) ? app->serve_bind
                       : "127.0.0.1";   /* loopback unless -b/serve_bind opts in */
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(node, portstr, &hints, &res) != 0) {
        fprintf(stderr, "clawc: getaddrinfo failed for %s\n", node);
        return 1;
    }
    int srv = -1, yes = 1;
    struct addrinfo *ai;
    for (ai = res; ai; ai = ai->ai_next) {
        if (!app->serve_tls && !bridge_loopback(ai->ai_addr)) continue;
        srv = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (srv < 0) continue;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        if (bind(srv, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(srv);
        srv = -1;
    }
    freeaddrinfo(res);
    if (srv < 0 || listen(srv, 64) != 0) {
        fprintf(stderr, "clawc: cannot listen on %d (plaintext is loopback-only)\n", port);
        return 1;
    }
    fprintf(stderr, "[serve] listening on %s:%d (one JSON object per line)\n", node, port);
    fprintf(stderr, "[serve] %s; tools auto-approved\n", app->serve_tls ? "TLS" : "plaintext on loopback");
    int flags = fcntl(srv, F_GETFL, 0);
    if (flags < 0 || fcntl(srv, F_SETFL, flags | O_NONBLOCK) < 0) { close(srv); bridge_tls_server_free(tlsctx); return 1; }
    pid_t *workers = xmalloc((size_t)app->serve_workers * sizeof *workers);
    memset(workers, 0, (size_t)app->serve_workers * sizeof *workers);
    serve_stop = 0;
    struct sigaction sa, oldterm, oldint;
    memset(&sa, 0, sizeof sa); sa.sa_handler = serve_stop_handler; sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, &oldterm); sigaction(SIGINT, &sa, &oldint);
    signal(SIGPIPE, SIG_IGN);
    fprintf(stderr, "[serve] bounded concurrency: %d workers\n", app->serve_workers);
    while (!serve_stop) {
        int i, slot = -1;
        for (i = 0; i < app->serve_workers; i++) {
            if (workers[i] && waitpid(workers[i], NULL, WNOHANG) == workers[i]) workers[i] = 0;
            if (!workers[i] && slot < 0) slot = i;
        }
        struct pollfd pfd = { slot >= 0 ? srv : -1, POLLIN, 0 };
        int ready = poll(&pfd, 1, 250);
        if (serve_stop) break;
        if (ready <= 0 || slot < 0) continue;
        int c = accept(srv, NULL, NULL);
        if (c < 0) continue;
        int cflags = fcntl(c, F_GETFL, 0);
        struct timeval tv = {5, 0};
        if (cflags < 0 || fcntl(c, F_SETFL, cflags & ~O_NONBLOCK) < 0 ||
            setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) < 0) { close(c); continue; }
        pid_t pid = fork();
        if (pid < 0) { fprintf(stderr, "[serve] worker fork failed\n"); close(c); continue; }
        if (!pid) {
            setpgid(0, 0); signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); close(srv);
            serve_client(app, tlsctx, c);
            _exit(0);
        }
        setpgid(pid, pid);
        workers[slot] = pid; close(c);
    }
    close(srv);
    int i;
    for (i = 0; i < app->serve_workers; i++) if (workers[i]) { kill(-workers[i], SIGTERM); kill(workers[i], SIGTERM); }
    for (i = 0; i < app->serve_workers; i++) if (workers[i]) while (waitpid(workers[i], NULL, 0) < 0 && errno == EINTR) {}
    free(workers); bridge_tls_server_free(tlsctx);
    sigaction(SIGTERM, &oldterm, NULL); sigaction(SIGINT, &oldint, NULL);
    return 0;
}

#endif /* serve */

/* ---- gateway entry points for the daemon supervisor ---- */

#ifndef CLAWC_NO_TELEGRAM
static int gw_telegram(void *ctx) {
    App *app = ctx;
    return telegram_run(&app->agent, &app->tg);
}

#endif /* telegram */
#ifndef CLAWC_NO_CRON
static void cron_fire(App *app, CronJob *j, FILE *out);

static int gw_cron(void *ctx) {
    App *app = ctx;
    for (;;) {
        CronJob *jobs = NULL;
        int n = 0, i, fired = 0;
        long long now = (long long)time(NULL);
        if (cron_load(app->agent.tools.workspace, &jobs, &n) == 0) {
            for (i = 0; i < n; i++) {
                if (!cron_due(&jobs[i], now)) continue;
                cron_fire(app, &jobs[i], NULL);
                cron_advance(&jobs[i], now);
                fired++;
            }
            if (fired) cron_save(app->agent.tools.workspace, jobs, n);
        }
        cron_free(jobs, n);
        sleep(15);
    }
    return 0;
}

static void cron_fire(App *app, CronJob *j, FILE *out) {
    Agent sub;
    agent_init(&sub);
    sub.llm = app->agent.llm;       /* shared, immutable config strings */
    sub.tools = app->agent.tools;
    sub.max_steps = app->agent.max_steps;
    sub.heartbeat_mode = 0;
    sub.confirm = NULL;             /* unattended: risky tools are refused */
    sub.auto_approve = 0;
    char sname[64];
    snprintf(sname, sizeof sname, "cron:%.40s", j->id);
    sub.session_name = xstrdup(sname); /* agent_free owns this */
    Buf reply;
    buf_init(&reply);
    agent_prompt(&sub, j->task, &reply);
    if (out) fprintf(out, "[%s] %s\n", j->id, reply.s ? reply.s : "(no output)");
    if (!app->quiet) fprintf(stderr, "[cron %s] done\n", j->id);
    app->hb_forward_tg = (app->tg.token && app->tg.allow && app->tg.nallow > 0);
    Buf msg;
    buf_init(&msg);
    buf_printf(&msg, "[cron %.40s] %.1800s", j->id, reply.s ? reply.s : "(no output)");
    notify_send(app, msg.s);
    buf_free(&msg);
    buf_free(&reply);
    agent_free(&sub);
}

#endif /* cron */
#ifndef CLAWC_NO_TELEGRAM
static int tg_getme(const char *api_base, const char *token, int insecure, Buf *user);
#endif

static int cmd_autonomous(App *app) {
    const char *PROMPT = packed_text(TXT_EXTRA_05);
    Agent a = app->agent;
    a.heartbeat_mode = 0;
    a.confirm = NULL;        /* unattended: risky tools are refused */
    a.auto_approve = 0;
    char sname[32];
    snprintf(sname, sizeof sname, "autonomous");
    a.session_name = sname;
    Buf reply;
    buf_init(&reply);
    int rc = agent_prompt(&a, PROMPT, &reply);
    const char *msg = reply.s ? reply.s : "(no output)";
    if (!str_eq(msg, "NO_GOALS")) {
        printf("%s\n", msg);
        Buf note;
        buf_init(&note);
        buf_printf(&note, "[autonomous] %.1500s", msg);
        notify_send(app, note.s);
        buf_free(&note);
    } else if (!app->quiet) {
        fprintf(stderr, "[autonomous] no open goals\n");
    }
    buf_free(&reply);
    return rc;
}

#ifndef CLAWC_NO_MATRIX
static int cmd_matrix(App *app) {
    return matrix_run(&app->agent, &app->mx);
}

static int gw_matrix(void *ctx) {
    App *app = ctx;
    return matrix_run(&app->agent, &app->mx);
}
#endif

#ifndef CLAWC_NO_DISCORD
static int cmd_discord(App *app) {
    return discord_run(&app->agent, &app->dc);
}

static int gw_discord(void *ctx) {
    App *app = ctx;
    return discord_run(&app->agent, &app->dc);
}
#endif

static int cmd_improve(App *app) {
    const char *PROMPT = packed_text(TXT_EXTRA_06);
    Agent a = app->agent;
    a.heartbeat_mode = 0;
    a.confirm = NULL;
    a.auto_approve = 0;
    char sname[32];
    snprintf(sname, sizeof sname, "improve");
    a.session_name = sname;
    Buf reply;
    buf_init(&reply);
    int rc = agent_prompt(&a, PROMPT, &reply);
    printf("%s\n", reply.s ? reply.s : "(no output)");
    buf_free(&reply);
    return rc;
}

static int cmd_doctor(App *app) {
    int fails = 0;
    printf("clawc %s doctor\n", CLAWC_VERSION);
    printf("  [features]  ");
#ifndef CLAWC_NO_TELEGRAM
    printf("telegram ");
#endif
#ifndef CLAWC_NO_SERVE
    printf("serve ");
#endif
#ifndef CLAWC_NO_CRON
    printf("cron ");
#endif
#ifndef CLAWC_NO_HEARTBEAT
    printf("heartbeat ");
#endif
    printf("\n");

    /* config */
    const char *ws = app->agent.tools.workspace;
    printf("  [config]    %s  ws %s\n",
           app->agent.llm.api_url ? "ok" : "warn",
           ws ? ws : "(unset)");

    /* workspace */
    if (ws && ws[0] && access(ws, W_OK) == 0) {
        printf("  [workspace] ok  %s\n", ws);
    } else {
        printf("  [workspace] FAIL %s\n", ws ? ws : "(unset)");
        fails++;
    }

    /* LLM: one real 1-token round trip */
    if (app->agent.llm.api_url) {
        Msg in, out;
        memset(&in, 0, sizeof in);
        memset(&out, 0, sizeof out);
        in.role = (char *)"user";
        in.content = (char *)"reply with the single word: pong";
        char *err = NULL;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int rc = llm_chat(&app->agent.llm, &in, 1, &out, &err, NULL, NULL);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (rc == 0) {
            printf("  [llm]       ok  %s %s (%ldms)\n",
                   app->agent.llm.api_url,
                   app->agent.llm.model ? app->agent.llm.model : "(no model)", ms);
        } else {
            {
                char one[120];
                snprintf(one, sizeof one, "%s", err ? err : "request failed");
                char *nl = strchr(one, '\n');
                if (nl) *nl = 0;
                printf("  [llm]       FAIL %s: %s\n", app->agent.llm.api_url, one);
            }
            fails++;
        }
        free(err);
        msg_clear(&out);
    } else {
        printf("  [llm]       FAIL no api_url\n");
        fails++;
    }

    /* telegram */
#ifndef CLAWC_NO_TELEGRAM
    if (app->tg.token && app->tg.token[0]) {
        Buf user;
        buf_init(&user);
        const char *base = (app->tg.api_base && app->tg.api_base[0])
                           ? app->tg.api_base : "https://api.telegram.org";
        if (tg_getme(base, app->tg.token, app->tg.insecure, &user) == 0)
            printf("  [telegram]  ok  %s\n", user.s ? user.s : "(getMe ok)");
        else {
            printf("  [telegram]  FAIL token rejected\n");
            fails++;
        }
        buf_free(&user);
    } else {
        printf("  [telegram]  --  not configured\n");
    }
#else
    printf("  [telegram]  --  not compiled in\n");
#endif

#ifndef CLAWC_NO_MATRIX
    if (app->mx.homeserver && app->mx.homeserver[0] &&
        app->mx.token && app->mx.token[0]) {
        Buf url;
        buf_init(&url);
        buf_printf(&url, "%s/_matrix/client/v3/account/whoami", app->mx.homeserver);
        const char *hdrs[2];
        char auth[400];
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", app->mx.token);
        hdrs[0] = auth;
        hdrs[1] = NULL;
        HttpResp *r = http_request("GET", url.s, hdrs, 1, NULL, 15,
                                   app->agent.tools.insecure_tls);
        if (r->status == 200) {
            JVal *root = json_parse(r->body.s);
            const char *uid = root ? json_get_str(root, "user_id") : NULL;
            printf("  [matrix]     ok  %s\n", uid ? uid : "(whoami ok)");
            json_free(root);
        } else {
            printf("  [matrix]     FAIL whoami HTTP %ld\n", r->status);
            fails++;
        }
        http_resp_free(r);
        buf_free(&url);
    } else {
        printf("  [matrix]     --  not configured\n");
    }
#endif

#ifndef CLAWC_NO_DISCORD
    if (app->dc.token && app->dc.token[0]) {
        printf("  [discord]    ok  token (guilds: %s)\n",
               app->dc.guilds && app->dc.guilds[0] ? app->dc.guilds : "all");
    } else {
        printf("  [discord]    --  not configured\n");
    }
#endif

    /* sandbox */
    if (sandbox_available())
        printf("  [sandbox]   ok  %s (set \"sandbox\": \"os\")\n",
               sandbox_backend());
    else
        fputs(packed_text(TXT_STDOUT_01), stdout);

    /* cron */
#ifndef CLAWC_NO_CRON
    {
        CronJob *jobs = NULL;
        int n = 0;
        if (ws && cron_load(ws, &jobs, &n) == 0) {
            printf("  [cron]      %s %d jobs\n", n ? "ok" : "--", n);
            cron_free(jobs, n);
        } else {
            printf("  [cron]      FAIL cron.json malformed\n");
            fails++;
        }
    }
#else
    printf("  [cron]      --  not compiled in\n");
#endif

    /* gateways */
    printf("  [gateways]  ");
#ifndef CLAWC_NO_TELEGRAM
    if (app->tg.token && app->tg.token[0]) printf("telegram ");
#endif
#ifndef CLAWC_NO_SERVE
    if (app->serve_port > 0) printf("serve:%d %s ", app->serve_port,
                                    (app->serve_bind && app->serve_bind[0]) ? app->serve_bind : "127.0.0.1");
#endif
#ifndef CLAWC_NO_HEARTBEAT
    if (app->hb_interval > 0) printf("heartbeat:%ds ", app->hb_interval);
#endif
#ifndef CLAWC_NO_CRON
    if (ws && cron_active(ws)) printf("cron ");
#endif
    printf("\n");

    printf(fails ? "doctor: %d problem(s) found\n" : "doctor: all good.\n", fails);
    return fails ? 1 : 0;
}

#ifndef CLAWC_NO_CRON
static int cmd_cron(App *app, int argc, char **argv) {
    const char *ws = app->agent.tools.workspace;
    CronJob *jobs = NULL;
    int n = 0, i, rc = 0;
    if (cron_load(ws, &jobs, &n) != 0) {
        fprintf(stderr, "clawc: %s/cron.json: bad schedule file\n", ws);
        return 2;
    }
    const char *sub = (argc > 0) ? argv[0] : "list";
    if (str_eq(sub, "list")) {
        long long now = (long long)time(NULL);
        for (i = 0; i < n; i++) {
            char when[32];
            struct tm t;
            time_t jt = (time_t)jobs[i].at;
            if (jobs[i].at > 0 && localtime_r(&jt, &t))
                stamp_fmt(when, sizeof when, "%m-%d %H:%M", &t);
            else
                snprintf(when, sizeof when, "?");
            printf("%s  %-8s  %s (in %llds)  %s\n", jobs[i].id,
                   jobs[i].enabled ? (jobs[i].every > 0 ? "recurring" : "one-shot") : "spent",
                   when, jobs[i].at > now ? jobs[i].at - now : 0LL, jobs[i].task);
        }
        if (n == 0) fputs(packed_text(TXT_STDOUT_02), stdout);
    } else if (str_eq(sub, "add")) {
        long long at = 0, every = 0;
        int bad = 0;
        Buf task;
        buf_init(&task);
        for (i = 1; i < argc; i++) {
            if (str_eq(argv[i], "--in") && i + 1 < argc) at = (long long)time(NULL) + atoll(argv[++i]);
            else if (str_eq(argv[i], "--at") && i + 1 < argc) {
                const char *v = argv[++i];
                if (strchr(v, ':')) { at = cron_next_clock(v, -1); bad = (at < 0); }
                else at = atoll(v);
            }
            else if (str_eq(argv[i], "--every") && i + 1 < argc) every = atoll(argv[++i]);
            else if (str_eq(argv[i], "--daily") && i + 1 < argc) {
                at = cron_next_clock(argv[++i], -1);
                every = 86400;
                bad = (at < 0);
            }
            else if (str_eq(argv[i], "--weekly") && i + 2 < argc) {
                int wd = cron_weekday(argv[++i]);
                at = cron_next_clock(argv[++i], wd);
                every = 604800;
                bad = (at < 0 || wd < 0);
            }
            else {
                if (task.len) buf_puts(&task, " ");
                buf_puts(&task, argv[i]);
            }
        }
        if (bad) {
            fputs(packed_text(TXT_EXTRA_08), stderr);
            rc = 2;
        } else if (task.len == 0) {
            fprintf(stderr, "clawc: cron add needs a task\n");
            rc = 2;
        } else {
            if (at <= 0 && every > 0) at = time(NULL) + every;
            else if (at <= 0) at = time(NULL);
            long maxid = 0;
            for (i = 0; i < n; i++)
                if (jobs[i].id[0] == 'j' && atol(jobs[i].id + 1) > maxid)
                    maxid = atol(jobs[i].id + 1);
            jobs = xrealloc(jobs, (size_t)(n + 1) * sizeof *jobs);
            snprintf(jobs[n].id, sizeof jobs[n].id, "j%ld", maxid + 1);
            jobs[n].task = xstrdup(task.s);
            jobs[n].at = at;
            jobs[n].every = every;
            jobs[n].enabled = 1;
            n++;
            if (cron_save(ws, jobs, n) != 0) {
                fprintf(stderr, "clawc: cannot write %s/cron.json\n", ws);
                rc = 1;
            } else {
                printf("added %s (due %lld%s)\n", jobs[n - 1].id, at,
                       every > 0 ? ", recurring" : "");
            }
        }
        buf_free(&task);
    } else if (str_eq(sub, "rm") && argc > 1) {
        int w = 0;
        for (i = 0; i < n; i++) {
            if (str_eq(jobs[i].id, argv[1])) { free(jobs[i].task); continue; }
            jobs[w++] = jobs[i];
        }
        if (w == n) {
            fprintf(stderr, "clawc: no cron job '%s'\n", argv[1]);
            rc = 2;
        } else {
            n = w;
            cron_save(ws, jobs, n);
            printf("removed %s\n", argv[1]);
        }
    } else if (str_eq(sub, "--run-due")) {
        long long now = (long long)time(NULL);
        int fired = 0;
        for (i = 0; i < n; i++) {
            if (!cron_due(&jobs[i], now)) continue;
            cron_fire(app, &jobs[i], stdout);
            cron_advance(&jobs[i], now);
            fired++;
        }
        if (fired) cron_save(ws, jobs, n);
        else if (!app->quiet) fprintf(stderr, "[cron] nothing due\n");
    } else {
        fputs(packed_text(TXT_EXTRA_09), stderr);
        rc = 2;
    }
    cron_free(jobs, n);
    return rc;
}

#endif /* cron */
#ifndef CLAWC_NO_SERVE
static int gw_serve(void *ctx) {
    App *app = ctx;
    return cmd_serve(app, app->serve_port);
}

#endif /* serve */
#ifndef CLAWC_NO_HEARTBEAT
static int gw_heartbeat(void *ctx) {
    App *app = ctx;
    for (;;) {
        cmd_heartbeat(app);
        fprintf(stderr, "[daemon] heartbeat sleeping %ds\n", app->hb_interval);
        sleep((unsigned)(app->hb_interval > 0 ? app->hb_interval : 1800));
    }
    return 0;
}

#endif /* heartbeat */

static int gw_autonomous(void *ctx) {
    App *app = ctx;
    for (;;) {
        cmd_autonomous(app);
        fprintf(stderr, "[daemon] autonomous sleeping %ds\n", app->au_interval);
        sleep((unsigned)(app->au_interval > 0 ? app->au_interval : 1800));
    }
    return 0;
}

static int cmd_daemon(App *app) {
    Gw gws[6];
    int n = 0;
    int tg_on = 0;
#ifndef CLAWC_NO_TELEGRAM
    tg_on = app->tg.token && app->tg.token[0];
    if (tg_on && !sender_ready("telegram", app->tg.users)) return 2;
    if (tg_on) {
        gws[n].name = "telegram";
        gws[n].fn = gw_telegram;
        gws[n].ctx = app;
        n++;
    }
#endif
#ifndef CLAWC_NO_SERVE
    if (app->serve_port > 0) {
        gws[n].name = "serve";
        gws[n].fn = gw_serve;
        gws[n].ctx = app;
        n++;
    }
#endif
#ifndef CLAWC_NO_HEARTBEAT
    if (app->hb_interval > 0) {
        app->hb_forward_tg = tg_on && app->tg.nallow > 0;
        gws[n].name = "heartbeat";
        gws[n].fn = gw_heartbeat;
        gws[n].ctx = app;
        n++;
    }
#endif
#ifndef CLAWC_NO_MATRIX
    if (app->mx.homeserver && app->mx.homeserver[0] &&
        app->mx.token && app->mx.token[0]) {
        if (!sender_ready("matrix", app->mx.users)) return 2;
        app->mx.once = 0;
        if (!app->hb_forward_tg)
            app->hb_forward_tg = (app->tg.token && app->tg.nallow > 0);
        gws[n].name = "matrix";
        gws[n].fn = gw_matrix;
        gws[n].ctx = app;
        n++;
    }
#endif
#ifndef CLAWC_NO_DISCORD
    if (app->dc.token && app->dc.token[0]) {
        if (!sender_ready("discord", app->dc.users)) return 2;
        app->dc.once = 0;
        if (!app->hb_forward_tg)
            app->hb_forward_tg = (app->tg.token && app->tg.nallow > 0);
        gws[n].name = "discord";
        gws[n].fn = gw_discord;
        gws[n].ctx = app;
        n++;
    }
#endif
    if (app->au_interval > 0) {
        if (!app->hb_forward_tg)
            app->hb_forward_tg = (app->tg.token && app->tg.nallow > 0);
        gws[n].name = "autonomous";
        gws[n].fn = gw_autonomous;
        gws[n].ctx = app;
        n++;
    }
#ifndef CLAWC_NO_CRON
    if (cron_active(app->agent.tools.workspace)) {
#ifndef CLAWC_NO_TELEGRAM
        if (!app->hb_forward_tg)
            app->hb_forward_tg = (app->tg.token && app->tg.nallow > 0);
#endif
        gws[n].name = "cron";
        gws[n].fn = gw_cron;
        gws[n].ctx = app;
        n++;
    }
#endif
    if (n == 0) {
        fputs(packed_text(TXT_EXTRA_10), stderr);
        return 2;
    }
    return daemon_run(gws, n);
}

static int cmd_config(const Cfg *c) {
    cfg_dump(c, stdout);
    return 0;
}

/* ---- onboarding wizard ---- */

static void ask_line(const char *label, const char *dflt, Buf *out) {
    if (dflt && dflt[0]) fprintf(stderr, "%s [%s]: ", label, dflt);
    else fprintf(stderr, "%s: ", label);
    fflush(stderr);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, stdin);
    if (n > 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        str_trim(line);
    }
    if (n > 0 && line[0]) buf_puts(out, line);
    else buf_puts(out, dflt ? dflt : "");
    free(line);
}

static int tg_getme(const char *api_base, const char *token, int insecure, Buf *user) {
    Buf url;
    buf_init(&url);
    buf_printf(&url, "%s/bot%s/getMe", api_base, token);
    HttpResp *r = http_request("GET", url.s, NULL, 0, NULL, 30, insecure);
    buf_free(&url);
    int rc = -1;
    if (r->status == 200) {
        JVal *root = json_parse(r->body.s);
        JVal *res = root ? json_obj_get(root, "result") : NULL;
        const char *u = res ? json_get_str(res, "username") : NULL;
        if (u) { buf_puts(user, u); rc = 0; }
        json_free(root);
    } else if (r->status == 401 || r->status == 403 || r->status == 404) {
        rc = 1; /* token rejected */
    }
    if (rc != 0)
        fprintf(stderr, "clawc: getMe failed (HTTP %ld): %s\n", r->status,
                r->err ? r->err : r->body.s ? r->body.s : "");
    http_resp_free(r);
    return rc;
}

/* mkdir -p the directory containing `path`; 0 on success (errno set on failure). */
static int ensure_parent_dir(const char *path) {
    char dir[4200];
    snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/');
    if (!sl) return 0;
    *sl = '\0';
    if (!dir[0]) return 0;
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(dir, 0755) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(dir, 0755) < 0 && errno != EEXIST) return -1;
    return 0;
}

/* The LLM endpoint is always <base>/v1 - accept "host:port" and fix it up. */
static void normalize_api_url(Buf *u) {
    while (u->len > 0 && u->s[u->len - 1] == '/') u->s[--u->len] = '\0';
    if (u->len == 0) return;
    if (u->len < 3 || strcmp(u->s + u->len - 3, "/v1") != 0) {
        buf_puts(u, "/v1");
        printf("API base url normalized to %s\n", u->s);
    }
}

static int cmd_setup(const char *path, int insecure) {
    Buf v, ws, url, model, key, tok, allow, users, port, hb, yes;
    buf_init(&v); buf_init(&ws); buf_init(&url); buf_init(&model); buf_init(&key);
    buf_init(&tok); buf_init(&allow); buf_init(&users); buf_init(&port); buf_init(&hb); buf_init(&yes);

    printf("clawc setup - Enter keeps default\n\n");
    ask_line("workspace", getenv("CLAWC_WORKSPACE"), &ws);
    ask_line("API base url (/v1)", getenv("CLAWC_API_URL"), &url);
    normalize_api_url(&url);
    ask_line("model", getenv("CLAWC_MODEL"), &model);
    ask_line("API key", getenv("CLAWC_API_KEY"), &key);
#ifndef CLAWC_NO_TELEGRAM
    ask_line("Telegram bot token (optional)", getenv("CLAWC_TELEGRAM_TOKEN"), &tok);
    ask_line("Telegram chat ids (empty = private)",
             getenv("CLAWC_TELEGRAM_ALLOW"), &allow);
    if (tok.len) ask_line("Telegram user ids (required)", getenv("CLAWC_TELEGRAM_USERS"), &users);
    if (tok.len && !users.len) fprintf(stderr, "[setup] warning: telegram_users is required to start Telegram\n");
#endif
    ask_line("TCP serve port (0 = off)", "0", &port);
#ifndef CLAWC_NO_SERVE
    if (atoi(port.s) > 0)
        fprintf(stderr, "[setup] serve requires bridge_token or CLAWC_BRIDGE_TOKEN (openssl rand -hex 32)\n");
#endif
    ask_line("heartbeat interval seconds", "0", &hb);
    ask_line("auto-approve risky actions?", "no", &yes);

    if (tok.len > 0) {
        const char *api = getenv("CLAWC_TELEGRAM_API");
        Buf user;
        buf_init(&user);
        int gr = tg_getme(api && api[0] ? api : "https://api.telegram.org",
                          tok.s, insecure, &user);
        if (gr == 1) {
            fprintf(stderr, "clawc: token rejected - not writing %s\n", path);
            return 1;
        }
        if (gr == 0) printf("verified: @%s (live)\n", user.s);
        else printf("warning: could not verify token now - saving anyway\n");
        buf_free(&user);
    }

    /* workspace scaffold */
    cmd_init(ws.s);

    /* write the config file (keep a backup of any previous one) */
    {
        char *hd = path_join(ws.s, "history");
        mkdir(hd, 0755);
        free(hd);
    }
    if (ensure_parent_dir(path) != 0) {
        fprintf(stderr, "clawc: cannot create dir for %s: %s\n", path, err_str(errno));
        fprintf(stderr, "clawc: pick a writable path\n");
        return 1;
    }
    if (file_exists(path)) {
        char bak[4200];
        snprintf(bak, sizeof bak, "%s.bak", path);
        rename(path, bak);
        printf("previous config moved to %s\n", bak);
    }
    Buf f;
    buf_init(&f);
    json_append_quoted(&f, packed_text(TXT_012), ws.s);
    json_append_quoted(&f, packed_text(TXT_013), url.s);
    json_append_quoted(&f, ",\n  \"model\": ", model.s);
    if (key.len > 0) { json_append_quoted(&f, packed_text(TXT_014), key.s); }
    if (tok.len > 0) { json_append_quoted(&f, packed_text(TXT_015), tok.s); }
    if (allow.len > 0) { json_append_quoted(&f, packed_text(TXT_016), allow.s); }
    if (users.len > 0) { json_append_quoted(&f, packed_text(TXT_017), users.s); }
    if (atoi(port.s) > 0) buf_printf(&f, ",\n  \"serve_port\": %d", atoi(port.s));
    if (atoi(hb.s) > 0) buf_printf(&f, ",\n  \"heartbeat_interval\": %d", atoi(hb.s));
    if (str_starts(yes.s, "y") || str_starts(yes.s, "Y") ||
        str_eq(yes.s, "on") || str_eq(yes.s, "true"))
        buf_puts(&f, packed_text(TXT_018));
    buf_puts(&f, "\n}\n");
    if (write_file(path, f.s) != 0) {
        fprintf(stderr, "clawc: cannot write %s: %s\n", path, err_str(errno));
        fprintf(stderr, "clawc: pick a writable path\n");
        return 1;
    }
    chmod(path, 0600);
    buf_free(&f);

    /* summary from the same machinery `clawc config` uses */
    {
        Cfg c;
        cfg_init(&c);
        c.path = xstrdup(path);
        cfg_set_str(&c, CK_WORKSPACE, xstrdup(ws.s), "setup");
        cfg_set_str(&c, CK_API_URL, xstrdup(url.s), "setup");
        cfg_set_str(&c, CK_MODEL, xstrdup(model.s), "setup");
        if (key.len > 0) cfg_set_str(&c, CK_API_KEY, xstrdup(key.s), "setup");
        if (tok.len > 0) cfg_set_str(&c, CK_TG_TOKEN, xstrdup(tok.s), "setup");
        if (allow.len > 0) cfg_set_str(&c, CK_TG_ALLOW, xstrdup(allow.s), "setup");
        if (users.len > 0) cfg_set_str(&c, CK_TG_USERS, xstrdup(users.s), "setup");
        if (atoi(port.s) > 0) cfg_set_int(&c, CK_SERVE_PORT, atoi(port.s), "setup");
        if (atoi(hb.s) > 0) cfg_set_int(&c, CK_HB_INTERVAL, atoi(hb.s), "setup");
        if (str_starts(yes.s, "y") || str_starts(yes.s, "Y") ||
            str_eq(yes.s, "on") || str_eq(yes.s, "true"))
            cfg_set_bool(&c, CK_AUTO_APPROVE, 1, "setup");
        printf("\nwrote %s (mode 600)\n\n", path);
        cfg_dump(&c, stdout);
    }
    fputs(packed_text(TXT_STDOUT_03), stdout);
    buf_free(&v); buf_free(&ws); buf_free(&url); buf_free(&model); buf_free(&key);
    buf_free(&tok); buf_free(&allow); buf_free(&users); buf_free(&port); buf_free(&hb); buf_free(&yes);
    return 0;
}

/* ---- CLI parsing + config resolution ---- */

#define F_WS     (1u << 0)
#define F_MODEL  (1u << 1)
#define F_URL    (1u << 2)
#define F_KEY    (1u << 3)
#define F_KIND   (1u << 4)
#define F_MAXST  (1u << 5)
#define F_TOUT   (1u << 6)
#define F_SB     (1u << 7)
#define F_INSEC  (1u << 8)
#define F_YES    (1u << 9)
#define F_QUIET  (1u << 10)
#define F_TGTOK  (1u << 11)
#define F_TGALW  (1u << 12)
#define F_TGPOLL (1u << 13)
#define F_SERVE  (1u << 14)
#define F_HB     (1u << 15)
#define F_ONCE   (1u << 16)
#define F_BIND   (1u << 17)

typedef struct {
    char *ws, *model, *url, *key, *kind, *sandbox, *cfg_path;
    char *tg_token, *tg_allow, *serve_bind;
    long max_steps, timeout, tg_poll, serve_port, hb_interval;
    unsigned set;
} Cli;

static void cli_free(Cli *v) {
    char **s[] = { &v->ws, &v->model, &v->url, &v->key, &v->kind, &v->sandbox,
                   &v->cfg_path, &v->tg_token, &v->tg_allow, &v->serve_bind };
    size_t i;
    for (i = 0; i < sizeof s / sizeof s[0]; i++) {
        free(*s[i]);
        *s[i] = NULL;
    }
}

/* Flag-to-field map keeps CLI overrides in one place. Offsets are derived
   by the compiler, so the layout works on 32- and 64-bit POSIX systems. */
struct CliField { unsigned short offset; unsigned char key, opt; unsigned flag; };
#define CLI_FIELD(field, flag, key, opt) { offsetof(Cli, field), key, opt, flag }
static const struct CliField cli_strings[] = {
    CLI_FIELD(ws,F_WS,CK_WORKSPACE,'w'), CLI_FIELD(model,F_MODEL,CK_MODEL,'m'),
    CLI_FIELD(url,F_URL,CK_API_URL,'u'), CLI_FIELD(key,F_KEY,CK_API_KEY,'k'),
    CLI_FIELD(kind,F_KIND,CK_API_KIND,'a'), CLI_FIELD(sandbox,F_SB,CK_SANDBOX,'x'),
    CLI_FIELD(tg_token,F_TGTOK,CK_TG_TOKEN,0), CLI_FIELD(tg_allow,F_TGALW,CK_TG_ALLOW,0),
    CLI_FIELD(serve_bind,F_BIND,CK_SERVE_BIND,'b')
};
static const struct CliField cli_numbers[] = {
    CLI_FIELD(max_steps,F_MAXST,CK_MAX_STEPS,'s'), CLI_FIELD(timeout,F_TOUT,CK_TIMEOUT,'t'),
    CLI_FIELD(tg_poll,F_TGPOLL,CK_TG_POLL,0), CLI_FIELD(serve_port,F_SERVE,CK_SERVE_PORT,'p'),
    CLI_FIELD(hb_interval,F_HB,CK_HB_INTERVAL,'i')
};
#undef CLI_FIELD
static int cli_short_value(Cli *cli, char opt, const char *arg, unsigned allowed) {
    size_t j;
    for (j=0; j<sizeof cli_strings/sizeof *cli_strings; j++) {
        const struct CliField *f=&cli_strings[j];
        if (opt == f->opt && (f->flag & allowed)) {
            char **p=(char **)((char *)cli + f->offset);
            *p=xstrdup(arg); cli->set |= f->flag; return 1;
        }
    }
    for (j=0; j<sizeof cli_numbers/sizeof *cli_numbers; j++) {
        const struct CliField *f=&cli_numbers[j];
        if (opt == f->opt && (f->flag & allowed)) {
            long *p=(long *)((char *)cli + f->offset);
            *p=atol(arg); cli->set |= f->flag; return 1;
        }
    }
    return 0;
}
static void apply_cli(Cfg *c, const Cli *v) {
    size_t i;
    for (i=0; i<sizeof cli_strings/sizeof *cli_strings; i++) {
        const struct CliField *f=&cli_strings[i];
        if (v->set & f->flag) {
            char *const *p=(char *const *)((const char *)v + f->offset);
            cfg_set_str(c, f->key, xstrdup(*p), "flag");
        }
    }
    for (i=0; i<sizeof cli_numbers/sizeof *cli_numbers; i++) {
        const struct CliField *f=&cli_numbers[i];
        if (v->set & f->flag) {
            const long *p=(const long *)((const char *)v + f->offset);
            cfg_set_int(c, f->key, *p, "flag");
        }
    }
    if (v->set & F_INSEC) cfg_set_bool(c, CK_INSECURE, 1, "flag");
    if (v->set & F_YES) cfg_set_bool(c, CK_AUTO_APPROVE, 1, "flag");
}

static void tg_parse_allow(TgCfg *tg, const char *allow_str) {
    if (!allow_str || !allow_str[0]) return;
    long ids[128];
    int n = 0;
    const char *p = allow_str;
    while (*p && n < 128) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        ids[n++] = v;
        p = end;
        if (*p == ',') p++;
    }
    tg->allow = xmalloc((size_t)n * sizeof(long) + 1);
    memcpy(tg->allow, ids, (size_t)n * sizeof(long));
    tg->nallow = n;
}

int main(int argc, char **argv) {
    App app;
    memset(&app, 0, sizeof app);
    agent_init(&app.agent);
    app.serve_port = -1;
    app.hb_interval = -1;
    app.au_interval = -1;

    Cli cli;
    memset(&cli, 0, sizeof cli);

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-') break;
        if (a[1] && !a[2]) {
            if (a[1] == 'q') { app.quiet = 1; cli.set |= F_QUIET; continue; }
            if (a[1] == 'h') { usage(stdout); return 0; }
            if (i + 1 < argc) {
                if (a[1] == 'c') { cli.cfg_path = xstrdup(argv[++i]); continue; }
                if (cli_short_value(&cli, a[1], argv[i+1],
                                    F_WS|F_MODEL|F_URL|F_KEY|F_KIND|F_SB|F_MAXST|F_TOUT)) {
                    i++; continue;
                }
            }
        }
        if (str_eq(a, "--yes")) { cli.set |= F_YES; continue; }
        if (str_eq(a, "--insecure")) { cli.set |= F_INSEC; continue; }
        if (str_eq(a, "--help")) { usage(stdout); return 0; }
        fprintf(stderr, "clawc: unknown option %s\n", a);
        usage(stderr);
        cli_free(&cli);
        return 2;
    }
    if (i >= argc) { usage(stderr); cli_free(&cli); return 2; }
    const char *cmd = argv[i++];

    if (str_eq(cmd, "help")) { usage(stdout); cli_free(&cli); return 0; }
    if (str_eq(cmd, "version")) {
        printf("clawc %s\n", CLAWC_VERSION);
        cli_free(&cli);
        return 0;
    }

    /* subcommand-local flags (all verbs except run/fetch, whose args are payload):
       -c CONFIG, daemon -i/-p, serve -p, telegram -token/-allow/-poll/--once */
    if (!str_eq(cmd, "run") && !str_eq(cmd, "fetch") && !str_eq(cmd, "cron") && !str_eq(cmd, "swarm")) {
        for (; i < argc; i++) {
            if ((str_eq(argv[i], "-c") || str_eq(argv[i], "--config")) && i + 1 < argc) {
                free(cli.cfg_path);
                cli.cfg_path = xstrdup(argv[++i]);
            }
            else if (argv[i][0] == '-' && argv[i][1] && !argv[i][2] && i + 1 < argc &&
                     cli_short_value(&cli, argv[i][1], argv[i+1], F_HB|F_SERVE|F_BIND)) { i++; }
            else if (str_eq(argv[i], "-token") && i + 1 < argc) { cli.tg_token = xstrdup(argv[++i]); cli.set |= F_TGTOK; }
            else if (str_eq(argv[i], "-allow") && i + 1 < argc) { cli.tg_allow = xstrdup(argv[++i]); cli.set |= F_TGALW; }
            else if (str_eq(argv[i], "-poll") && i + 1 < argc) { cli.tg_poll = atol(argv[++i]); cli.set |= F_TGPOLL; }
            else if (str_eq(argv[i], "--once")) { cli.set |= F_ONCE; }
            else { fprintf(stderr, "clawc: bad argument %s\n", argv[i]); return 2; }
        }
    }

    /* config resolution: defaults <- file <- env <- CLI flags */
    Cfg cfg;
    cfg_init(&cfg);
    char dflt_buf[512];
    char *dflt_path = dflt_buf;
    {
        const char *home = getenv("HOME");
        snprintf(dflt_buf, sizeof dflt_buf, "%s/.clawc/config",
                 home && home[0] ? home : ".");
    }
    const char *cpath = cli.cfg_path ? cli.cfg_path : getenv("CLAWC_CONFIG");
    if (!cpath && file_exists(dflt_path)) cpath = dflt_path;
    if (cpath && !str_eq(cmd, "setup")) { /* setup writes the file; never blocks on it */
        int r = cfg_load_file(&cfg, cpath);
        if (r == 1) {
            fprintf(stderr, "clawc: config file not found: %s\n", cpath);
            return 2;
        }
        if (r < 0) return 2;
    }
    cfg_load_env(&cfg);
    apply_cli(&cfg, &cli);

    if (str_eq(cmd, "setup")) {
        const char *target = cli.cfg_path ? cli.cfg_path
                            : (getenv("CLAWC_CONFIG") && getenv("CLAWC_CONFIG")[0])
                              ? getenv("CLAWC_CONFIG") : dflt_path;
        int src = cmd_setup(target, cfg.insecure);
        cli_free(&cli);
        cfg_free(&cfg);
        return src;
    }

    app.agent.tools.workspace = cfg.workspace ? cfg.workspace : default_workspace();

    if (str_eq(cmd, "init")) {
        cmd_init(app.agent.tools.workspace);
        if (!cfg.workspace) free((void *)app.agent.tools.workspace);
        agent_free(&app.agent);
        cli_free(&cli);
        cfg_free(&cfg);
        return 0;
    }


    /* configure LLM */
    app.agent.llm.api_url = cfg.api_url ? cfg.api_url : xstrdup("https://api.openai.com/v1");
    app.agent.llm.context_chars = cfg.context_chars;
    app.agent.auto_learn = cfg.auto_learn;
    app.agent.tools.a2a_peers = cfg.a2a_peers;
    app.agent.tools.bridge_token = cfg.bridge_token;
    app.agent.tools.a2a_ca = cfg.a2a_ca;
    app.agent.tools.a2a_tls = cfg.a2a_tls;
#ifndef CLAWC_NO_MATRIX
    app.mx.homeserver = cfg.mx_hs;
    app.mx.token = cfg.mx_token;
    app.mx.user = cfg.mx_user;
    app.mx.rooms = cfg.mx_rooms;
    app.mx.users = cfg.mx_users;
#endif
#ifndef CLAWC_NO_DISCORD
    app.dc.token = cfg.dc_token;
    app.dc.guilds = cfg.dc_guilds;
    app.dc.channels = cfg.dc_channels;
    app.dc.users = cfg.dc_users;
    app.dc.gateway = getenv("CLAWC_DISCORD_GATEWAY");
    app.dc.api = getenv("CLAWC_DISCORD_API");
#endif
    app.agent.llm.model = cfg.model ? cfg.model : xstrdup("gpt-4o-mini");
    app.agent.llm.max_tokens = 4096;
    app.agent.tools.timeout_s = cfg.timeout;
    app.agent.llm.timeout_s = cfg.timeout > 0 ? cfg.timeout : 60;
    app.agent.llm.insecure = cfg.insecure;
    app.agent.tools.insecure_tls = cfg.insecure;
    app.agent.max_steps = cfg.max_steps > 0 ? cfg.max_steps : 8;
    app.agent.llm.api_key = xstrdup(cfg.api_key && cfg.api_key[0] ? cfg.api_key : "");

    if (cfg.api_kind && cfg.api_kind[0]) {
        app.agent.llm.api_kind = str_eq(cfg.api_kind, "anthropic") ? API_ANTHROPIC : API_OPENAI;
    } else {
        app.agent.llm.api_kind = strstr(app.agent.llm.api_url, "anthropic") ? API_ANTHROPIC
                                                                          : API_OPENAI;
    }

    app.agent.tools.sandbox = NULL;
    app.agent.tools.os_sandbox = 0;
    if (cfg.sandbox && cfg.sandbox[0]) {
        char real[4096];
        if (str_eq(cfg.sandbox, "landlock") || str_eq(cfg.sandbox, "os")) {
            /* OS-enforced mode ("landlock" kept as alias).  tools.sandbox is
               pre-seeded with the workspace so that if no OS backend exists,
               the agent can downgrade to real dir checks on it. */
            app.agent.tools.os_sandbox = 1;
            if (realpath(app.agent.tools.workspace ? app.agent.tools.workspace
                                                  : ".", real))
                app.agent.tools.sandbox = xstrdup(real);
            else
                app.agent.tools.sandbox = xstrdup(
                    app.agent.tools.workspace ? app.agent.tools.workspace : ".");
        } else if (!realpath(cfg.sandbox, real)) {
            fprintf(stderr, "clawc: sandbox dir must exist: %s\n", cfg.sandbox);
            return 2;
        } else {
            app.agent.tools.sandbox = xstrdup(real);
        }
    }

    /* channel settings */
    app.notify_cmd = cfg.notify;
    app.serve_port = cfg.serve_port;
    app.serve_bind = cfg.serve_bind;
    app.serve_tls = cfg.serve_tls;
    app.serve_workers = cfg.serve_workers;
    app.serve_cert = cfg.serve_cert;
    app.serve_key = cfg.serve_key;
    app.hb_interval = cfg.hb_interval;
    app.au_interval = cfg.autonomous_interval;
    app.tg.token = cfg.tg_token;
    app.tg.users = cfg.tg_users;
    app.tg.api_base = cfg.tg_api ? cfg.tg_api : xstrdup("https://api.telegram.org");
    app.tg.poll_timeout = cfg.tg_poll > 0 ? cfg.tg_poll : 50;
    app.tg.insecure = cfg.insecure;
    app.tg.auto_approve = cfg.auto_approve;
    app.tg.once = (cli.set & F_ONCE) ? 1 : 0;
    tg_parse_allow(&app.tg, cfg.tg_allow);

    /* Sandbox gate: applied ONCE, here, before anything runs.  With
       "sandbox": "os" the kernel backend is used when one exists (landlock /
       sandbox_init / pledge).  Otherwise the clawc jail takes over as the
       fallback - our own userspace enforcement: file tools are dir-checked
       against the workspace and every subprocess runs with the jailshim
       path interposer (tools/jailshim.c) plus rlimits.  Never silent: the
       active tier is announced on stderr. */
    if (app.agent.tools.os_sandbox) {
        if (sandbox_available() && sandbox_apply(app.agent.tools.workspace) == 0) {
            fprintf(stderr, "[sandbox] OS sandbox active: %s\n", sandbox_backend());
        } else {
            char exe[4096], sopath[4600], roots[8192], ld[6400];
            const char *so = getenv("CLAWC_JAIL_SO");
            const char *prev;
            ssize_t n;

            /* file confinement now rests on safe_path dir checks */
            app.agent.tools.os_sandbox = 0;
            snprintf(roots, sizeof roots, "%s:/tmp:/dev",
                     app.agent.tools.workspace ? app.agent.tools.workspace : ".");
            jail_arm(roots); /* syscall tripwire for run_shell (src/jail.c) */
            if (!so) {
                n = readlink("/proc/self/exe", exe, sizeof exe - 1);
                if (n > 0) {
                    exe[n] = 0;
                    snprintf(sopath, sizeof sopath, "%s-jail.so", exe);
                    so = sopath;
                }
            }
            if (so && access(so, R_OK) == 0) {
                setenv("CLAWC_JAIL_ROOTS", roots, 1);
                prev = getenv("LD_PRELOAD");
                snprintf(ld, sizeof ld, "%s%s%s", so, prev ? " " : "",
                         prev ? prev : "");
                setenv("LD_PRELOAD", ld, 1);
                setenv("DYLD_INSERT_LIBRARIES", so, 1); /* macOS best-effort */
            }
            if (!so || access(so, R_OK) != 0)
                fprintf(stderr, "[sandbox] jail shim unavailable\n");
            fputs(packed_text(TXT_EXTRA_13), stderr);
        }
    }

    int rc;
    if (str_eq(cmd, "swarm")) { app.agent.auto_approve = cfg.auto_approve; rc = i < argc ? swarm_run(&app.agent, argv[i]) : 2; }
    else if (str_eq(cmd, "run")) rc = cmd_run(&app, argc - i, argv + i, cfg.auto_approve);
    else if (str_eq(cmd, "chat")) rc = cmd_chat(&app, cfg.auto_approve);
#ifndef CLAWC_NO_HEARTBEAT
    else if (str_eq(cmd, "heartbeat")) rc = cmd_heartbeat(&app);
#endif
    else if (str_eq(cmd, "config")) rc = cmd_config(&cfg);
    else if (str_eq(cmd, "fetch")) {
        if (i >= argc) { fprintf(stderr, "clawc: fetch needs a URL\n"); rc = 2; }
        else {
            HttpResp *r = http_request("GET", argv[i], NULL, 0, NULL,
                                       app.agent.tools.timeout_s > 0 ? app.agent.tools.timeout_s : 60,
                                       cfg.insecure);
            if (r->status < 0) {
                fprintf(stderr, "clawc: %s\n", r->err ? r->err : "request failed");
                rc = 1;
            } else {
                fwrite(r->body.s, 1, r->body.len, stdout);
                if (r->body.len == 0 || r->body.s[r->body.len-1] != '\n') printf("\n");
                rc = 0;
            }
            http_resp_free(r);
        }
    }
#if defined(CLAWC_NO_TELEGRAM) || defined(CLAWC_NO_SERVE)
    else if (0
#ifndef CLAWC_NO_TELEGRAM
        || str_eq(cmd, "telegram")
#endif
#ifndef CLAWC_NO_SERVE
        || str_eq(cmd, "serve")
#endif
        ) {
        fprintf(stderr, "clawc: '%s' is not compiled into this build (make FEATURES=...)\n", cmd);
        rc = 2;
    }
#endif
#ifndef CLAWC_NO_TELEGRAM
    else if (str_eq(cmd, "telegram")) {
        if (!app.tg.token || !app.tg.token[0]) {
            fprintf(stderr, "clawc: telegram needs a token (-token or telegram_token)\n");
            rc = 2;
        } else {
            rc = telegram_run(&app.agent, &app.tg);
        }
    }
#endif /* telegram */
    else if (str_eq(cmd, "doctor")) rc = cmd_doctor(&app);
    else if (str_eq(cmd, "improve")) rc = cmd_improve(&app);
    else if (str_eq(cmd, "autonomous")) rc = cmd_autonomous(&app);
#ifndef CLAWC_NO_MATRIX
    else if (str_eq(cmd, "matrix")) {
        app.mx.once = 1;
        rc = cmd_matrix(&app);
    }
#endif
#ifndef CLAWC_NO_DISCORD
    else if (str_eq(cmd, "discord")) {
        app.dc.once = 1;
        rc = cmd_discord(&app);
    }
#endif
#ifndef CLAWC_NO_CRON
    else if (str_eq(cmd, "cron")) rc = cmd_cron(&app, argc - i, argv + i);
#endif
    else if (str_eq(cmd, "daemon")) rc = cmd_daemon(&app);
#ifndef CLAWC_NO_SERVE
    else if (str_eq(cmd, "serve")) {
        signal(SIGPIPE, SIG_IGN);
        rc = cmd_serve(&app, app.serve_port >= 0 ? app.serve_port : 8848);
    }
#endif
    else {
        fprintf(stderr, "clawc: unknown command '%s'\n", cmd);
        usage(stderr);
        rc = 2;
    }
    if (!cfg.tg_api) free((void *)app.tg.api_base); /* our default strdup */
    if (!cfg.workspace) free((void *)app.agent.tools.workspace);
    if (!cfg.model) free((void *)app.agent.llm.model);   /* our default strdup */
    free((void *)app.agent.llm.api_key);                 /* always our copy */
    agent_free(&app.agent);
    cli_free(&cli);
    cfg_free(&cfg);
    return rc;
}
