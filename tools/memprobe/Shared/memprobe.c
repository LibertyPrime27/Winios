#include "memprobe.h"
#include <stdlib.h>

unsigned char mp_pattern(size_t offset) {
    return (unsigned char)((offset >> 12) ^ 0xA5u);
}

void *mp_alloc_touch(size_t bytes, size_t page) {
    if (bytes == 0 || page == 0) return NULL;
    volatile unsigned char *p = (volatile unsigned char *)malloc(bytes);
    if (!p) return NULL;
    for (size_t off = 0; off < bytes; off += page) {
        p[off] = mp_pattern(off);
    }
    return (void *)p;
}

int mp_verify(const void *block, size_t bytes, size_t page) {
    if (!block || page == 0) return 0;
    const volatile unsigned char *p = (const volatile unsigned char *)block;
    for (size_t off = 0; off < bytes; off += page) {
        if (p[off] != mp_pattern(off)) return 0;
    }
    return 1;
}
