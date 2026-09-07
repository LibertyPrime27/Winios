/* A zip extractor, because an itch.io download is a zip and iOS has no
 * public API that opens one.
 *
 * The Files app can browse into a zip, but what it hands back through a
 * document picker is the archive, not its contents -- and a game is a folder
 * of DLLs and data, so "the archive" is not something that can be run. So the
 * app has to be able to unpack one itself.
 *
 * Two things here are not the textbook zip reader:
 *
 *  - **Self-extracting archives.** A zip glued onto the back of an .exe has
 *    a central directory whose recorded offsets are relative to where the zip
 *    starts, not to where the file starts. Every offset is therefore short by
 *    the size of the loader in front of it. The fix is to work out that
 *    difference once, from where the central directory actually turned out to
 *    be, and apply it to everything -- which also means a 7-Zip or WinRAR SFX
 *    that happens to be a zip underneath can be unpacked rather than run.
 *
 *  - **Paths are not trusted.** An archive can name `../../etc/passwd` or
 *    `C:\Windows\System32\x.dll`, and on a phone the only defence is to not
 *    honour it. Names are sanitised to a relative path under the destination,
 *    and an entry whose name survives that as empty is skipped.
 */
#ifndef UNZIP_H
#define UNZIP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called once per entry as it is written, for a progress bar. `done` and
 * `total` count entries, not bytes: a caller wants to know how far through
 * the archive it is, and byte counts are not known until each entry's header
 * has been read anyway. Return non-zero to abort the extraction. */
typedef int (*uz_progress)(void *ctx, const char *name, uint64_t done, uint64_t total);

/* Does this file have a zip central directory at or near its end? True for a
 * plain .zip and for a self-extracting .exe with a zip payload. */
int uz_is_zip(const char *path);

/* Extract `zip_path` into `dest_dir`, which is created if needed.
 * Returns the number of files written, or -1 on failure with `err` filled in.
 * Directories in the archive are created; entries outside `dest_dir` after
 * sanitising are skipped and counted in `*skipped` (which may be NULL). */
int uz_extract(const char *zip_path, const char *dest_dir,
               uz_progress cb, void *ctx,
               int *skipped, char *err, size_t err_len);

/* mkdir -p. Exposed because everything that unpacks something needs it and
 * two copies of it in one tool would be one too many. */
int uz_mkdirs(const char *path);

/* Sanitise an archive member name to a relative path with forward slashes:
 * strips any drive letter, leading separators and every `..` component, and
 * turns backslashes into slashes. Writes at most `n` bytes and returns 0 if
 * nothing usable was left. Exposed so it can be tested directly -- this is
 * the security-relevant part and it should not only be reachable through a
 * real archive. */
int uz_safe_name(const char *raw, char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif
