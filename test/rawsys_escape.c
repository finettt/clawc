/* test/rawsys_escape.c - jail test fixture: an escape that bypasses libc
 * interposition (LD_PRELOAD cannot see raw syscalls).  Only the clawc jail's
 * syscall tripwire (tools/jaild.c) can stop this.  The suite compiles it to
 * /tmp/clawc-rawsys-escape and asks the mock LLM to run it. */
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(void)
{
    int fd = (int)syscall(SYS_openat, AT_FDCWD, "/home/user/landlock-canary",
                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, "pwned\n", 6); return 0; }
    return 1;
}
