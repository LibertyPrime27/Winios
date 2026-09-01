/* Runs on the Linux CI job. Proves the ladder logic is sound before it is
 * trusted on device, where a bug looks identical to a low memory limit. */
#include "memprobe.h"
#include <stdio.h>
#include <stdlib.h>

#define PAGE 16384          /* iOS page size; the probe must use the host's */

static int fail(const char *what) { fprintf(stderr, "FAIL: %s\n", what); return 1; }

int main(void) {
    const size_t block = 4u * 1024u * 1024u;

    void *p = mp_alloc_touch(block, PAGE);
    if (!p) return fail("mp_alloc_touch returned NULL for 4 MB");
    if (!mp_verify(p, block, PAGE)) return fail("pattern did not survive");

    /* A corrupted stride must be detected, or verify is a no-op and the
     * compiler is free to drop the writes that make pages resident. */
    ((unsigned char *)p)[PAGE] ^= 0xFFu;
    if (mp_verify(p, block, PAGE)) return fail("mp_verify missed corruption");
    free(p);

    if (mp_alloc_touch(0, PAGE) != NULL) return fail("zero size should be NULL");
    if (mp_alloc_touch(block, 0) != NULL) return fail("zero page should be NULL");
    if (mp_verify(NULL, block, PAGE)) return fail("NULL block should not verify");

    printf("memprobe: all checks passed\n");
    return 0;
}
