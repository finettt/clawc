/* clawc - daemon.h: multi-gateway supervisor.
   Forks one child per configured gateway and keeps them alive: a gateway that
   exits is restarted with exponential backoff (2s..60s). SIGINT/SIGTERM stop
   all children gracefully. Pure POSIX; each gateway is process-isolated. */
#ifndef CLAWC_DAEMON_H
#define CLAWC_DAEMON_H

typedef struct {
    const char *name;
    int (*fn)(void *ctx); /* runs forever in the child; return = exit code */
    void *ctx;
} Gw;

/* Run until stopped. Returns 0 after clean shutdown, 2 if n <= 0. */
int daemon_run(const Gw *gws, int n);

#endif
