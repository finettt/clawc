/* clawc - history.h: append-only JSONL conversation log + audit log. */
#ifndef CLAWC_HISTORY_H
#define CLAWC_HISTORY_H

#include "llm.h"

/* Load messages from a JSONL file. Missing file is not an error (empty).
   Returns 0 on success. Caller frees *out (array of Msg) after msg_clearing
   each entry. Corrupt lines are skipped. */
int hist_load(const char *path, Msg **out, int *nout);

/* Append one message as a single JSON line (creates file/dir as needed).
   Returns 0 on success, -1 if the log cannot be opened or written. */
int  hist_append(const char *path, const Msg *m);

/* Truncate (clear) a history file. Returns 0 on success, -1 on failure. */
int  hist_truncate(const char *path);

/* Rewrite the whole log with n messages. Truncates first unless flag is set,
   in which case it appends. Returns 0 on success, -1 on failure. */
int hist_write(const char *path, const Msg *msgs, int n, int flag);

/* Append an audit record: ts, session, tool, args, result (truncated). */
void audit_log(const char *workspace, const char *session, const char *tool,
               const char *args, const char *result);

#endif
