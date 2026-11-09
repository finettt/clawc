/* Verify all packed strings, including embedded NULs, across concurrent calls. */
#include "../src/packed_text.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#define TEXT(id, text) static const char expected_##id[] = text;
#include "../src/packed_text.def"
#undef TEXT
struct item { int id; const char *text; size_t len; };
static const struct item expected[] = {
#define TEXT(id, text) {id, expected_##id, sizeof expected_##id},
#include "../src/packed_text.def"
#undef TEXT
};
static int mismatch;
static void *check(void *unused) {
    size_t i, pass; (void)unused;
    for (pass=0; pass<100; pass++) for (i=0; i<sizeof expected/sizeof expected[0]; i++)
        if (memcmp(packed_text(expected[i].id), expected[i].text, expected[i].len)) return &mismatch;
    return NULL;
}
int main(void) {
    pthread_t threads[12]; int i; void *result;
    if (sizeof expected/sizeof expected[0] != PACKED_TEXT_COUNT) return 1;
    for (i=0; i<12; i++) if (pthread_create(&threads[i],NULL,check,NULL)) return 1;
    for (i=0; i<12; i++) if (pthread_join(threads[i],&result) || result) return 1;
    puts("ok: packed text matches every original byte across threads");
    return 0;
}
