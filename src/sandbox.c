/* clawc - sandbox.c: OS-enforced confinement.
 *
 *  - Linux:   Landlock LSM (the original backend).
 *  - macOS:   sandbox_init(3) seatbelt profile, equivalent semantics.
 *  - OpenBSD: unveil(2) + pledge(2), equivalent semantics.
 *  - others:  no backend.  sandbox_available() returns 0 and callers
 *             HARD-REFUSE to run with "sandbox": "os" (see sandbox.h) -
 *             there is no silent degradation and no fake fallback.
 *
 * SANDBOX_STUBS=1 compiles the no-backend branch anywhere (used by the test
 * suite to exercise the hard refusal on a Linux host).
 */
#define _GNU_SOURCE /* syscall(), O_PATH */
#include "sandbox.h"

#if !defined(CLAWC_SANDBOX_STUBS) && defined(__linux__)
#define SB_LANDLOCK 1
#elif !defined(CLAWC_SANDBOX_STUBS) && defined(__APPLE__)
#define SB_APPLE 1
#elif !defined(CLAWC_SANDBOX_STUBS) && defined(__OpenBSD__)
#define SB_OPENBSD 1
#else
#define SB_STUBS 1
#endif

#ifdef SB_LANDLOCK

#include <fcntl.h>
#include <linux/landlock.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13) /* ABI 2 */
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14) /* ABI 3 */
#endif

static int ll_create_ruleset(const struct landlock_ruleset_attr *attr,
                             size_t size, __u32 flags)
{
    return syscall(444, attr, size, flags); /* __NR_landlock_create_ruleset */
}
static int ll_add_rule(int fd, __u32 t, const void *r, __u32 f)
{
    return syscall(445, fd, t, r, f);       /* __NR_landlock_add_rule */
}
static int ll_restrict_self(int fd, __u32 f)
{
    return syscall(446, fd, f);             /* __NR_landlock_restrict_self */
}

static void rule(int rset, const char *path, __u64 acc)
{
    struct landlock_path_beneath_attr pb;
    int fd;

    if ((fd = open(path, O_PATH)) < 0) return;
    pb.allowed_access = acc;
    pb.parent_fd = fd;
    ll_add_rule(rset, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
    close(fd);
}

int sandbox_apply(const char *ws)
{
    struct landlock_ruleset_attr attr;
    int rset, abi;
    __u64 w;
    const __u64 ro = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;

    abi = ll_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0) return -1;

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return -1;

    memset(&attr, 0, sizeof(attr));
    /* Read/exec everywhere (shell tools, curl, ...), write only where we say.
       REFER is ABI 2, TRUNCATE is ABI 3 - both must be gated here AND when
       passing allowed_access to add_rule, or the kernel rejects the rule. */
    w = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR
      | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR
      | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG
      | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO
      | LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
    if (abi >= 2) w |= LANDLOCK_ACCESS_FS_REFER;
    if (abi >= 3) w |= LANDLOCK_ACCESS_FS_TRUNCATE;
    attr.handled_access_fs = ro | LANDLOCK_ACCESS_FS_EXECUTE | w;

    rset = ll_create_ruleset(&attr, sizeof(attr), 0);
    if (rset < 0) return -1;

    rule(rset, "/", ro | LANDLOCK_ACCESS_FS_EXECUTE); /* system: read + exec */
    if (ws) rule(rset, ws, w);                        /* workspace: rw */
    rule(rset, "/tmp", w);
    rule(rset, "/dev", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | w);
    rule(rset, "/proc", ro);

    if (ll_restrict_self(rset, 0)) { close(rset); return -1; }
    close(rset);
    return 0;
}

int sandbox_available(void)
{
    /* Probe the kernel, not just the OS: pre-5.13 kernels must report 0 so
       main.c can hard-refuse instead of pretending. */
    return ll_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION) >= 0;
}
const char *sandbox_backend(void) { return "landlock"; }

#elif defined(SB_APPLE)

/* macOS seatbelt via sandbox_init(3).  Same policy as the Landlock backend:
   read/exec everywhere, write only in the workspace, /tmp and /dev, full
   network access (the LLM/telegram traffic lives there).  The profile's
   ordered rules are last-match-wins: general deny of file writes, then the
   specific writable subpaths.  sandbox_init is deprecated in the SDK but is
   still the only apply-to-self API (sandbox-exec is a wrapper around it);
   the pragma keeps the build quiet. */
#include <sandbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* Quote a path for the SBPL string literal: backslash + double-quote. */
static void sb_quote(char *dst, size_t cap, const char *p)
{
    size_t o = 0;
    for (; *p && o + 3 < cap; p++) {
        if (*p == '"' || *p == '\\') dst[o++] = '\\';
        dst[o++] = *p;
    }
    dst[o] = '\0';
}

int sandbox_apply(const char *ws)
{
    char q1[PATH_MAX * 2], q2[PATH_MAX * 2], profile[PATH_MAX * 6 + 512];
    char real[PATH_MAX];
    char *err = NULL;
    int n;

    q1[0] = q2[0] = '\0';
    if (ws && ws[0]) {
        sb_quote(q1, sizeof(q1), ws);
        /* Resolve symlinks (/tmp -> /private/tmp) so both spellings work. */
        if (realpath(ws, real)) sb_quote(q2, sizeof(q2), real);
    }

    n = snprintf(profile, sizeof(profile),
        "(version 1)\n"
        "(allow default)\n"
        "(deny file-write*)\n"
        "(allow file-write*\n"
        "  (subpath \"%s\") (subpath \"%s\")\n"
        "  (subpath \"/tmp\") (subpath \"/private/tmp\")\n"
        "  (subpath \"/dev\") (subpath \"/private/dev\"))\n",
        q1, q2);
    if (n < 0 || n >= (int)sizeof(profile)) return -1;

    if (sandbox_init(profile, 0, &err) != 0) {
        if (err) sandbox_free(err);
        return -1;
    }
    return 0;
}

int sandbox_available(void) { return 1; }
const char *sandbox_backend(void) { return "sandbox_init"; }

#elif defined(SB_OPENBSD)

/* OpenBSD: unveil(2) builds the path map (enforced by the kernel on every
   lookup), then pledge(2) locks the syscall set.  Once unveil(NULL,NULL)
   seals the map and pledge runs without "unveil", neither can be widened. */
#include <unistd.h>

int sandbox_apply(const char *ws)
{
    if (unveil("/", "rx") < 0) return -1;            /* read + exec everywhere */
    if (ws && unveil(ws, "rwc") < 0) return -1;      /* workspace: rw + create */
    if (unveil("/tmp", "rwc") < 0) return -1;
    if (unveil("/dev", "rw") < 0) return -1;
    if (unveil(NULL, NULL) < 0) return -1;           /* seal the map */
    /* What the agent needs: stdio on open fds, reads/writes/creates,
       exec of sh for run_shell, fork/wait/kill, ttys, TCP + DNS for the
       LLM and gateways.  Everything else (ptrace, mount, setuid, sysctl,
       ...) becomes EPERM. */
    if (pledge("stdio rpath wpath cpath exec proc tty inet dns", NULL) < 0)
        return -1;
    return 0;
}

int sandbox_available(void) { return 1; }
const char *sandbox_backend(void) { return "pledge"; }

#else /* SB_STUBS: platforms with no OS sandbox, or SANDBOX_STUBS=1 */

int sandbox_apply(const char *ws) { (void)ws; return -1; }
int sandbox_available(void) { return 0; }
const char *sandbox_backend(void) { return "none"; }

#endif
