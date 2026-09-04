#include "xcore/cpu.h"

void xc_mem_init_identity(xc_mem *m) {
    m->mode = XC_MODE_64;
    m->base = 0;
    m->size = 0;
}

void xc_mem_init_arena(xc_mem *m, void *base, uint64_t size) {
    m->mode = XC_MODE_32;
    m->base = (uint8_t *)base;
    m->size = size;
}

void *xc_mem_ptr(const xc_mem *m, uint64_t gaddr, size_t len) {
    if (m->mode == XC_MODE_64) {
        return (void *)(uintptr_t)gaddr;
    }
    /* 32-bit: addresses are already masked to 32 bits by the caller, but be
     * defensive -- an unmasked address here would silently escape the arena. */
    uint64_t a = gaddr & 0xFFFFFFFFu;
    if (a + len > m->size || a + len < a) return 0;
    /* uintptr arithmetic: a NULL base (identity in the low 4 GB, used by the
     * 32-bit difftest) is legal here. */
    return (void *)((uintptr_t)m->base + (uintptr_t)a);
}
