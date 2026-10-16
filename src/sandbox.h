/* clawc - sandbox.h: OS-enforced process confinement.
 *
 * Backends, per platform (all real kernel enforcement - never a no-op):
 *   Linux   - Landlock LSM (kernel >= 5.13, ABI 1-4 probed at runtime).
 *             Read/exec on the filesystem root, write only inside the
 *             workspace and /tmp, read on /proc, read-write on /dev.
 *   macOS   - sandbox_init(3) (seatbelt): read/exec everywhere, write only
 *             in the workspace, /tmp (/private/tmp) and /dev.  Full network
 *             access - the LLM traffic lives there.
 *   OpenBSD - unveil(2) path map + pledge(2) syscall set: same policy.
 *   others  - no backend exists (Windows, FreeBSD, exotic POSIX).
 *
 * Policy (main.c enforces it): when OS confinement is requested
 * ("sandbox": "os", alias "landlock", or CLAWC_SANDBOX=os), a missing or
 * failed backend is a STARTUP ERROR - clawc refuses to run tools rather than
 * degrade silently.  On such platforms use the explicit path sandbox
 * (-x DIR / "sandbox": "/dir") instead; run_shell is only truly contained by
 * an OS backend.
 *
 * All backends are permanent for the process lifetime and inherited by
 * children (run_shell forks keep the confinement).  Build with
 * SANDBOX_STUBS=1 to compile the no-backend branch anywhere (the test suite
 * uses it to verify the hard refusal).
 */
#ifndef CLAWC_SANDBOX_H
#define CLAWC_SANDBOX_H

/* Confine the current process; ws (may be NULL) additionally gets write
   access.  Returns 0 if a backend is active, -1 if unavailable/failed. */
int sandbox_apply(const char *ws);

/* 1 if this build+platform+kernel has an OS sandbox backend. */
int sandbox_available(void);

/* "landlock" | "sandbox_init" | "pledge" | "none" - for doctor/logs. */
const char *sandbox_backend(void);

#endif
