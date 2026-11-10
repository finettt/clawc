/* clawc - util.c */
#define _POSIX_C_SOURCE 200809L
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "clawc: out of memory\n"); exit(2); }
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "clawc: out of memory\n"); exit(2); }
    return q;
}

char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}
char *xstrdup0(const char *s) {
    return s ? xstrdup(s) : xstrdup("");
}

void buf_init(Buf *b) {
    b->cap = 256;
    b->s = xmalloc(b->cap);
    b->s[0] = 0;
    b->len = 0;
}

void buf_free(Buf *b) {
    free(b->s);
    b->s = NULL;
    b->len = b->cap = 0;
}

void buf_add(Buf *b, const void *data, size_t n) {
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap) b->cap *= 2;
        b->s = xrealloc(b->s, b->cap);
    }
    memcpy(b->s + b->len, data, n);
    b->len += n;
    b->s[b->len] = 0;
}

void buf_puts(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    if (b->len + (size_t)n + 1 > b->cap) {
        while (b->len + (size_t)n + 1 > b->cap) b->cap *= 2;
        b->s = xrealloc(b->s, b->cap);
    }
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

void buf_trim(Buf *b) {
    while (b->len && (b->s[b->len-1] == '\n' || b->s[b->len-1] == '\r' ||
                      b->s[b->len-1] == ' ' || b->s[b->len-1] == '\t'))
        b->s[--b->len] = 0;
}

char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    Buf b;
    buf_init(&b);
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_add(&b, tmp, n);
    fclose(f);
    return b.s; /* caller frees */
}

int write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = strlen(data);
    int ok = fwrite(data, 1, n, f) == n;
    ok = (fclose(f) == 0) && ok;
    return ok ? 0 : -1;
}

int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

char *path_join(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    int need_slash = na && a[na-1] != '/';
    char *p = xmalloc(na + nb + 2);
    memcpy(p, a, na);
    if (need_slash) p[na++] = '/';
    memcpy(p + na, b, nb + 1);
    return p;
}

char *str_trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\n' || s[n-1] == '\r'))
        s[--n] = 0;
    return s;
}

int str_starts(const char *s, const char *pfx) { return strncmp(s, pfx, strlen(pfx)) == 0; }
int str_ends(const char *s, const char *sfx) {
    size_t ns = strlen(s), nx = strlen(sfx);
    return ns >= nx && strcmp(s + ns - nx, sfx) == 0;
}
int str_eq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

/* Damage limits for every subprocess (jail layer): cap file size, CPU time
   and open fds below the parent's ceilings. */
void proc_limits(int timeout_s) {
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

/* Exact comma-separated membership; missing/empty lists fail closed. */
int csv_has(const char *list, const char *item) {
    if (!list || !item || !item[0]) return 0;
    size_t n = strlen(item);
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        const char *trim = end;
        while (trim > p && (trim[-1] == ' ' || trim[-1] == '\t')) trim--;
        if ((size_t)(trim - p) == n && !strncmp(p, item, n)) return 1;
        p = *end ? end + 1 : end;
    }
    return 0;
}

int sender_ready(const char *channel, const char *list) {
    while (list && *list && strchr(" ,\t\r\n", *list)) list++;
    if (list && *list) return 1;
    fprintf(stderr, "[%s] %s_users required\n", channel, channel);
    return 0;
}

int proc_run(const char *cmd, int timeout_s, Buf *out, size_t cap) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        proc_limits(timeout_s);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    time_t deadline = time(NULL) + (timeout_s > 0 ? timeout_s : 60);
    int timed_out = 0;
    char tmp[4096];
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fds[0], &rfds);
        struct timeval tv = {1, 0};
        int r = select(fds[0] + 1, &rfds, NULL, NULL, &tv);
        if (r > 0) {
            ssize_t n = read(fds[0], tmp, sizeof tmp);
            if (n <= 0) break;
            if (out && out->len < cap) {
                size_t room = cap - out->len;
                buf_add(out, tmp, (size_t)n < room ? (size_t)n : room);
            }
        } else if (r < 0 && errno != EINTR) {
            break;
        }
        if (time(NULL) >= deadline) { timed_out = 1; break; }
    }
    close(fds[0]);
    if (timed_out) kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    if (timed_out) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* ---- libc-light helpers (keeps the static build small) ---- */

const char *err_str(int e) {
    switch (e) {
    case 2:  return "no such file or directory";       /* ENOENT */
    case 13: return "permission denied";               /* EACCES */
    case 17: return "file exists";                     /* EEXIST */
    case 22: return "invalid argument";                /* EINVAL */
    case 12: return "out of memory";                   /* ENOMEM */
    case 5:  return "i/o error";                       /* EIO */
    case 28: return "no space left on device";         /* ENOSPC */
    case 1:  return "operation not permitted";         /* EPERM */
    case 111: return "connection refused";             /* ECONNREFUSED */
    case 110: return "connection timed out";           /* ETIMEDOUT */
    default: return "error";
    }
}

void stamp_fmt(char *dst, int n, const char *fmt, const struct tm *t) {
    int o = 0;
    const char *p = fmt;
    while (*p && o < n - 1) {
        if (*p == '%' && p[1]) {
            p++;
            int v = 0, w = 2;
            switch (*p) {
            case 'Y': v = t->tm_year + 1900; w = 4; break;
            case 'm': v = t->tm_mon + 1; break;
            case 'd': v = t->tm_mday; break;
            case 'H': v = t->tm_hour; break;
            case 'M': v = t->tm_min; break;
            case 'S': v = t->tm_sec; break;
            default:
                dst[o++] = *p;
                p++;
                continue;
            }
            o += snprintf(dst + o, (size_t)(n - o), "%0*d", w, v);
            p++;
        } else {
            dst[o++] = *p++;
        }
    }
    dst[o < n ? o : n - 1] = '\0';
}

int scan_hhmm(const char *s, int *hh, int *mm) {
    int h = 0, m = 0;
    if (!s || s[0] < '0' || s[0] > '9') return -1;
    while (*s >= '0' && *s <= '9') { h = h * 10 + (*s++ - '0'); if (h > 99) return -1; }
    if (*s++ != ':') return -1;
    if (*s < '0' || *s > '9') return -1;
    while (*s >= '0' && *s <= '9') { m = m * 10 + (*s++ - '0'); if (m > 99) return -1; }
    if (*s != '\0' || h > 23 || m > 59) return -1;
    *hh = h;
    *mm = m;
    return 0;
}

double scan_num(const char **p) {
    const char *s = *p;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    double v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (*s == '.') {
        double f = 1;
        s++;
        while (*s >= '0' && *s <= '9') { f /= 10; v += (*s++ - '0') * f; }
    }
    if (*s == 'e' || *s == 'E') {
        int en = 0, es = 1;
        s++;
        if (*s == '+') s++;
        else if (*s == '-') { es = -1; s++; }
        while (*s >= '0' && *s <= '9') en = en * 10 + (*s++ - '0');
        while (en-- > 0) v *= (es > 0 ? 10 : 0.1);
    }
    *p = s;
    return neg ? -v : v;
}

void cursor_save(const char *path, const char *value) {
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (write_file(tmp, value) == 0) rename(tmp, path);
}

char *cursor_load(const char *path) {
    return read_file(path);
}
