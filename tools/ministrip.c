#define _XOPEN_SOURCE 700
/* ministrip - drop ELF section headers (debug-only metadata) from a binary. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) return 1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 64) { close(fd); return 1; }
    unsigned char h[64];
    if (read(fd, h, 64) != 64 || h[0] != 0x7f || h[1] != 'E' || h[4] != 2) { close(fd); return 1; }
    uint64_t phoff, maxend = 64;
    uint16_t phentsize = (uint16_t)(h[54] | (h[55] << 8)), phnum = (uint16_t)(h[56] | (h[57] << 8));
    memcpy(&phoff, h + 32, 8);
    uint16_t i;
    for (i = 0; i < phnum; i++) {
        unsigned char ph[56];
        if (pread(fd, ph, 56, (off_t)(phoff + (uint64_t)i * phentsize)) != 56) continue;
        uint32_t type; uint64_t off, filesz;
        memcpy(&type, ph, 4); memcpy(&off, ph + 8, 8); memcpy(&filesz, ph + 32, 8);
        if (type == 1 && off + filesz > maxend) maxend = off + filesz;
    }
    unsigned char z[8] = {0};
    unsigned char zz[2] = {0};
    if (pwrite(fd, z, 8, 40) != 8 || pwrite(fd, zz, 2, 60) != 2 ||
        pwrite(fd, zz, 2, 62) != 2) { close(fd); return 1; }
    if (ftruncate(fd, (off_t)maxend) != 0) { close(fd); return 1; }
    close(fd);
    return 0;
}
