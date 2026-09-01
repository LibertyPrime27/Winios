/* Portable core of the extension memory probe.
 *
 * The question this exists to answer: can an iOS app extension hold ~3 GB
 * resident? Apple documents that extension points define their own memory
 * limits which OVERRIDE com.apple.developer.kernel.increased-memory-limit
 * (the docs use a 100 MB example). LiveContainer's author claims parity with
 * regular apps. Both cannot be true, and the answer gates whether wineserver
 * can run as an extension -- and therefore whether 64-bit Windows games are
 * reachable on iOS at all. See docs/ARCHITECTURE.md section 2.
 */
#ifndef MEMPROBE_H
#define MEMPROBE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate `bytes` and dirty one byte in every `page`-sized stride.
 *
 * Touching matters: untouched pages are not resident, so an allocation-only
 * loop measures the address space limit rather than the jetsam limit and will
 * happily "succeed" at sizes that would be killed in practice.
 *
 * Returns the block, or NULL if the allocation itself failed. */
void *mp_alloc_touch(size_t bytes, size_t page);

/* Re-read every touched stride and check the value written by mp_alloc_touch.
 * Returns 1 if intact. Its real job is to be a consumer of the writes so the
 * optimizer cannot elide them. */
int mp_verify(const void *block, size_t bytes, size_t page);

/* Byte pattern mp_alloc_touch writes at a given offset. */
unsigned char mp_pattern(size_t offset);

#ifdef __cplusplus
}
#endif
#endif /* MEMPROBE_H */
