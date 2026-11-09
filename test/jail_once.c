/* Exit 126 is a valid command exit, not proof that jail setup failed. */
#include "../src/jail.h"
#include <stdio.h>
int main(int argc, char **argv) {
    Buf out;
    buf_init(&out);
    jail_arm("/tmp:/dev");
    char cmd[2048];
    const char *path = argc > 1 ? argv[1] : "/tmp/clawc-jail-once-output";
    if (snprintf(cmd, sizeof cmd, "echo once >> %s; exit 126", path) >= (int)sizeof cmd) return 2;
    int rc = jail_run(cmd, 10, &out, 8192);
    buf_free(&out);
    return rc == 126 ? 0 : 1;
}
