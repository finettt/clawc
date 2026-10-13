/* clawc - tools.c */
#define _XOPEN_SOURCE 700
#include "tools.h"

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/time.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "http.h"
#include "jail.h"
#include "bridge_tls.h"

#define SHELL_CAP  (32 * 1024)
#define READ_CAP   (64 * 1024)

typedef struct {
    const char *name;
    const char *desc;
    const char *schema;
} ToolDef;

static const ToolDef TOOL_DEFS[] = {
    {"run_shell",
     "Run a shell command; return combined stdout/stderr.",
     "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}"},
    {"read_file",
     "Read a text file; optional 1-based offset/limit selects lines.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"path\"]}"},
    {"write_file",
     "Create or overwrite a text file.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}"},
    {"list_dir",
     "List a directory.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}"},
    {"remember",
     "Append a fact to MEMORY.md.",
     "{\"type\":\"object\",\"properties\":{\"fact\":{\"type\":\"string\"}},\"required\":[\"fact\"]}"},
    {"search_memory",
     "Search long-term memory for lines containing a query.",
     "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}"},
    {"load_skill",
     "Load a skill's full text by name (list in system prompt).",
     "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}"},
    {"fetch_url",
     "GET a URL over HTTP(S); return the body (truncated).",
     "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}"},
    {"write_skill",
     "Save a procedure as skills/<name>/SKILL.md (load_skill reads it).",
     "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},"
     "\"description\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},"
     "\"required\":[\"name\",\"description\",\"content\"]}"},
    {"reflect",
     "Record a durable lesson to LESSONS.md (guides all future behavior).",
     "{\"type\":\"object\",\"properties\":{\"lesson\":{\"type\":\"string\"}},"
     "\"required\":[\"lesson\"]}"},
#ifndef CLAWC_NO_SERVE
    {"a2a",
     "Ask another clawc agent (peer) to do a task and return its reply. Peers "
     "are listed in your prompt; peer is a name or host:port.",
     "{\"type\":\"object\",\"properties\":{\"peer\":{\"type\":\"string\"},"
     "\"task\":{\"type\":\"string\"}},\"required\":[\"peer\",\"task\"]}"},
#endif
    {"goal_add",
     "Add a goal to GOALS.md (autonomous task list). Optional due, priority.",
     "{\"type\":\"object\",\"properties\":{\"task\":{\"type\":\"string\"},"
     "\"due\":{\"type\":\"string\"},\"priority\":{\"type\":\"string\"}},"
     "\"required\":[\"task\"]}"},
    {"goal_done",
     "Mark a goal complete in GOALS.md by its id (e.g. g3).",
     "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},"
     "\"required\":[\"id\"]}"},
};
#define NTOOLS ((int)(sizeof TOOL_DEFS / sizeof TOOL_DEFS[0]))

int tools_count(void) { return NTOOLS; }
const char *tools_name(int i) { return (i >= 0 && i < NTOOLS) ? TOOL_DEFS[i].name : NULL; }
const char *tools_desc(int i) { return (i >= 0 && i < NTOOLS) ? TOOL_DEFS[i].desc : NULL; }
const char *tools_schema(int i) { return (i >= 0 && i < NTOOLS) ? TOOL_DEFS[i].schema : NULL; }

const char *tools_openai_json(void) {
    static char *cached = NULL;
    if (cached) return cached;
    Buf b;
    buf_init(&b);
    buf_puts(&b, "[");
    int i;
    for (i = 0; i < NTOOLS; i++) {
        if (i) buf_puts(&b, ",");
        buf_printf(&b, "{\"type\":\"function\",\"function\":{\"name\":\"%s\",\"description\":",
                   TOOL_DEFS[i].name);
        json_quote(&b, TOOL_DEFS[i].desc);
        buf_printf(&b, ",\"parameters\":%s}}", TOOL_DEFS[i].schema);
    }
    buf_puts(&b, "]");
    cached = b.s;
    return cached;
}

const char *tools_anthropic_json(void) {
    static char *cached = NULL;
    if (cached) return cached;
    Buf b;
    buf_init(&b);
    buf_puts(&b, "[");
    int i;
    for (i = 0; i < NTOOLS; i++) {
        if (i) buf_puts(&b, ",");
        buf_printf(&b, "{\"name\":\"%s\",\"description\":", TOOL_DEFS[i].name);
        json_quote(&b, TOOL_DEFS[i].desc);
        buf_printf(&b, ",\"input_schema\":%s}", TOOL_DEFS[i].schema);
    }
    buf_puts(&b, "]");
    cached = b.s;
    return cached;
}

/* ---- path sandboxing ---- */

static const char *arg_str(const JVal *args, const char *key) {
    return json_get_str(args, key);
}

static char *safe_path(const ToolsCtx *ctx, const char *path, int must_exist) {
    char *joined;
    if (!ctx->sandbox || ctx->os_sandbox) {
        /* os_sandbox = OS-enforced mode (src/sandbox.c: landlock, sandbox_init
           or pledge/unveil): the kernel confines writes to workspace + /tmp +
           /dev, so no dir-prefix check needed here.  main.c refuses to start
           with os_sandbox set unless a real backend took effect. */
        /* no sandbox: absolute paths as-is, relative paths anchored at the workspace */
        if (path[0] == '/') return xstrdup(path);
        return path_join(ctx->workspace ? ctx->workspace : ".", path[0] ? path : ".");
    }
    if (path[0] == '/') joined = xstrdup(path);
    else joined = path_join(ctx->sandbox, path);
    /* Reject obvious escapes; realpath gives the canonical answer when it exists. */
    if (strstr(joined, "/../")) { free(joined); return NULL; }
    char real[4096];
    size_t sbn = strlen(ctx->sandbox);
    /* Symlinks are not followed at all in path-sandbox mode: lstat every
       component at or under the sandbox root and reject if any is a link.
       This closes the "ln -s /etc/passwd inside the workspace" trick even
       before the realpath containment check below runs. */
    {
        struct stat st;
        char probe[4096];
        size_t jn = strlen(joined);
        if (jn >= sizeof probe) { free(joined); return NULL; }
        if (strncmp(joined, ctx->sandbox, sbn) == 0
            && (joined[sbn] == 0 || joined[sbn] == '/')) {
            char *q;
            memcpy(probe, joined, jn + 1);
            if (lstat(probe, &st) == 0 && S_ISLNK(st.st_mode)) { free(joined); return NULL; }
            for (q = probe + sbn; *q; q++) {
                if (*q == '/') {
                    *q = 0;
                    if (lstat(probe, &st) == 0 && S_ISLNK(st.st_mode)) {
                        free(joined);
                        return NULL;
                    }
                    *q = '/';
                }
            }
        }
    }
#define INSIDE(p) (strncmp((p), ctx->sandbox, sbn) == 0 && ((p)[sbn] == 0 || (p)[sbn] == '/'))
    if (realpath(joined, real)) {
        if (!INSIDE(real)) { free(joined); return NULL; }
        free(joined);
        return xstrdup(real);
    }
    if (must_exist) { free(joined); return NULL; }
    /* File may not exist yet (writes): the parent directory must be inside. */
    char *slash = strrchr(joined, '/');
    char *base = slash ? slash + 1 : joined;
    char parent[4096];
    if (!base[0]) { free(joined); return NULL; }
    if (!slash) snprintf(parent, sizeof parent, ".");
    else if (slash == joined) snprintf(parent, sizeof parent, "/");
    else {
        size_t plen = (size_t)(slash - joined);
        if (plen >= sizeof parent) { free(joined); return NULL; }
        memcpy(parent, joined, plen);
        parent[plen] = 0;
    }
    if (!realpath(parent, real) || !INSIDE(real)) { free(joined); return NULL; }
#undef INSIDE
    char *out = path_join(real, base);
    free(joined);
    return out;
}

/* ---- individual tools ---- */

/* O_NOFOLLOW write for user-supplied paths: even if a symlink is swapped in
   between safe_path and the open, the kernel refuses the final hop (ELOOP). */
static int write_file_nf(const char *p, const char *content) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    size_t len = strlen(content), off = 0;
    if (fd < 0) return -1;
    while (off < len) {
        ssize_t w = write(fd, content + off, len - off);
        if (w <= 0) { close(fd); return -1; }
        off += (size_t)w;
    }
    return close(fd) == 0 ? 0 : -1;
}

static void tool_run_shell(const ToolsCtx *ctx, const JVal *args, Buf *out) {
    const char *cmd = arg_str(args, "command");
    if (!cmd) { buf_puts(out, "error: missing 'command'"); return; }
    fprintf(stderr, "[tool] run_shell: %s\n", cmd);
    Buf b;
    buf_init(&b);
    int rc = jail_armed() ? jail_run(cmd, ctx->timeout_s, &b, SHELL_CAP)
                          : proc_run(cmd, ctx->timeout_s, &b, SHELL_CAP);
    if (b.len == 0) buf_puts(&b, "(no output)");
    if (rc == -1) buf_printf(out, "[timeout or signal]\n%s", b.s);
    else buf_printf(out, "[exit %d]\n%s", rc, b.s);
    buf_free(&b);
}

static void tool_read_file(const ToolsCtx *ctx, const JVal *args, Buf *out) {
    const char *path = arg_str(args, "path");
    if (!path) { buf_puts(out, "error: missing 'path'"); return; }
    char *p = safe_path(ctx, path, 1);
    if (!p) { buf_puts(out, "error: path not allowed or not found"); return; }
    char *data = read_file(p);
    free(p);
    if (!data) { buf_puts(out, "error: cannot read file"); return; }
    long offset = (long)json_get_num(args, "offset", 1);
    long limit = (long)json_get_num(args, "limit", 2000);
    if (offset < 1) offset = 1;
    if (limit < 1) limit = 2000;
    fprintf(stderr, "[tool] read_file: %s\n", path);
    long lineno = 1;
    char *s = data;
    while (*s && lineno < offset) {
        if (*s == '\n') lineno++;
        s++;
    }
    long printed = 0;
    while (*s && printed < limit && out->len < READ_CAP) {
        char *nl = strchr(s, '\n');
        size_t n = nl ? (size_t)(nl - s) + 1 : strlen(s);
        buf_add(out, s, n);
        s += n;
        printed++;
    }
    if (!*s && printed == 0) buf_puts(out, "(empty or past end)");
    free(data);
}

static void tool_write_file(const ToolsCtx *ctx, const JVal *args, Buf *out) {
    const char *path = arg_str(args, "path");
    const char *content = arg_str(args, "content");
    if (!path || !content) { buf_puts(out, "error: need 'path' and 'content'"); return; }
    char *p = safe_path(ctx, path, 0);
    if (!p) { buf_puts(out, "error: path not allowed"); return; }
    fprintf(stderr, "[tool] write_file: %s (%zu bytes)\n", path, strlen(content));
    if (write_file_nf(p, content) != 0) buf_printf(out, "error: cannot write %s", path);
    else buf_printf(out, "wrote %zu bytes to %s", strlen(content), path);
    free(p);
}

static void tool_list_dir(const ToolsCtx *ctx, const JVal *args, Buf *out) {
    const char *path = arg_str(args, "path");
    if (!path || !path[0] || str_eq(path, ".")) path = ".";
    char *p = safe_path(ctx, path, 1);
    if (!p) { buf_puts(out, "error: path not allowed or not found"); return; }
    DIR *d = opendir(p);
    free(p);
    if (!d) { buf_puts(out, "error: cannot open directory"); return; }
    fprintf(stderr, "[tool] list_dir: %s\n", path);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (str_eq(e->d_name, ".") || str_eq(e->d_name, "..")) continue;
        buf_printf(out, "%s\n", e->d_name);
    }
    closedir(d);
    if (out->len == 0) buf_puts(out, "(empty)");
}

static void tool_reflect(const ToolsCtx *ctx, const JVal *args, Buf *out) {
    const char *lesson = arg_str(args, "lesson");
    if (!lesson) { buf_puts(out, "error: missing 'lesson'"); return; }
    char *mp = path_join(ctx->workspace, "LESSONS.md");
    Buf entry;
    buf_init(&entry);
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char stamp[32];
    stamp_fmt(stamp, sizeof stamp, "%Y-%m-%d %H:%M", &tmv);
    buf_printf(&entry, "- [%s] %s\n", stamp, lesson);
    char *old = read_file(mp);
    Buf all;
    buf_init(&all);
    if (old) {
        if (strlen(old) > 8192) {
            buf_puts(out, "error: LESSONS.md is full - consolidate the lessons "
                          "with write_file into a tighter set first");
            free(old);
            buf_free(&entry);
            buf_free(&all);
            free(mp);
            return;
        }
        buf_puts(&all, old);
        free(old);
    }
    buf_puts(&all, entry.s);
    int rc = write_file(mp, all.s);
    fprintf(stderr, "[tool] reflect -> LESSONS.md\n");
    if (rc != 0) buf_puts(out, "error: cannot write LESSONS.md");
    else buf_printf(out, "lesson recorded: %s", lesson);
    buf_free(&entry);
    buf_free(&all);
    free(mp);
}

static void tool_write_skill(const ToolsCtx *ctx, const JVal *args, Buf *out) {
    const char *name = arg_str(args, "name");
    const char *desc = arg_str(args, "description");
    const char *content = arg_str(args, "content");
    if (!name || !desc || !content) {
        buf_puts(out, "error: write_skill needs name, description and content");
        return;
    }
    /* strict name: no path tricks */
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > 32) { buf_puts(out, "error: invalid skill name"); return; }
    size_t i;
    for (i = 0; i < nlen; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            buf_puts(out, "error: invalid skill name (use lowercase letters, digits, - or _)");
            return;
        }
    }
    if (strlen(content) > 8192) {
        buf_puts(out, "error: skill content too long (max 8KB)");
        return;
    }
    char *sd = path_join(ctx->workspace, "skills");
    char *sk = path_join(sd, name);
    mkdir(sd, 0755);
    mkdir(sk, 0755);
    char *mp = path_join(sk, "SKILL.md");
    Buf body;
    buf_init(&body);
    buf_printf(&body, "# %s - %s\n\n%s\n", name, desc, content);
    int rc = write_file(mp, body.s);
    fprintf(stderr, "[tool] write_skill -> skills/%s/SKILL.md\n", name);
    if (rc != 0) buf_puts(out, "error: cannot write skill file");
    else buf_printf(out, "skill saved: skills/%s/SKILL.md (load it with load_skill)", name);
    buf_free(&body);
    free(mp);
    free(sk);
    free(sd);
}

#ifndef CLAWC_NO_SERVE
/* Only configured aliases may receive the bearer credential. In particular,
   model-supplied host:port values must never redirect the token to an attacker. */
static int a2a_resolve(const ToolsCtx *ctx, const char *peer,
                       char *host, size_t hn, int *port) {
    const char *p = ctx->a2a_peers;
    size_t nl = strlen(peer);
    while (p && *p) {
        while (*p == ' ' || *p == ',') p++;
        const char *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        while (end > p && end[-1] == ' ') end--;
        if ((size_t)(end - p) > nl + 1 && !strncmp(p, peer, nl) && p[nl] == '=') {
            const char *start = p + nl + 1, *colon = NULL, *q;
            for (q = start; q < end; q++) if (*q == ':') colon = q;
            if (!colon || colon == start || (size_t)(colon - start) >= hn) return -1;
            memcpy(host, start, (size_t)(colon - start)); host[colon - start] = 0;
            *port = 0;
            if (colon + 1 == end) return -1;
            for (q = colon + 1; q < end; q++) {
                if (*q < '0' || *q > '9' || *port > 6553) return -1;
                *port = *port * 10 + (*q - '0');
            }
            return *port > 0 && *port <= 65535 ? 0 : -1;
        }
        p = *end == ',' ? end + 1 : (*end ? strchr(end, ',') : NULL);
    }
    return -1;
}

static void tool_a2a(const ToolsCtx *ctx, const JVal *args, Buf *out) {
    signal(SIGPIPE, SIG_IGN); /* peer disconnect must not terminate the agent */
    const char *peer = arg_str(args, "peer");
    const char *task = arg_str(args, "task");
    if (!peer || !task || !task[0]) {
        buf_puts(out, "error: a2a needs peer and task");
        return;
    }
    if (!ctx->bridge_token || strlen(ctx->bridge_token) != 64) {
        buf_puts(out, "error: a2a needs bridge_token (64 hex characters)");
        return;
    }
#ifndef CLAWC_TLS
    if (ctx->a2a_tls) {
        buf_puts(out, "error: peer TLS unavailable in TLS=0 build");
        return;
    }
#endif
    char host[256];
    int port = 0;
    if (a2a_resolve(ctx, peer, host, sizeof host, &port) != 0) {
        buf_printf(out, "error: unknown peer %s (configure a2a_peers)", peer);
        return;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        buf_printf(out, "error: peer %s unreachable", peer);
        return;
    }
    int fd = -1;
    struct addrinfo *ai;
    for (ai = res; ai; ai = ai->ai_next) {
        if (!ctx->a2a_tls && !bridge_loopback(ai->ai_addr)) continue;
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) { close(fd); fd = -1; continue; }
        int cr = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (cr != 0 && errno == EINPROGRESS) {
            struct pollfd pfd = {fd, POLLOUT, 0};
            int soerr = 0; socklen_t slen = sizeof soerr;
            cr = poll(&pfd, 1, 5000) > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0 && !soerr ? 0 : -1;
        }
        if (cr == 0 && fcntl(fd, F_SETFL, flags) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        buf_printf(out, "error: peer %s unreachable (plaintext is loopback-only)", peer);
        return;
    }
    BridgeTls *tls = NULL;
    if (ctx->a2a_tls) {
        tls = bridge_tls_connect(fd, host, ctx->a2a_ca, 5);
        if (!tls) {
            close(fd);
            buf_puts(out, "error: peer TLS verification or handshake failed");
            return;
        }
    } else {
        struct timeval tv = {5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    Buf req;
    buf_init(&req);
    buf_puts(&req, "{\"text\":");
    json_quote(&req, task);
    buf_puts(&req, ",\"from\":\"clawc\",\"token\":");
    json_quote(&req, ctx->bridge_token);
    buf_puts(&req, "}\n");
    fprintf(stderr, "[a2a] -> %s:%d %.80s\n", host, port, task);
    int sent = 0;
    if (tls) sent = bridge_tls_write(tls, req.s, req.len, 5) == 0;
    else {
        size_t off = 0;
        while (off < req.len) {
            ssize_t n = send(fd, req.s + off, req.len - off, 0);
            if (n <= 0) break;
            off += (size_t)n;
        }
        sent = off == req.len;
    }
    buf_free(&req);
    if (!sent) { bridge_tls_free(tls); close(fd); buf_puts(out, "error: peer write failed"); return; }
    Buf resp;
    buf_init(&resp);
    char ch;
    time_t deadline = time(NULL) + 30;
    while (resp.len < 65536 && time(NULL) < deadline) {
        ssize_t nr = tls ? bridge_tls_read(tls, &ch, 1, 5) : recv(fd, &ch, 1, 0);
        if (nr != 1 || ch == '\n') break;
        buf_add(&resp, &ch, 1);
    }
    bridge_tls_free(tls);
    close(fd);
    JVal *root = json_parse(resp.s);
    const char *reply = root ? json_get_str(root, "reply") : NULL;
    const char *err = root ? json_get_str(root, "error") : NULL;
    if (reply && reply[0]) buf_printf(out, "%.4000s", reply);
    else if (err && str_eq(err, "unauthorized")) buf_puts(out, "error: peer authentication failed");
    else buf_puts(out, "error: peer sent an unreadable reply");
    json_free(root);
    buf_free(&resp);
}
#endif

static void tool_goal_add(const ToolsCtx *ctx, const JVal *args, Buf *out) {
    goal_add_text(ctx, arg_str(args, "task"), arg_str(args, "due"),
                  arg_str(args, "priority"), out);
}

int goal_add_text(const ToolsCtx *ctx, const char *task, const char *due,
                  const char *prio, Buf *out) {
    if (!task || !task[0]) { buf_puts(out, "error: missing task"); return -1; }
    char *gp = path_join(ctx->workspace, "GOALS.md");
    char *old = read_file(gp);
    Buf all;
    buf_init(&all);
    if (old) { buf_puts(&all, old); free(old); }
    else buf_puts(&all, "# Goals\n\n");
    /* next id: highest existing gN + 1 */
    int maxid = 0;
    {
        const char *p = all.s;
        while ((p = strstr(p, "(g")) != NULL) {
            int v = atoi(p + 2);
            if (v > maxid) maxid = v;
            p += 2;
        }
    }
    Buf entry;
    buf_init(&entry);
    buf_printf(&entry, "- [ ] (g%d) %s", maxid + 1, task);
    if (due && due[0]) buf_printf(&entry, " (due: %s)", due);
    if (prio && prio[0]) buf_printf(&entry, " [priority: %s]", prio);
    buf_puts(&entry, "\n");
    buf_puts(&all, entry.s);
    int rc = write_file(gp, all.s);
    fprintf(stderr, "[tool] goal_add -> GOALS.md\n");
    if (rc != 0) buf_puts(out, "error: cannot write GOALS.md");
    else buf_printf(out, "goal added: (g%d) %s", maxid + 1, task);
    buf_free(&entry);
    buf_free(&all);
    free(gp);
    return rc;
}

static void tool_goal_done(const ToolsCtx *ctx, const JVal *args, Buf *out) {
    goal_done_text(ctx, arg_str(args, "id"), out);
}

int goal_done_text(const ToolsCtx *ctx, const char *id, Buf *out) {
    if (!id || !id[0]) { buf_puts(out, "error: missing id"); return -1; }
    while (*id == ' ') id++;
    char *gp = path_join(ctx->workspace, "GOALS.md");
    char *old = read_file(gp);
    if (!old) {
        buf_puts(out, "error: no GOALS.md");
        free(gp);
        return -1;
    }
    /* replace "- [ ] (id)" with "- [x] (id) [done <stamp>]" once */
    Buf all;
    buf_init(&all);
    char needle[48];
    snprintf(needle, sizeof needle, "- [ ] (%s)", id);
    char *hit = strstr(old, needle);
    if (!hit) {
        buf_printf(out, "error: no open goal with id %s", id);
        free(old);
        buf_free(&all);
        free(gp);
        return -1;
    }
    size_t pre = (size_t)(hit - old);
    buf_add(&all, old, pre);
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char stamp[32];
    stamp_fmt(stamp, sizeof stamp, "%Y-%m-%d", &tmv);
    buf_printf(&all, "- [x] (%s) [done %s]", id, stamp);
    buf_puts(&all, hit + strlen(needle));
    int rc = write_file(gp, all.s);
    fprintf(stderr, "[tool] goal_done -> GOALS.md\n");
    if (rc != 0) buf_puts(out, "error: cannot write GOALS.md");
    else buf_printf(out, "goal %s completed", id);
    free(old);
    buf_free(&all);
    free(gp);
    return rc;
}

int goal_rm_text(const ToolsCtx *ctx, const char *id, Buf *out) {
    if (!id || !id[0]) { buf_puts(out, "error: missing id"); return -1; }
    while (*id == ' ') id++;
    char *gp = path_join(ctx->workspace, "GOALS.md");
    char *old = read_file(gp);
    if (!old) { buf_puts(out, "error: no GOALS.md"); free(gp); return -1; }
    char needle[48];
    snprintf(needle, sizeof needle, "(%s)", id);
    char *hit = strstr(old, needle);
    if (!hit) {
        buf_printf(out, "error: no goal with id %s", id);
        free(old); free(gp); return -1;
    }
    /* remove the whole line containing (id) */
    char *line = hit;
    while (line > old && line[-1] != '\n') line--;
    char *eol = strchr(hit, '\n');
    Buf all;
    buf_init(&all);
    buf_add(&all, old, (size_t)(line - old));
    if (eol) buf_puts(&all, eol + 1);
    write_file(gp, all.s);
    fprintf(stderr, "[tool] goal_rm -> GOALS.md\n");
    buf_printf(out, "goal %s removed", id);
    buf_free(&all);
    free(old);
    free(gp);
    return 0;
}

static void tool_remember(const ToolsCtx *ctx, const JVal *args, Buf *out) {
    const char *fact = arg_str(args, "fact");
    if (!fact) { buf_puts(out, "error: missing 'fact'"); return; }
    char *mp = path_join(ctx->workspace, "MEMORY.md");
    Buf entry;
    buf_init(&entry);
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char stamp[32];
    stamp_fmt(stamp, sizeof stamp, "%Y-%m-%d %H:%M", &tmv);
    buf_printf(&entry, "- [%s] %s\n", stamp, fact);
    char *old = read_file(mp);
    Buf all;
    buf_init(&all);
    if (old) { buf_puts(&all, old); free(old); }
    buf_puts(&all, entry.s);
    int rc = write_file(mp, all.s);
    fprintf(stderr, "[tool] remember -> MEMORY.md\n");
    if (rc != 0) buf_puts(out, "error: cannot write MEMORY.md");
    else buf_printf(out, "remembered: %s", fact);
    buf_free(&entry);
    buf_free(&all);
    free(mp);
}

static void search_one(const char *path, const char *label, const char *q,
                       Buf *out, int *hits) {
    char *data = read_file(path);
    if (!data) return;
    long lineno = 1;
    char *s = data;
    while (*s && *hits < 24) {
        char *nl = strchr(s, '\n');
        size_t n = nl ? (size_t)(nl - s) : strlen(s);
        char save = s[n];
        s[n] = 0;
        if (strstr(s, q)) {
            buf_printf(out, "%s:%ld: %.220s\n", label, lineno, s);
            (*hits)++;
        }
        s[n] = save;
        if (!nl) break;
        s = nl + 1;
        lineno++;
    }
    free(data);
}

static void tool_search_memory(const ToolsCtx *ctx, const JVal *args, Buf *out) {
    const char *q = arg_str(args, "query");
    if (!q) { buf_puts(out, "error: missing 'query'"); return; }
    fprintf(stderr, "[tool] search_memory: %s\n", q);
    int hits = 0, i;
    const char *names[] = { "MEMORY.md", "LESSONS.md", "GOALS.md" };
    for (i = 0; i < 3; i++) {
        char *p = path_join(ctx->workspace, names[i]);
        search_one(p, names[i], q, out, &hits);
        free(p);
    }
    {
        char *hd = path_join(ctx->workspace, "history");
        DIR *d = opendir(hd);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL && hits < 24) {
                size_t l = strlen(e->d_name);
                if (l < 6 || strcmp(e->d_name + l - 6, ".jsonl") != 0) continue;
                char *p = path_join(hd, e->d_name);
                search_one(p, e->d_name, q, out, &hits);
                free(p);
            }
            closedir(d);
        }
        free(hd);
    }
    if (!hits) buf_printf(out, "no memory lines contain \"%s\"", q);
}

static void tool_load_skill(const ToolsCtx *ctx, const JVal *args, Buf *out) {
    const char *name = arg_str(args, "name");
    if (!name) { buf_puts(out, "error: missing 'name'"); return; }
    char *dir = path_join(ctx->workspace, "skills");
    char *a = path_join(dir, name);
    Buf p1;
    buf_init(&p1);
    buf_printf(&p1, "%s/SKILL.md", a);
    char *data = read_file(p1.s);
    if (!data) {
        Buf p2;
        buf_init(&p2);
        buf_printf(&p2, "%s.md", a);
        data = read_file(p2.s);
        buf_free(&p2);
    }
    fprintf(stderr, "[tool] load_skill: %s\n", name);
    if (!data) buf_printf(out, "error: no skill named '%s'", name);
    else {
        buf_puts(out, data);
        buf_puts(out, "\n(refine this skill with write_skill if you find a better "
                     "approach while using it)");
        free(data);
    }
    buf_free(&p1);
    free(a);
    free(dir);
}

static void tool_fetch_url(const ToolsCtx *ctx, const JVal *args, Buf *out) {
    const char *url = arg_str(args, "url");
    if (!url) { buf_puts(out, "error: missing 'url'"); return; }
    if (!str_starts(url, "http://") && !str_starts(url, "https://")) {
        buf_puts(out, "error: url must start with http:// or https://");
        return;
    }
    fprintf(stderr, "[tool] fetch_url: %s\n", url);
    int t = ctx->timeout_s > 30 ? ctx->timeout_s : 30;
    HttpResp *r = http_request("GET", url, NULL, 0, NULL, t, ctx->insecure_tls);
    if (r->status < 0) {
        buf_printf(out, "error: %s", r->err ? r->err : "request failed");
    } else {
        size_t n = r->body.len > 32 * 1024 ? 32 * 1024 : r->body.len;
        buf_printf(out, "HTTP %ld\n%.*s", r->status, (int)n, r->body.s);
        if (n < r->body.len) buf_puts(out, "\n[... truncated]");
    }
    http_resp_free(r);
}

int tools_is_risky(const char *name, const JVal *args) {
    if (str_eq(name, "run_shell")) {
        const char *cmd = arg_str(args, "command");
        if (!cmd) return 1; /* unknown command: treat as risky */
        static const char bad[][18] = {
            "rm -rf", "rm -fr", "rm -r /", "mkfs", "dd if=", "> /dev/", ":(){",
            "shutdown", "reboot", "halt", "poweroff", "kill -9 -1", "killall",
            "| sh", "| bash", "|sh", "|bash", "| zsh",
            "chmod -R 777", "chown -R", "git push --force", "git reset --hard",
            "DROP TABLE", "DROP DATABASE", "truncate ", ": > /", "eval ", ""
        };
        int i;
        for (i = 0; bad[i][0]; i++)
            if (strstr(cmd, bad[i])) return 1;
        return 0;
    }
    if (str_eq(name, "write_file")) {
        const char *p = arg_str(args, "path");
        if (!p) return 1;
        static const char syspaths[][15] = {
            "/etc/", "/usr/", "/bin/", "/sbin/", "/boot/", "/dev/", "/sys/",
            "/proc/", "/var/", "/root/", "/lib/",
#ifdef __APPLE__
            "/System/", "/Library/", "/private/", "/Applications/", "/Volumes/",
#endif
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
            "/stand/", "/rescue/",
#endif
            ""
        };
        int i;
        for (i = 0; syspaths[i][0]; i++)
            if (str_starts(p, syspaths[i])) return 1;
        return 0;
    }
    return 0;
}

int tools_exec(const ToolsCtx *ctx, const char *name, const JVal *args, Buf *out) {
    if (str_eq(name, "run_shell")) tool_run_shell(ctx, args, out);
    else if (str_eq(name, "read_file")) tool_read_file(ctx, args, out);
    else if (str_eq(name, "write_file")) tool_write_file(ctx, args, out);
    else if (str_eq(name, "list_dir")) tool_list_dir(ctx, args, out);
    else if (str_eq(name, "remember")) tool_remember(ctx, args, out);
    else if (str_eq(name, "reflect")) tool_reflect(ctx, args, out);
    else if (str_eq(name, "write_skill")) tool_write_skill(ctx, args, out);
#ifndef CLAWC_NO_SERVE
    else if (str_eq(name, "a2a")) tool_a2a(ctx, args, out);
#endif
    else if (str_eq(name, "goal_add")) tool_goal_add(ctx, args, out);
    else if (str_eq(name, "goal_done")) tool_goal_done(ctx, args, out);
    else if (str_eq(name, "search_memory")) tool_search_memory(ctx, args, out);
    else if (str_eq(name, "load_skill")) tool_load_skill(ctx, args, out);
    else if (str_eq(name, "fetch_url")) tool_fetch_url(ctx, args, out);
    else {
        buf_printf(out, "error: unknown tool '%s'", name ? name : "");
        return -1;
    }
    return 0;
}

/* ---- workspace bootstrap helpers ---- */

static char *ws_read(const ToolsCtx *ctx, const char *file, const char *dflt) {
    char *p = path_join(ctx->workspace, file);
    char *data = read_file(p);
    free(p);
    return data ? data : xstrdup(dflt);
}

char *tools_read_memory(const ToolsCtx *ctx) { return ws_read(ctx, "MEMORY.md", ""); }
char *tools_read_soul(const ToolsCtx *ctx) { return ws_read(ctx, "SOUL.md", ""); }

char *tools_skills_listing(const ToolsCtx *ctx) {
    Buf b;
    buf_init(&b);
    char *dir = path_join(ctx->workspace, "skills");
    DIR *d = opendir(dir);
    if (!d) { free(dir); return b.s; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char *sub = path_join(dir, e->d_name);
        Buf p;
        buf_init(&p);
        buf_printf(&p, "%s/SKILL.md", sub);
        char *data = read_file(p.s);
        if (!data) {
            buf_free(&p);
            buf_printf(&p, "%s.md", sub);
            data = read_file(p.s);
        }
        buf_free(&p);
        char *first = data ? data : "";
        char *nl = strchr(first, '\n');
        size_t n = nl ? (size_t)(nl - first) : strlen(first);
        if (n > 120) n = 120;
        buf_printf(&b, "- %.*s\n", (int)n, first);
        free(data);
        free(sub);
    }
    closedir(d);
    free(dir);
    return b.s;
}
