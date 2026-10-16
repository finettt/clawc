/* clawc - daemon.c: fork-based gateway supervisor. */
#define _XOPEN_SOURCE 700
#include "daemon.h"
#include "util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CRASH_SECS 5   /* a lifetime under this counts as a crash */
#define BACKOFF_MAX 60

static volatile sig_atomic_t g_stop;

static void on_stop(int sig) {
    (void)sig;
    g_stop = 1;
}

typedef struct {
    const Gw *g;
    pid_t pid;
    time_t started;
    int crashes;
    time_t restart_at;
} Child;

static void reap(Child *ch, int n, time_t now) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int i;
        for (i = 0; i < n; i++) {
            if (ch[i].pid != pid) continue;
            int alive = (int)(now - ch[i].started);
            ch[i].crashes = (alive < CRASH_SECS) ? ch[i].crashes + 1 : 0;
            int back = ch[i].crashes ? 2 : 0;
            int k;
            for (k = 1; k < ch[i].crashes && back < BACKOFF_MAX; k++) back *= 2;
            if (back > BACKOFF_MAX) back = BACKOFF_MAX;
            if (back == 0) back = 1;
            if (g_stop)
                fprintf(stderr, "[daemon] gateway %s: stopped\n", ch[i].g->name);
            else
                fprintf(stderr, "[daemon] gateway %s: exited (%s); restart in %ds\n",
                        ch[i].g->name,
                        WIFEXITED(status) ? "clean" : "killed",
                        back);
            ch[i].pid = 0;
            ch[i].restart_at = now + back;
        }
    }
}

static void start_child(Child *c) {
    pid_t p = fork();
    if (p == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        int rc = c->g->fn(c->g->ctx);
        _exit(rc > 0 && rc < 126 ? rc : 0);
    }
    if (p < 0) {
        fprintf(stderr, "[daemon] gateway %s: fork failed\n", c->g->name);
        c->restart_at = time(NULL) + 5;
        return;
    }
    c->pid = p;
    c->started = time(NULL);
    fprintf(stderr, "[daemon] gateway %s: started (pid %d)\n", c->g->name, (int)p);
}

int daemon_run(const Gw *gws, int n) {
    int i;
    if (n <= 0) return 2;

    Child *ch = xmalloc(sizeof(Child) * (size_t)n);
    memset(ch, 0, sizeof(Child) * (size_t)n);

    g_stop = 0;
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    time_t now = time(NULL);
    for (i = 0; i < n; i++) {
        ch[i].g = &gws[i];
        ch[i].restart_at = now;
    }
    fprintf(stderr, "[daemon] supervising %d gateway(s)\n", n);

    for (;;) {
        now = time(NULL);
        reap(ch, n, now);
        if (g_stop) break;
        for (i = 0; i < n; i++)
            if (!ch[i].pid && now >= ch[i].restart_at) start_child(&ch[i]);
        sleep(1);
    }

    fprintf(stderr, "[daemon] shutting down\n");
    for (i = 0; i < n; i++)
        if (ch[i].pid > 0) kill(ch[i].pid, SIGTERM);
    {
        int t;
        for (t = 0; t < 5; t++) {
            int alive = 0;
            reap(ch, n, time(NULL));
            for (i = 0; i < n; i++)
                if (ch[i].pid > 0) alive = 1;
            if (!alive) break;
            sleep(1);
        }
    }
    for (i = 0; i < n; i++) {
        if (ch[i].pid > 0) {
            kill(ch[i].pid, SIGKILL);
            waitpid(ch[i].pid, NULL, 0);
            ch[i].pid = 0;
        }
    }
    fprintf(stderr, "[daemon] stopped\n");
    free(ch);
    return 0;
}
