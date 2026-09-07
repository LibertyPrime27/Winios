/* Bringing a Windows program in from outside, in the two shapes it arrives in.
 *
 * **A game** is already installed: a folder with an executable, some DLLs and
 * a lot of data, usually downloaded as a zip. Importing it is copying it onto
 * the virtual C: and working out which of its executables to run.
 *
 * **An installer** is a program whose *output* is the game. Importing it means
 * running it -- and an installer's window is the one thing this runtime cannot
 * draw, because Windows dialogs are comctl32 controls and comctl32 is not
 * implemented. Every installer family in wide use has a silent mode, though,
 * put there so that deployments can happen without a human clicking Next, and
 * in silent mode an installer is a file copier with a registry writer
 * attached. So: identify the family, use its flags, and find out afterwards
 * what appeared on the drive.
 *
 * The two modes are separate calls rather than one call with a guess, because
 * the guess is wrong in the expensive direction: running a game as an
 * installer wastes a minute, and copying an installer in as a game leaves the
 * user with a library entry that opens a dialog and stops. The caller knows
 * which it has -- and `wi_probe` tells it what the file looks like, so the UI
 * can offer the right mode first.
 */
#ifndef WI_IMPORT_H
#define WI_IMPORT_H

#include <stddef.h>
#include <stdint.h>
#include "setupkind.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { WI_SRC_EXE, WI_SRC_FOLDER, WI_SRC_ZIP, WI_SRC_UNKNOWN } wi_srckind;

typedef struct {
    wi_srckind src;
    sk_info    setup;        /* what kind of installer, if it is one */
    int        looks_like_installer;
    int        is32;         /* -1 when there is no single executable to ask */
    char       suggested_name[256];
} wi_probe_result;

/* Look at `path` without changing anything: is it a folder, an archive or an
 * executable, and if an executable, is it a setup program? */
wi_probe_result wi_probe(const char *path);

/* What an import produced. `detail` is the human-readable report -- it is the
 * product, not a log: an import that half-worked has to be able to say so. */
typedef struct {
    int  ok;
    char name[256];          /* the library entry: a folder name under C:\    */
    char exe_rel[512];       /* which executable inside it to run             */
    char exe_host[1200];     /* the same, as a host path, ready to run        */
    char dll_dir[1200];      /* where its own DLLs are (winrun's -L): a prefix
                              * of exe_host, so it is sized the same          */
    int  is32;
    int  files;              /* how many were copied or extracted             */
    int  exes;               /* how many executables were found               */
    char detail[8192];
} wi_result;

/* Progress, so a phone can show something during a 400 MB extraction.
 * `stage` is a short noun ("extracting", "copying", "installing"). Return
 * non-zero to cancel. */
typedef int (*wi_progress)(void *ctx, const char *stage, uint64_t done, uint64_t total);

/* Import an already-installed program. `src` may be a folder, a .zip, a
 * self-extracting .exe, or a single loose .exe. Everything lands in
 * `drive_c`/<name>. */
int wi_import_game(const char *src, const char *drive_c,
                   wi_progress cb, void *ctx, wi_result *out);

/* Import by running a setup program.
 *   keep_going  carry on past functions we have not implemented (winrun's -k).
 *               Off is the honest setting -- an install that stopped is better
 *               than one that half-happened -- but on names everything the
 *               installer wanted in a single run, which is the roadmap.
 *   timeout_s   an installer that waits for a window nobody will click cannot
 *               be allowed to wait forever.
 * Requires a run function, so this file does not have to link the emulator;
 * pass wi_run_winrun (below) unless you are testing.
 */
typedef int (*wi_runner)(int argc, char **argv);
int wi_import_installer(const char *setup, const char *drive_c,
                        wi_runner run, int keep_going, int timeout_s,
                        wi_progress cb, void *ctx, wi_result *out);

/* How many bytes an import of `src` will need on the drive, and how many are
 * free where `drive_c` is. Either out-parameter may be NULL. Returns 0 when
 * the size could not be worked out (an unreadable source), which the caller
 * should treat as "go ahead" rather than "refuse" -- a wrong refusal is worse
 * than a run that fails on a full disk.
 *
 * Both modes call this before writing anything. The destination is a phone:
 * running out of space halfway through leaves a broken half-install *and* a
 * full disk, and the person then has to work out which to deal with first. */
int wi_space_needed(const char *src, const char *drive_c,
                    uint64_t *needed, uint64_t *free_bytes);

/* Turn a name from outside into one that is safe as a single directory
 * component: no separators, no leading dots, no trailing spaces, and never
 * empty. Exposed because it is worth testing on its own. */
int wi_safe_component(const char *raw, char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif
