/* clawc - util.h: dynamic buffers, file IO, misc helpers. Pure C99 + POSIX. */
#ifndef CLAWC_UTIL_H
#define CLAWC_UTIL_H

#include <stddef.h>
#include <time.h>

#define CLAWC_VERSION "0.3.0"

typedef struct {
    char *s;      /* NUL-terminated data */
    size_t len;   /* strlen(s) */
    size_t cap;
} Buf;

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_add(Buf *b, const void *data, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...);
void buf_trim(Buf *b);

/* tiny libc-light helpers (keeps static builds small) */
const char *err_str(int e);              /* strerror without the tables */
void stamp_fmt(char *dst, int n, const char *fmt, const struct tm *t); /* strftime-lite */
int scan_hhmm(const char *s, int *hh, int *mm);   /* "HH:MM" -> 0 ok */
double scan_num(const char **p);         /* strtod-lite for JSON numbers */
/* atomic text cursor persistence (channels): tmp + rename */
void cursor_save(const char *path, const char *value);
char *cursor_load(const char *path);     /* malloc'd or NULL */

char *read_file(const char *path);          /* malloc'd NUL-terminated, or NULL */
int   write_file(const char *path, const char *data);
int   file_exists(const char *path);
int   is_dir(const char *path);

char *path_join(const char *a, const char *b);   /* malloc'd */
char *str_trim(char *s);                          /* in place, returns s */
int   str_starts(const char *s, const char *pfx);
int   str_ends(const char *s, const char *sfx);
int   str_eq(const char *a, const char *b);
int   csv_has(const char *list, const char *item); /* exact, non-empty membership */
int   sender_ready(const char *channel, const char *list);

/* Run cmd through /bin/sh, capture combined stdout+stderr into out (capped).
   Kills the process after timeout_s seconds. Returns exit code, or -1 on
   timeout/infra error (out is still filled with whatever was captured). */
int proc_run(const char *cmd, int timeout_s, Buf *out, size_t cap);
/* Damage limits for subprocesses (jail layer): call in the child before exec. */
void proc_limits(int timeout_s);

#endif
