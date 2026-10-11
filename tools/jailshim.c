/* clawc - tools/jailshim.c: the clawc jail's shell-side enforcement.
 *
 * A from-scratch, kernel-sandbox-independent path jail.  Built as a shared
 * library and injected into every subprocess via LD_PRELOAD (or
 * DYLD_INSERT_LIBRARIES on macOS): every path-taking libc call from shells,
 * coreutils, interpreters, ... is checked in USERSPACE against
 * CLAWC_JAIL_ROOTS (colon-separated list of writable roots, e.g.
 * "/home/u/ws:/tmp:/dev").  Reads are unrestricted (matching the Landlock
 * policy); anything that writes, creates, removes or renames must land under
 * a root.  Symlink escapes are caught by resolving first (realpath) - a
 * link inside the workspace pointing at /etc/passwd resolves outside and
 * the call is denied.
 *
 * Build:   cc -shared -fPIC -O2 -o clawc-jail.so tools/jailshim.c -ldl
 * Arm:     setenv CLAWC_JAIL_ROOTS (clawc does this automatically), then
 *          LD_PRELOAD=/path/clawc-jail.so in the child environment.
 *
 * Honest limits (documented in PORTABILITY.md): statically linked binaries
 * and code that issues raw syscalls (inline asm) bypass libc interposition.
 * The kernel backends (landlock/sandbox_init/pledge) remain the strong tier;
 * this jail is the fallback where those do not exist.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char *g_roots;          /* ":/root2:..." writable roots */
static int g_len[32];
static char g_root[32][PATH_MAX];
static int g_nroots;

static void jinit(void) __attribute__((constructor));
static void jinit(void)
{
    const char *r = getenv("CLAWC_JAIL_ROOTS");
    g_roots = r;
    g_nroots = 0;
    if (r && r[0]) {
        char buf[8192];
        snprintf(buf, sizeof buf, "%s", r);
        char *save = NULL, *tok = strtok_r(buf, ":", &save);
        while (tok && g_nroots < 32) {
            snprintf(g_root[g_nroots], PATH_MAX, "%s", tok);
            g_len[g_nroots] = (int)strlen(g_root[g_nroots]);
            /* strip trailing slash (but keep "/") */
            if (g_len[g_nroots] > 1 && g_root[g_nroots][g_len[g_nroots]-1] == '/')
                g_root[g_nroots][--g_len[g_nroots]] = 0;
            g_nroots++;
            tok = strtok_r(NULL, ":", &save);
        }
    }
}

/* Resolve p the way the kernel would (cwd-relative, symlinks, ..) and say
   whether a write there is inside the jail.  For not-yet-existing targets
   the parent directory is resolved and the basename appended. */
static int path_writable(const char *p)
{
    char joined[PATH_MAX], resolved[PATH_MAX], parent[PATH_MAX];
    const char *check;
    int i;

    if (!g_roots) return 1;                 /* shim not armed: pass through */
    if (!p || !p[0]) return 1;              /* "" == cwd, which is jailed anyway */

    if (p[0] == '/') {
        if (strlen(p) >= sizeof joined) return 0;
        snprintf(joined, sizeof joined, "%s", p);
    } else {
        char cwd[PATH_MAX];
        size_t cl;
        if (!getcwd(cwd, sizeof cwd)) return 0;
        cl = strlen(cwd);
        if (cl + 1 + strlen(p) >= sizeof joined) return 0;
        snprintf(joined, sizeof joined, "%s/%s", cwd, p);
    }

    if (realpath(joined, resolved)) {
        check = resolved;
    } else {
        /* new file: resolve the parent dir, append basename */
        char *slash = strrchr(joined, '/');
        const char *base;
        if (!slash) return 0;
        base = slash + 1;
        if (!base[0]) return 0;
        if (slash == joined) snprintf(parent, sizeof parent, "/");
        else {
            size_t plen = (size_t)(slash - joined);
            if (plen >= sizeof parent) return 0;
            memcpy(parent, joined, plen);
            parent[plen] = 0;
        }
        if (!realpath(parent, resolved)) return 0;
        if (strlen(resolved) + 1 + strlen(base) >= sizeof resolved) return 0;
        if (strcmp(resolved, "/") != 0) strcat(resolved, "/");
        strcat(resolved, base);
        check = resolved;
    }

    for (i = 0; i < g_nroots; i++) {
        int n = g_len[i];
        if (strncmp(check, g_root[i], n) == 0
            && (check[n] == 0 || check[n] == '/'
                || (n == 1 && g_root[i][0] == '/')))
            return 1;
    }
    return 0;
}

static void deny(void) { errno = EACCES; }

/* ---- real libc pointers, resolved lazily ---- */
#define RESOLVE(fn) \
    static int (*real_##fn)(); \
    if (!real_##fn) *(void **)(&real_##fn) = dlsym(RTLD_NEXT, #fn)

int open(const char *p, int flags, ...)
{
    RESOLVE(open);
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (!path_writable(p)
        && ((flags & (O_WRONLY | O_RDWR))
            || (flags & (O_CREAT | O_TRUNC | O_APPEND)))) {
        deny();
        return -1;
    }
    return real_open(p, flags, mode);
}

int open64(const char *p, int flags, ...)
{
    RESOLVE(open64);
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (!path_writable(p)
        && ((flags & (O_WRONLY | O_RDWR))
            || (flags & (O_CREAT | O_TRUNC | O_APPEND)))) {
        deny();
        return -1;
    }
    return real_open64(p, flags, mode);
}

int openat(int dirfd, const char *p, int flags, ...)
{
    RESOLVE(openat);
    mode_t mode = 0;
    char link[64], target[PATH_MAX];
    ssize_t n;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (!path_writable(p)
        && ((flags & (O_WRONLY | O_RDWR))
            || (flags & (O_CREAT | O_TRUNC | O_APPEND)))) {
        /* relative to a dirfd: re-resolve against the fd's path when we can */
        if (p[0] != '/' && dirfd != AT_FDCWD) {
            snprintf(link, sizeof link, "/proc/self/fd/%d", dirfd);
            n = readlink(link, target, sizeof target - 1);
            if (n > 0) {
                char full[PATH_MAX];
                target[n] = 0;
                if (strlen(target) + 1 + strlen(p) < sizeof full) {
                    snprintf(full, sizeof full, "%s/%s", target, p);
                    if (path_writable(full))
                        return real_openat(dirfd, p, flags, mode);
                }
            }
            deny();
            return -1;
        }
        deny();
        return -1;
    }
    return real_openat(dirfd, p, flags, mode);
}

int openat64(int dirfd, const char *p, int flags, ...)
{
    RESOLVE(openat64);
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (!path_writable(p)
        && ((flags & (O_WRONLY | O_RDWR))
            || (flags & (O_CREAT | O_TRUNC | O_APPEND)))) {
        deny();
        return -1;
    }
    return real_openat64(dirfd, p, flags, mode);
}

int creat(const char *p, mode_t mode)
{
    RESOLVE(creat);
    if (!path_writable(p)) { deny(); return -1; }
    return real_creat(p, mode);
}

int creat64(const char *p, mode_t mode)
{
    RESOLVE(creat64);
    if (!path_writable(p)) { deny(); return -1; }
    return real_creat64(p, mode);
}

FILE *fopen(const char *p, const char *mode)
{
    static FILE *(*real_fopen)(const char *, const char *);
    if (!real_fopen) *(void **)(&real_fopen) = dlsym(RTLD_NEXT, "fopen");
    if (strpbrk(mode, "wax+") && !path_writable(p)) { deny(); return NULL; }
    return real_fopen(p, mode);
}

FILE *fopen64(const char *p, const char *mode)
{
    static FILE *(*real_fopen64)(const char *, const char *);
    if (!real_fopen64) *(void **)(&real_fopen64) = dlsym(RTLD_NEXT, "fopen64");
    if (strpbrk(mode, "wax+") && !path_writable(p)) { deny(); return NULL; }
    return real_fopen64(p, mode);
}

FILE *freopen(const char *p, const char *mode, FILE *stream)
{
    static FILE *(*real_freopen)(const char *, const char *, FILE *);
    if (!real_freopen) *(void **)(&real_freopen) = dlsym(RTLD_NEXT, "freopen");
    if (p && strpbrk(mode, "wax+") && !path_writable(p)) { deny(); return NULL; }
    return real_freopen(p, mode, stream);
}

#define WRAP1(fn, ret, fail, expr) \
    ret fn(const char *p) \
    { \
        RESOLVE(fn); \
        if (!path_writable(p)) { deny(); return fail; } \
        return expr; \
    }

WRAP1(unlink, int, -1, real_unlink(p))
WRAP1(rmdir, int, -1, real_rmdir(p))
WRAP1(remove, int, -1, real_remove(p))

int mkdir(const char *p, mode_t mode)
{
    RESOLVE(mkdir);
    if (!path_writable(p)) { deny(); return -1; }
    return real_mkdir(p, mode);
}

int truncate(const char *p, off_t length)
{
    RESOLVE(truncate);
    if (!path_writable(p)) { deny(); return -1; }
    return real_truncate(p, length);
}

int truncate64(const char *p, off64_t length)
{
    RESOLVE(truncate64);
    if (!path_writable(p)) { deny(); return -1; }
    return real_truncate64(p, length);
}

int unlinkat(int dirfd, const char *p, int flags)
{
    RESOLVE(unlinkat);
    if (!path_writable(p)) { deny(); return -1; }
    return real_unlinkat(dirfd, p, flags);
}

int rename(const char *from, const char *to)
{
    RESOLVE(rename);
    if (!path_writable(from) || !path_writable(to)) { deny(); return -1; }
    return real_rename(from, to);
}

int renameat(int ofd, const char *from, int nfd, const char *to)
{
    RESOLVE(renameat);
    if (!path_writable(from) || !path_writable(to)) { deny(); return -1; }
    return real_renameat(ofd, from, nfd, to);
}

int mkdirat(int dirfd, const char *p, mode_t mode)
{
    RESOLVE(mkdirat);
    if (!path_writable(p)) { deny(); return -1; }
    return real_mkdirat(dirfd, p, mode);
}

int link(const char *from, const char *to)
{
    RESOLVE(link);
    if (!path_writable(from) || !path_writable(to)) { deny(); return -1; }
    return real_link(from, to);
}

int linkat(int ofd, const char *from, int nfd, const char *to, int flags)
{
    RESOLVE(linkat);
    if (!path_writable(from) || !path_writable(to)) { deny(); return -1; }
    return real_linkat(ofd, from, nfd, to, flags);
}

int symlink(const char *target, const char *linkpath)
{
    RESOLVE(symlink);
    if (!path_writable(linkpath)) { deny(); return -1; }
    return real_symlink(target, linkpath);
}

int symlinkat(const char *target, int dirfd, const char *linkpath)
{
    RESOLVE(symlinkat);
    if (!path_writable(linkpath)) { deny(); return -1; }
    return real_symlinkat(target, dirfd, linkpath);
}

int chmod(const char *p, mode_t mode)
{
    RESOLVE(chmod);
    if (!path_writable(p)) { deny(); return -1; }
    return real_chmod(p, mode);
}

int fchmodat(int dirfd, const char *p, mode_t mode, int flags)
{
    RESOLVE(fchmodat);
    if (!path_writable(p)) { deny(); return -1; }
    return real_fchmodat(dirfd, p, mode, flags);
}

int chown(const char *p, uid_t uid, gid_t gid)
{
    RESOLVE(chown);
    if (!path_writable(p)) { deny(); return -1; }
    return real_chown(p, uid, gid);
}

int lchown(const char *p, uid_t uid, gid_t gid)
{
    RESOLVE(lchown);
    if (!path_writable(p)) { deny(); return -1; }
    return real_lchown(p, uid, gid);
}

int fchownat(int dirfd, const char *p, uid_t uid, gid_t gid, int flags)
{
    RESOLVE(fchownat);
    if (!path_writable(p)) { deny(); return -1; }
    return real_fchownat(dirfd, p, uid, gid, flags);
}

int mknod(const char *p, mode_t mode, dev_t dev)
{
    RESOLVE(mknod);
    if (!path_writable(p)) { deny(); return -1; }
    return real_mknod(p, mode, dev);
}

int mknodat(int dirfd, const char *p, mode_t mode, dev_t dev)
{
    RESOLVE(mknodat);
    if (!path_writable(p)) { deny(); return -1; }
    return real_mknodat(dirfd, p, mode, dev);
}
