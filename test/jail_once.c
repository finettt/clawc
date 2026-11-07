/* Exit 126 is a valid command exit, not proof that jail setup failed. */
#include "../src/jail.h"
int main(void) {
    Buf out;
    buf_init(&out);
    jail_arm("/tmp:/dev");
    int rc = jail_run("echo once >> /tmp/clawc-jail-once-output; exit 126", 10, &out, 8192);
    buf_free(&out);
    return rc == 126 ? 0 : 1;
}
