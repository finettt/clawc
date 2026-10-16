/* clawc - best-effort userspace fallback, not a security boundary.
 * On Linux the helper uses kernel seccomp user notifications. It checks only
 * selected operations and has path races; the shim is bypassable. */
#ifndef CLAWC_JAIL_H
#define CLAWC_JAIL_H

#include "util.h"

/* Arm the jail with a colon-separated list of writable roots. */
void jail_arm(const char *roots);
int jail_armed(void);

/* Supervised drop-in for proc_run(): same signature/semantics. */
int jail_run(const char *cmd, int timeout_s, Buf *out, size_t cap);

#endif
