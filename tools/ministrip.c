#define _XOPEN_SOURCE 700
/* ministrip - drop ELF section headers (debug-only metadata) from a binary. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* Read a fixed-width integer in the ELF file's endianness (ELFDATA2MSB = 1). */
static uint64_t rd(const unsigned char *p, int n, int be) {
    uint64_t v = 0;
    if (be) {
        for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    } else {
        for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    }
    return v;
}

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) return 1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 64) { close(fd); return 1; }
    unsigned char h[64];
    if (read(fd, h, 64) != 64 || h[0] != 0x7f || h[1] != 'E' || h[4] != 2) { close(fd); return 1; }
    int be = (h[5] == 2);
    uint64_t phoff = rd(h + 32, 8, be);
    uint64_t phentsize = rd(h + 54, 2, be), phnum = rd(h + 56, 2, be);
    uint64_t size = (uint64_t)st.st_size;
    if (phnum == 0xffff) {
        /* PN_XNUM: the real program-header count lives in section header 0,
         * which we are about to drop; leave the file untouched and warn. */
        fprintf(stderr, "ministrip: extended program-header count, preserving %s\n", argv[1]);
        close(fd);
        return 0;
    }
    uint64_t maxend = 64;
    /* The program-header table is file-backed content too. */
    if (phoff + phnum * phentsize <= size && phoff + phnum * phentsize > maxend)
        maxend = phoff + phnum * phentsize;
    for (uint64_t i = 0; i < phnum; i++) {
        unsigned char ph[56];
        if (pread(fd, ph, 56, (off_t)(phoff + i * phentsize)) != 56) continue;
        uint64_t off = rd(ph + 8, 8, be), filesz = rd(ph + 32, 8, be);
        if (!filesz) continue;
        if (off > size || filesz > size - off) { close(fd); return 1; }
        if (off + filesz > maxend) maxend = off + filesz;
    }
    unsigned char z[8] = {0};
    if (pwrite(fd, z, 8, 40) != 8 || pwrite(fd, z, 2, 60) != 2 || pwrite(fd, z, 2, 62) != 2) { close(fd); return 1; }
    if (ftruncate(fd, (off_t)maxend) != 0) { close(fd); return 1; }
    close(fd);
    return 0;
}
