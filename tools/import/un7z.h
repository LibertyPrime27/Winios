/* 7z archives: what a game download from itch or a mod site is as often as
 * it is a zip. Read here, with the same face as unzip.h, so the importer
 * treats the two alike. See un7z.c for what is and is not decoded. */
#ifndef UN7Z_H
#define UN7Z_H
#include "unzip.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* A 7z archive, plain or appended to a self-extractor stub. */
int sz_is_7z(const char *path);
/* Bytes the archive unpacks to, or 0 if it cannot be read. */
uint64_t sz_uncompressed_total(const char *path);
/* Extract into dest_dir. Returns files written, or -1 with `err` filled. Entries
 * that cannot be decoded (a coder not here, a bad CRC, an unsafe name) are
 * counted in *skipped. */
int sz_extract(const char *path, const char *dest_dir, uz_progress cb, void *ctx,
               int *skipped, char *err, size_t err_len);
/* A RAR archive or RAR self-extractor: recognised so it can be refused with a
 * reason rather than a shrug. */
int sz_is_rar(const char *path);
#ifdef __cplusplus
}
#endif
#endif
