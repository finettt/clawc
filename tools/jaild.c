/* clawc - experimental Linux seccomp-notification helper.
 * Checks selected syscalls only; TOCTOU and other bypasses make this
 * unsuitable as a security boundary. On setup failure it warns and runs
 * the command once with only the preload shim, if available. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#endif
#ifndef SECCOMP_USER_NOTIF_FLAG_CONTINUE
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE 0x00000002U
#endif
#ifndef SECCOMP_IOCTL_NOTIF_RESPOND
#define SECCOMP_IOCTL_NOTIF_RESPOND SECCOMP_IOCTL_NOTIF_SEND
#endif

#define MAX_ROOTS 32

static char g_roots[MAX_ROOTS][PATH_MAX];
static int g_nroots;

/* ---- policy engine (same shape as tools/jailshim.c) ---- */

static int write_allowed(const char *base, const char *p)
{
    char joined[PATH_MAX], resolved[PATH_MAX];
    const char *check;
    int i;

    if (!p || !p[0]) return 1;
    if (p[0] == '/') {
        if (strlen(p) >= sizeof joined) return 0;
        snprintf(joined, sizeof joined, "%s", p);
    } else {
        if (!base || !base[0] || strlen(base) + 1 + strlen(p) >= sizeof joined) return 0;
        snprintf(joined, sizeof joined, "%s/%s", base, p);
    }
    if (realpath(joined, resolved)) {
        check = resolved;
    } else {
        char parent[PATH_MAX];
        char *slash = strrchr(joined, '/');
        const char *leaf;
        if (!slash || !slash[1]) return 0;
        leaf = slash + 1;
        if (slash == joined) snprintf(parent, sizeof parent, "/");
        else {
            size_t plen = (size_t)(slash - joined);
            if (plen >= sizeof parent) return 0;
            memcpy(parent, joined, plen);
            parent[plen] = 0;
        }
        if (!realpath(parent, resolved)) return 0;
        if (strlen(resolved) + 1 + strlen(leaf) >= sizeof resolved) return 0;
        if (strcmp(resolved, "/") != 0) strcat(resolved, "/");
        strcat(resolved, leaf);
        check = resolved;
    }
    for (i = 0; i < g_nroots; i++) {
        size_t n = strlen(g_roots[i]);
        if (strncmp(check, g_roots[i], n) == 0
            && (check[n] == 0 || check[n] == '/'
                || (n == 1 && g_roots[i][0] == '/')))
            return 1;
    }
    return 0;
}

static int read_str(pid_t pid, unsigned long addr, char *out, size_t cap)
{
    struct iovec loc = { out, cap - 1 }, rem = { (void *)addr, cap - 1 };
    ssize_t n = process_vm_readv(pid, &loc, 1, &rem, 1, 0);
    if (n <= 0) return -1;
    out[n] = 0;
    return 0;
}

static int read_u64(pid_t pid, unsigned long addr, unsigned long long *out)
{
    struct iovec loc = { out, sizeof *out }, rem = { (void *)addr, sizeof *out };
    return process_vm_readv(pid, &loc, 1, &rem, 1, 0) == (ssize_t)sizeof *out ? 0 : -1;
}

static void dirfd_path(pid_t pid, int dirfd, char *out, size_t cap)
{
    char link[64];
    ssize_t n;
    if (dirfd == AT_FDCWD) snprintf(link, sizeof link, "/proc/%d/cwd", (int)pid);
    else snprintf(link, sizeof link, "/proc/%d/fd/%d", (int)pid, dirfd);
    n = readlink(link, out, cap - 1);
    if (n > 0) out[n] = 0;
    else out[0] = 0;
}

/* rule table: nr -> path arg positions i0/i1, dirfd positions d0/d1
   (0xff = cwd), flags position for open-likes (0xff = always a write) */
struct jrule { int nr; unsigned char i0, i1, d0, d1, fl; };
static const struct jrule rules[] = {
#ifdef __NR_open
    { __NR_open, 0, 0xff, 0xff, 0xff, 1 },
#endif
#ifdef __NR_open64
    { __NR_open64, 0, 0xff, 0xff, 0xff, 1 },
#endif
#ifdef __NR_creat
    { __NR_creat, 0, 0xff, 0xff, 0xff, 0xff },
#endif
#ifdef __NR_unlink
    { __NR_unlink, 0, 0xff, 0xff, 0xff, 0xff },
#endif
#ifdef __NR_rmdir
    { __NR_rmdir, 0, 0xff, 0xff, 0xff, 0xff },
#endif
#ifdef __NR_mkdir
    { __NR_mkdir, 0, 0xff, 0xff, 0xff, 0xff },
#endif
#ifdef __NR_truncate
    { __NR_truncate, 0, 0xff, 0xff, 0xff, 0xff },
#endif
#ifdef __NR_truncate64
    { __NR_truncate64, 0, 0xff, 0xff, 0xff, 0xff },
#endif
#ifdef __NR_chmod
    { __NR_chmod, 0, 0xff, 0xff, 0xff, 0xff },
#endif
#ifdef __NR_chown
    { __NR_chown, 0, 0xff, 0xff, 0xff, 0xff },
#endif
#ifdef __NR_lchown
    { __NR_lchown, 0, 0xff, 0xff, 0xff, 0xff },
#endif
#ifdef __NR_mknod
    { __NR_mknod, 0, 0xff, 0xff, 0xff, 0xff },
#endif
#ifdef __NR_openat
    { __NR_openat, 1, 0xff, 0, 0xff, 2 },
#endif
#ifdef __NR_unlinkat
    { __NR_unlinkat, 1, 0xff, 0, 0xff, 0xff },
#endif
#ifdef __NR_mkdirat
    { __NR_mkdirat, 1, 0xff, 0, 0xff, 0xff },
#endif
#ifdef __NR_mknodat
    { __NR_mknodat, 1, 0xff, 0, 0xff, 0xff },
#endif
#ifdef __NR_fchmodat
    { __NR_fchmodat, 1, 0xff, 0, 0xff, 0xff },
#endif
#ifdef __NR_fchownat
    { __NR_fchownat, 1, 0xff, 0, 0xff, 0xff },
#endif
#ifdef __NR_symlink
    { __NR_symlink, 1, 0xff, 0xff, 0xff, 0xff },
#endif
#ifdef __NR_symlinkat
    { __NR_symlinkat, 2, 0xff, 1, 0xff, 0xff },
#endif
#ifdef __NR_link
    { __NR_link, 0, 1, 0xff, 0xff, 0xff },
#endif
#ifdef __NR_rename
    { __NR_rename, 0, 1, 0xff, 0xff, 0xff },
#endif
#ifdef __NR_linkat
    { __NR_linkat, 1, 3, 0, 2, 0xff },
#endif
#ifdef __NR_renameat
    { __NR_renameat, 1, 3, 0, 2, 0xff },
#endif
#ifdef __NR_renameat2
    { __NR_renameat2, 1, 3, 0, 2, 0xff },
#endif
};

static int classify(long nr, const unsigned long long *a, pid_t pid,
                    char paths[][PATH_MAX], char dirs[][PATH_MAX])
{
    const struct jrule *r = NULL;
    size_t i;
    int flags;

    dirs[0][0] = dirs[1][0] = 0;
#ifdef __NR_openat2
    if (nr == __NR_openat2) {
        unsigned long long how = 0;
        dirfd_path(pid, (int)a[0], dirs[0], PATH_MAX);
        if (read_u64(pid, (unsigned long)a[2], &how) != 0) how = O_WRONLY;
        if (!((int)how & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND))) return 0;
        if (read_str(pid, (unsigned long)a[1], paths[0], PATH_MAX) != 0) return 3;
        return 1;
    }
#endif
    for (i = 0; i < sizeof rules / sizeof rules[0]; i++)
        if (rules[i].nr == nr) { r = &rules[i]; break; }
    if (!r) return -1;
    if (r->fl != 0xff) {
        flags = (int)a[r->fl];
        if (!(flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND))) return 0;
    }
    if (r->d0 != 0xff) dirfd_path(pid, (int)a[r->d0], dirs[0], PATH_MAX);
    else dirfd_path(pid, AT_FDCWD, dirs[0], PATH_MAX);
    if (r->i1 != 0xff) {
        if (r->d1 != 0xff) dirfd_path(pid, (int)a[r->d1], dirs[1], PATH_MAX);
        else memcpy(dirs[1], dirs[0], PATH_MAX);
        if (read_str(pid, (unsigned long)a[r->i0], paths[0], PATH_MAX) != 0) return 3;
        if (read_str(pid, (unsigned long)a[r->i1], paths[1], PATH_MAX) != 0) return 3;
        return 2;
    }
    if (read_str(pid, (unsigned long)a[r->i0], paths[0], PATH_MAX) != 0) return 3;
    return 1;
}

/* ---- the seccomp tripwire ---- */

static int build_filter(struct sock_fprog *prog)
{
    static struct sock_filter f[80];
    int n = 0, i;
    static const long traps[] = {
#ifdef __NR_open
        __NR_open,
#endif
#ifdef __NR_open64
        __NR_open64,
#endif
#ifdef __NR_openat
        __NR_openat,
#endif
#ifdef __NR_openat2
        __NR_openat2,
#endif
#ifdef __NR_creat
        __NR_creat,
#endif
#ifdef __NR_unlink
        __NR_unlink,
#endif
#ifdef __NR_unlinkat
        __NR_unlinkat,
#endif
#ifdef __NR_rmdir
        __NR_rmdir,
#endif
#ifdef __NR_mkdir
        __NR_mkdir,
#endif
#ifdef __NR_mkdirat
        __NR_mkdirat,
#endif
#ifdef __NR_rename
        __NR_rename,
#endif
#ifdef __NR_renameat
        __NR_renameat,
#endif
#ifdef __NR_renameat2
        __NR_renameat2,
#endif
#ifdef __NR_link
        __NR_link,
#endif
#ifdef __NR_linkat
        __NR_linkat,
#endif
#ifdef __NR_symlink
        __NR_symlink,
#endif
#ifdef __NR_symlinkat
        __NR_symlinkat,
#endif
#ifdef __NR_truncate
        __NR_truncate,
#endif
#ifdef __NR_truncate64
        __NR_truncate64,
#endif
#ifdef __NR_chmod
        __NR_chmod,
#endif
#ifdef __NR_chown
        __NR_chown,
#endif
#ifdef __NR_lchown
        __NR_lchown,
#endif
#ifdef __NR_fchmodat
        __NR_fchmodat,
#endif
#ifdef __NR_fchownat
        __NR_fchownat,
#endif
#ifdef __NR_mknod
        __NR_mknod,
#endif
#ifdef __NR_mknodat
        __NR_mknodat,
#endif
        -1
    };
#if defined(__x86_64__)
    const __u32 arch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    const __u32 arch = AUDIT_ARCH_AARCH64;
#elif defined(__riscv) && __riscv_xlen == 64
    const __u32 arch = AUDIT_ARCH_RISCV;
#else
    const __u32 arch = 0;
#endif
    if (arch == 0) return -1;
    /* seccomp_data: nr at offset 0, arch at offset 4 */
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 4);
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, arch, 1, 0);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    f[n++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0);
    for (i = 0; traps[i] >= 0 && n < 77; i++) {
        f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (__u32)traps[i], 0, 1);
        f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF);
    }
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    prog->len = (unsigned short)n;
    prog->filter = f;
    return 0;
}

static void handle_notif(int lfd)
{
    struct seccomp_notif req;
    struct seccomp_notif_resp resp;
    char paths[2][PATH_MAX], dirs[2][PATH_MAX];
    int kind, ok;

    memset(&req, 0, sizeof req);
    if (ioctl(lfd, SECCOMP_IOCTL_NOTIF_RECV, &req) != 0) return;
    memset(&resp, 0, sizeof resp);
    resp.id = req.id;
    kind = classify((long)req.data.nr, req.data.args, (pid_t)req.pid, paths, dirs);
    if (kind == 0 || kind < 0) {
        resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
    } else if (kind == 3) {
        resp.error = -EACCES;
        fprintf(stderr, "[jail] blocked unverifiable syscall\n");
    } else {
        ok = write_allowed(dirs[0], paths[0]);
        if (ok && kind == 2) ok = write_allowed(dirs[1], paths[1]);
        if (ok) {
            resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
        } else {
            resp.error = -EACCES;
            fprintf(stderr, "[jail] blocked %s%s%s (pid %d)\n",
                    paths[0], kind == 2 ? " -> " : "", kind == 2 ? paths[1] : "",
                    (int)req.pid);
        }
    }
    ioctl(lfd, SECCOMP_IOCTL_NOTIF_RESPOND, &resp);
}

/* ---- damage limits for the shell tree ---- */

static void limits(int timeout_s)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_FSIZE, &rl) == 0 && rl.rlim_cur > (rlim_t)256 * 1024 * 1024) {
        rl.rlim_cur = (rlim_t)256 * 1024 * 1024;
        setrlimit(RLIMIT_FSIZE, &rl);
    }
    if (getrlimit(RLIMIT_CPU, &rl) == 0) {
        rlim_t want = (rlim_t)(timeout_s > 0 ? timeout_s * 2 + 60 : 300);
        if (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > want) {
            rl.rlim_cur = want;
            setrlimit(RLIMIT_CPU, &rl);
        }
    }
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur > 256) {
        rl.rlim_cur = 256;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

int main(void)
{
    const char *roots = getenv("CLAWC_JAIL_ROOTS");
    const char *cmd = getenv("CLAWC_JAIL_CMD");
    int timeout_s = atoi(getenv("CLAWC_JAIL_TIMEOUT") ? getenv("CLAWC_JAIL_TIMEOUT") : "60");
    struct sock_fprog prog;
    char buf[8192];
    char *save = NULL, *tok;
    int lfd, fds[2], status = 0, rc = -1;
    pid_t pid;
    time_t deadline;

    if (!cmd || !cmd[0]) return 125;
    /* children must not be able to ptrace us and answer their own
       notifications (only relevant on kernels without Yama) */
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    g_nroots = 0;
    if (roots) {
        snprintf(buf, sizeof buf, "%s", roots);
        for (tok = strtok_r(buf, ":", &save);
             tok && g_nroots < MAX_ROOTS;
             tok = strtok_r(NULL, ":", &save)) {
            char real[PATH_MAX];
            size_t n = strlen(tok);
            if (n == 0 || n >= PATH_MAX) continue;
            memcpy(g_roots[g_nroots], tok, n + 1);
            if (n > 1 && g_roots[g_nroots][n - 1] == '/') g_roots[g_nroots][n - 1] = 0;
            if (realpath(g_roots[g_nroots], real))
                snprintf(g_roots[g_nroots], PATH_MAX, "%s", real);
            g_nroots++;
        }
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || build_filter(&prog) != 0)
        goto shim_only;
    lfd = (int)syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
                       SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
    if (lfd < 0) goto shim_only;
    if (pipe(fds) != 0) return 125;
    pid = fork();
    if (pid < 0) return 125;
    if (pid == 0) {
        setpgid(0, 0);
        limits(timeout_s);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        close(lfd);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    deadline = time(NULL) + (timeout_s > 0 ? timeout_s : 60);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    for (;;) {
        struct pollfd pfd[2];
        pfd[0].fd = fds[0];
        pfd[0].events = POLLIN;
        pfd[1].fd = lfd;
        pfd[1].events = POLLIN;
        if (poll(pfd, 2, 1000) < 0 && errno != EINTR) break;
        if (pfd[0].revents & POLLIN) {
            char tmp[4096];
            ssize_t n = read(fds[0], tmp, sizeof tmp);
            if (n > 0) fwrite(tmp, 1, (size_t)n, stdout);
        }
        if (pfd[1].revents & POLLIN) handle_notif(lfd);
        if (time(NULL) >= deadline) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            break;
        }
        if (waitpid(pid, &status, WNOHANG) == pid) {
            for (;;) {
                char tmp[4096];
                ssize_t n = read(fds[0], tmp, sizeof tmp);
                if (n <= 0) break;
                fwrite(tmp, 1, (size_t)n, stdout);
            }
            rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            break;
        }
    }
    close(fds[0]);
    close(lfd);
    if (rc == -1 && waitpid(pid, &status, WNOHANG) != pid) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    fflush(stdout);
    return rc < 0 ? 1 : rc;
shim_only:
    fprintf(stderr, "[jail] syscall supervision unavailable; shim only, not secure containment\n");
    limits(timeout_s);
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    return 127;
}
