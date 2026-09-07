/* Two questions the importer cannot answer without walking the drive.
 *
 * **What did the installer put there?** An installer's script is its own
 * business; asking it where it installed to means parsing a different format
 * for every family. The filesystem already knows, though: snapshot the drive,
 * run the setup, snapshot again, and the difference is the install. That works
 * the same for Inno, NSIS, a self-extractor and something we have never seen.
 *
 * **Which of these executables is the game?** A game folder routinely holds
 * half a dozen: an uninstaller, a crash handler, a redistributable, a
 * launcher, and the program itself. Offering the alphabetically first one is
 * wrong most of the time, and making the user guess from a list of eight is
 * only slightly better. The engines leave fingerprints -- Unity puts a
 * `<Name>_Data` folder beside its executable, Godot a `.pck`, GameMaker a
 * `data.win`, Unreal a `Binaries/Win64/...-Shipping.exe` -- and those are
 * worth far more than any name-based guess.
 */
#ifndef DRIVEDIFF_H
#define DRIVEDIFF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dd_snap dd_snap;

/* Every file under `root`, recorded by relative path, size and mtime.
 * Directories are not recorded in their own right -- a directory with nothing
 * in it is not an install. Returns NULL if `root` cannot be walked. */
dd_snap *dd_snapshot(const char *root);
void     dd_free(dd_snap *s);
int      dd_count(const dd_snap *s);

/* Files present in `now` but not in `before`, or whose size or mtime changed.
 * Called in path order, which puts an install's own tree together. Return
 * non-zero from the callback to stop. `before` may be NULL, in which case
 * every file is new. */
typedef int (*dd_added_fn)(void *ctx, const char *rel, uint64_t size);
int dd_added(const dd_snap *before, const dd_snap *now, dd_added_fn cb, void *ctx);

/* The common prefix of every added path, which for a well-behaved installer
 * is the directory it installed into. Returns 0 if the additions are spread
 * across the drive (an installer that wrote to Windows\System32 as well as
 * its own folder, which is most of them) -- in that case the caller should
 * look at the deepest directory that holds an executable instead. */
int dd_added_root(const dd_snap *before, const dd_snap *now, char *out, size_t n);

/* --- which executable to run ---------------------------------------------- */

enum { DD_MAX_EXES = 64 };

typedef struct {
    char rel[512];       /* path relative to the folder that was ranked      */
    uint64_t size;
    int is32;            /* from the PE header; -1 if it could not be read   */
    int score;           /* higher is more likely to be the thing to run     */
    char why[96];        /* what earned or cost it the points, for the UI    */
} dd_exe;

/* Rank the executables under `root`, best first. `hint` is the name the user
 * knows the program by (the folder or archive name) and is worth points when
 * an executable matches it; it may be NULL. Returns how many were found, at
 * most `max`. */
int dd_rank_exes(const char *root, const char *hint, dd_exe *out, int max);

/* Read just enough of a PE to answer "32 or 64 bit". 1, 0, or -1 for
 * "not a PE / unreadable". */
int dd_pe_is32(const char *path);

#ifdef __cplusplus
}
#endif

#endif
