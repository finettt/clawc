/* clawc - cron.h: scheduled jobs kept in $workspace/cron.json.
   One-shots (at, every=0) fire once; recurring jobs (every>0) reschedule. */
#ifndef CLAWC_CRON_H
#define CLAWC_CRON_H

typedef struct {
    char id[32];
    char *task;     /* agent prompt */
    long long at;   /* next due, epoch seconds (64-bit: Y2038-safe) */
    long long every;/* repeat interval seconds; 0 = one-shot */
    int enabled;
} CronJob;

/* Load $ws/cron.json. Missing file = empty list (returns 0). -1 on parse error. */
int cron_load(const char *ws, CronJob **out, int *nout);
void cron_free(CronJob *jobs, int n);

/* Atomic save (tmp + rename). 0 on success. */
int cron_save(const char *ws, const CronJob *jobs, int n);

int  cron_due(const CronJob *j, long long now); /* enabled && at <= now */
void cron_advance(CronJob *j, long long now);   /* one-shot disables; recurring re-arms */

/* Next epoch for a wall-clock spec: "HH:MM" (any day) or plus a tm_wday
   (0=Sun..6=Sat) for a specific weekday. -1 on a bad spec. */
long long cron_next_clock(const char *hhmm, int weekday);
int  cron_weekday(const char *name);         /* "mon".."sun" -> 1..0; -1 bad */

/* 1 if the workspace has cron.json with at least one enabled job. */
int cron_active(const char *ws);

#endif
