/* clawc - jail.c: launcher for the clawc jail's supervision helper.
 *
 * The heavy machinery (seccomp tripwire + policy engine) lives in
 * tools/jaild.c, built as <target>-jaild next to clawc - part of the jail
 * LAYER (like clawc-jail.so), not the clawc binary.  jail_run() is a
 * drop-in for proc_run(): it routes run_shell through the helper so every
 * descendant's path syscalls are checked against the jail roots.  If the
 * helper is missing or cannot supervise this kernel, the command still
 * runs (unsupervised) and we say so once. */
#define _XOPEN_SOURCE 700
#include "jail.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_roots[4096];
static int g_armed;
static int g_warned;

void jail_arm(const char *roots)
{
    if (roots && roots[0]) {
        snprintf(g_roots, sizeof g_roots, "%s", roots);
        g_armed = 1;
    } else {
        g_armed = 0;
    }
}

int jail_armed(void) { return g_armed; }

/* Locate <exe>-jaild (same convention as <exe>-jail.so), or CLAWC_JAILD. */
static const char *jaild_path(char *buf, size_t cap)
{
    const char *so = getenv("CLAWC_JAILD");
#if defined(__linux__)
    char exe[4096];
    ssize_t n;
#endif
    if (so && so[0]) return so;
#if defined(__linux__)
    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        snprintf(buf, cap, "%s-jaild", exe);
        return buf;
    }
#endif
    return NULL;
}

int jail_run(const char *cmd, int timeout_s, Buf *out, size_t cap)
{
    char path[4600], line[8300], esc[4200];
    const char *helper;
    int rc;

    if (!g_armed) return proc_run(cmd, timeout_s, out, cap);
    helper = jaild_path(path, sizeof path);
    if (!helper || access(helper, X_OK) != 0) {
        if (!g_warned) {
            g_warned = 1;
            fprintf(stderr, "[jail] helper not found (%s) - shell runs "
                            "unsupervised; shim may be unavailable\n",
                    helper ? helper : "clawc-jaild?");
        }
        return proc_run(cmd, timeout_s, out, cap);
    }
    /* single-quote the helper path for sh */
    {
        size_t i, o = 0;
        for (i = 0; helper[i] && o + 5 < sizeof esc; i++) {
            if (helper[i] == '\'') { memcpy(esc + o, "'\\''", 4); o += 4; }
            else esc[o++] = helper[i];
        }
        esc[o] = 0;
    }
    setenv("CLAWC_JAIL_CMD", cmd, 1);
    setenv("CLAWC_JAIL_ROOTS", g_roots, 1);
    {
        char t[16];
        snprintf(t, sizeof t, "%d", timeout_s > 0 ? timeout_s : 60);
        setenv("CLAWC_JAIL_TIMEOUT", t, 1);
    }
    snprintf(line, sizeof line, "exec '%s'", esc);
    rc = proc_run(line, (timeout_s > 0 ? timeout_s : 60) + 15, out, cap);
    unsetenv("CLAWC_JAIL_CMD");
    return rc;
}
