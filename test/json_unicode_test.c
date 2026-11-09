/* Exhaustively verify JSON Unicode escape decoding, including standalone
   surrogate code units (the existing parser accepts and encodes those). */
#include "../src/json.h"
#include <stdio.h>
#include <string.h>

static int check(unsigned unit, unsigned low) {
    char input[32], expected[5];
    unsigned cp = unit;
    size_t n;
    if (low) {
        snprintf(input, sizeof input, "\"\\u%04x\\u%04x\"", unit, low);
        cp = 0x10000 + ((unit - 0xd800) << 10) + low - 0xdc00;
    } else snprintf(input, sizeof input, "\"\\u%04x\"", unit);
    if (cp < 0x80) { expected[0] = (char)cp; n = 1; }
    else if (cp < 0x800) {
        expected[0] = (char)(0xc0 | (cp >> 6)); expected[1] = (char)(0x80 | (cp & 63)); n = 2;
    } else if (cp < 0x10000) {
        expected[0] = (char)(0xe0 | (cp >> 12)); expected[1] = (char)(0x80 | ((cp >> 6) & 63));
        expected[2] = (char)(0x80 | (cp & 63)); n = 3;
    } else {
        expected[0] = (char)(0xf0 | (cp >> 18)); expected[1] = (char)(0x80 | ((cp >> 12) & 63));
        expected[2] = (char)(0x80 | ((cp >> 6) & 63)); expected[3] = (char)(0x80 | (cp & 63)); n = 4;
    }
    expected[n] = 0;
    JVal *v = json_parse(input);
    int ok = v && v->type == J_STR && v->str && strlen(v->str) == n && !memcmp(v->str, expected, n);
    json_free(v);
    return ok;
}
int main(void) {
    unsigned i, j;
    for (i = 1; i <= 0xffff; i++) if (!check(i, 0)) return 1;
    for (i = 0xd800; i <= 0xdbff; i++)
        for (j = 0xdc00; j <= 0xdfff; j++) if (!check(i, j)) return 2;
    JVal *nul = json_parse("\"\\u0000\"");
    if (nul) { json_free(nul); return 3; }
    puts("ok: Unicode escapes including all 1,048,576 surrogate pairs");
    return 0;
}
