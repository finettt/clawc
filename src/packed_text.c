/* Lossless decompression of built-in ASCII text. No external library. */
#include "packed_text.h"
#include <pthread.h>
#include <stddef.h>
#include "packed_text_data.h"
static unsigned char plain[PACKED_TEXT_LENGTH];
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void unpack(void) {
    size_t p=0, out=0;
    while (out < sizeof plain) {
        unsigned c=packed_bytes[p++];
        if (c<128) plain[out++]=(unsigned char)c;
        else {
            size_t len=((c>>4)&7)+3, dist=((c&15)<<8)+packed_bytes[p++]+1;
            while (len--) { plain[out]=plain[out-dist]; out++; }
        }
    }
}
const char *packed_text(int id) {
    pthread_once(&once, unpack);
    return (const char *)plain + packed_offsets[id];
}
