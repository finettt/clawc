/* test/fuzz_json.c - dumb mutational fuzzer for src/json.c (the parser that
 * ingests all external input: Telegram updates, LLM output, config, A2A).
 * Build:  cc -fsanitize=address,undefined -g -o /tmp/fuzz_json \
 *             test/fuzz_json.c src/json.c src/util.c
 * Run:    /tmp/fuzz_json [iterations]   (default 100000)
 * The corpus is every literal that appears in the product's tests plus
 * random garbage; mutations flip/insert/delete bytes.  Any crash, ASan
 * report or hang is a finding. */
#include "../src/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *seeds[] = {
    "{}", "[]", "{\"a\":1}", "{\"a\":[1,2,{\"b\":null}]}", "[[[[[]]]]]",
    "{\"text\":\"hi\",\"chat\":{\"id\":123}}",
    "{\"choices\":[{\"delta\":{\"content\":\"x\"},\"finish_reason\":\"stop\"}]}",
    "{\"content\":[{\"type\":\"text\",\"text\":\"y\"}],\"stop_reason\":\"end_turn\"}",
    "\"\\u00e4\\ud83d\\ude00\"", "1.5e-9", "-0", "true", "false", "null",
    "{\"a\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}",
    "{\"nested\":{\"nested\":{\"nested\":{\"nested\":[1,2,3,4,5]}}}}",
    "{\"id\":\"j1\",\"task\":\"x\",\"at\":1700000000,\"every\":60,\"enabled\":true}",
    "", " ", "\t\n", "{", "}", "[", "]", "{\"a\":", ":1", ",,", "\"\\", "0123456789"
};

static unsigned rng_state = 12345;
static unsigned rnd(void)
{
    rng_state = rng_state * 1103515245 + 12345;
    return (rng_state >> 16) & 0x7fff;
}

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 100000;
    char buf[8192];
    long i;
    int s;

    for (i = 0; i < iters; i++) {
        size_t len;
        JVal *v;
        if (i % 4 == 0) {
            /* mutate a seed */
            const char *seed = seeds[rnd() % (int)(sizeof seeds / sizeof seeds[0])];
            len = strlen(seed);
            if (len > sizeof buf - 2) len = sizeof buf - 2;
            memcpy(buf, seed, len);
            if (len > 0 && rnd() % 2) {
                int m, nm = 1 + rnd() % 4;
                for (m = 0; m < nm; m++) {
                    size_t pos = rnd() % len;
                    switch (rnd() % 3) {
                    case 0: buf[pos] = (char)(rnd() % 256); break;
                    case 1: buf[pos] = "{}[]\",\\:0123456789abcdefu tn"[rnd() % 28]; break;
                    default: memmove(buf + pos + 1, buf + pos, len - pos); len++; break;
                    }
                }
            }
        } else {
            /* random garbage */
            size_t j;
            len = rnd() % 64;
            for (j = 0; j < len; j++) buf[j] = (char)(rnd() % 256);
        }
        buf[len] = 0;
        v = json_parse(buf);
        if (v) {
            /* exercise the accessors too */
            json_obj_get(v, "a");
            json_get_str(v, "b");
            json_get_num(v, "c", 0);
            json_free(v);
        }
        if (i % 20000 == 0) printf("fuzz_json: %ld ok\n", i);
    }
    printf("fuzz_json: done, %ld iterations, no crashes\n", iters);
    return 0;
}
