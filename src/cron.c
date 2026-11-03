/* clawc - cron.c: schedule persistence and due-time math. */
#define _XOPEN_SOURCE 700
#include "cron.h"
#include "json.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static char *cron_path(const char *ws) { return path_join(ws, "cron.json"); }

int cron_load(const char *ws, CronJob **out, int *nout) {
    *out = NULL;
    *nout = 0;
    char *p = cron_path(ws);
    char *data = read_file(p);
    free(p);
    if (!data) return 0; /* no schedule yet */

    JVal *root = json_parse(data);
    free(data);
    if (!root || root->type != J_OBJ) {
        json_free(root);
        return -1;
    }
    JVal *jobs = json_obj_get(root, "jobs");
    if (jobs && jobs->type == J_ARR && jobs->nitems > 0) {
        CronJob *arr = xmalloc((size_t)jobs->nitems * sizeof *arr);
        int n = 0, i;
        for (i = 0; i < jobs->nitems; i++) {
            JVal *j = jobs->items[i];
            const char *id = json_get_str(j, "id");
            const char *task = json_get_str(j, "task");
            if (!id || !task) continue;
            snprintf(arr[n].id, sizeof arr[n].id, "%s", id);
            arr[n].task = xstrdup(task);
            arr[n].at = (long long)json_get_num(j, "at", 0);
            arr[n].every = (long long)json_get_num(j, "every", 0);
            JVal *en = json_obj_get(j, "enabled");
            arr[n].enabled = (en && en->type == J_BOOL) ? en->boolean
                            : (en && en->type == J_NUM) ? (int)en->num : 1;
            n++;
        }
        *out = arr;
        *nout = n;
    }
    json_free(root);
    return 0;
}

void cron_free(CronJob *jobs, int n) {
    int i;
    for (i = 0; i < n; i++) free(jobs[i].task);
    free(jobs);
}

int cron_save(const char *ws, const CronJob *jobs, int n) {
    char *p = cron_path(ws);
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\n  \"jobs\": [");
    int i;
    for (i = 0; i < n; i++) {
        buf_puts(&b, i ? ",\n    {" : "\n    {");
        buf_printf(&b, "\"id\": ");
        json_quote(&b, jobs[i].id);
        buf_printf(&b, ", \"task\": ");
        json_quote(&b, jobs[i].task);
        buf_printf(&b, ", \"at\": %lld, \"every\": %lld, \"enabled\": %s}",
                   jobs[i].at, jobs[i].every, jobs[i].enabled ? "true" : "false");
    }
    buf_puts(&b, n ? "\n  ]\n}\n" : "]\n}\n");

    char tmp[4600];
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    int rc = write_file(tmp, b.s);
    if (rc == 0 && rename(tmp, p) != 0) rc = -1;
    buf_free(&b);
    free(p);
    return rc;
}

int cron_due(const CronJob *j, long long now) {
    return j->enabled && j->at > 0 && j->at <= now;
}

void cron_advance(CronJob *j, long long now) {
    if (j->every > 0) {
        /* step forward by whole intervals: keeps the wall-clock anchor
           (a daily 09:00 job stays at 09:00 instead of drifting) */
        for (j->at += j->every; j->at <= now; j->at += j->every) {}
    } else {
        j->enabled = 0; /* one-shot: spent */
    }
}

int cron_weekday(const char *name) {
    static const char days[][4] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
    if (!name) return -1;
    int i;
    for (i = 0; i < 7; i++)
        if (strncasecmp(name, days[i], 3) == 0) return i;
    return -1;
}

long long cron_next_clock(const char *hhmm, int weekday) {
    int hh, mm;
    if (scan_hhmm(hhmm, &hh, &mm) != 0) return -1;
    time_t now = time(NULL);
    struct tm base;
    localtime_r(&now, &base);
    int d;
    for (d = 0; d <= 8; d++) {
        struct tm c = base;
        c.tm_mday += d;
        c.tm_hour = hh;
        c.tm_min = mm;
        c.tm_sec = 0;
        c.tm_isdst = -1;
        time_t cand = mktime(&c);
        if (cand < 0) continue;
        if (weekday >= 0 && c.tm_wday != weekday) continue;
        if (cand > now) return (long long)cand;
    }
    return -1;
}

int cron_active(const char *ws) {
    CronJob *jobs = NULL;
    int n = 0, i, active = 0;
    if (cron_load(ws, &jobs, &n) != 0) return 0;
    for (i = 0; i < n; i++)
        if (jobs[i].enabled) { active = 1; break; }
    cron_free(jobs, n);
    return active;
}
