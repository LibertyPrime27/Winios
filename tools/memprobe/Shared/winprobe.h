/* Run a Windows .exe on the device, capture its stdout, time it, and report
 * how much x87 the dynarec lowered natively while it ran. */
#ifndef WINPROBE_H
#define WINPROBE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the guest's exit code (2 on a setup failure). `arg1`/`arg2` may be
 * NULL. Any of the out-parameters may be NULL. */
int win_probe_run(const char *exe_path, const char *arg1, const char *arg2,
                  char *out, size_t out_len, uint64_t *ns, uint64_t *x87_native,
                  uint64_t *x87_callout);

/* The same, plus the directory the program's own DLLs are in (winrun's -L).
 * An imported game keeps them beside its executable, which may be several
 * folders below the one the user sees, so it cannot be inferred here. */
int win_probe_run_dir(const char *exe_path, const char *dll_dir, int keep_going,
                      int timeout_s, char *out, size_t out_len, uint64_t *ns);

/* --- importing ---
 *
 * Flat scalars and caller-provided buffers, not the importer's own structs,
 * and deliberately so: Swift imports a fixed-size C array as a *tuple* of
 * that many elements, and `wi_result` carries an 8 KB report buffer. An
 * 8192-element tuple is the sort of thing that makes the Swift type checker
 * take minutes or give up altogether. Nothing here crosses the bridge as an
 * aggregate, which also matches how win_probe_imports has always worked.
 *
 * Any out-parameter may be NULL.
 */

/* What kind of file is this? Changes nothing on disk, so the UI can ask
 * before it offers a mode. Returns one of: */
enum { WIN_LOOK_EXE = 0, WIN_LOOK_FOLDER = 1, WIN_LOOK_ARCHIVE = 2, WIN_LOOK_UNKNOWN = 3 };
/*   is32          1, 0, or -1 when there is no single executable to ask
 *   is_installer  whether it should be offered as an install rather than a game
 *   family        the installer family's name ("Inno Setup", "not an installer")
 *   note          one line saying what will happen, for the person importing */
int win_probe_look(const char *path, int *is32, int *is_installer,
                   char *family, size_t family_len,
                   char *note, size_t note_len);

/* How far along an import is, for a UI that has to show something during a
 * 580 MB extraction. Polled rather than pushed: see the note in winprobe.c.
 * `stage` is a short noun ("extracting", "copying", "installing"), `total` is
 * 0 when it is not known. All parameters are optional. */
void win_probe_progress(char *stage, size_t stage_len, uint64_t *done, uint64_t *total);

/* Ask a running import to stop at its next step. It leaves what it has
 * already written in place -- an abandoned half-copy is visible in the
 * library and can be deleted, which is better than a silent rollback that
 * loses a 10-minute extraction the person actually wanted. */
void win_probe_cancel_import(void);

/* Import, in one of the two modes. Returns 0 when the program is ready to
 * run. `installer` picks the mode; `visible`, `keep_going` and `timeout_s`
 * apply only to installer mode.
 *
 * `visible` non-zero runs the installer with its own screens drawn, so a
 * person answers its questions and chooses where it goes; zero runs it in
 * its family's silent mode. Either way the installed program is found the
 * same way afterwards -- by looking at what appeared on the drive -- so a
 * destination the person typed is found as reliably as one we chose. While a
 * visible install runs, frames arrive through win_probe_copy_frame and input
 * goes back through the win_probe_input_* calls, exactly as for a game.
 *
 * `detail` receives the report *and* the guest's own output -- when an install
 * produces nothing, the reason is in the run report and the importer's summary
 * only says that nothing appeared. `name` is what to call the library entry and
 * `dir_rel` is where its folder actually is, relative to the drive -- the two
 * differ whenever an installer put the program somewhere structured, which a
 * visible install nearly always does. `exe_rel` is which executable inside
 * that folder to run and `dll_dir` is where the program's own DLLs are, for
 * win_probe_run_dir. */
int win_probe_import(const char *src, const char *drive_c, int installer,
                     int visible, int keep_going, int timeout_s,
                     char *detail, size_t detail_len,
                     char *name, size_t name_len,
                     char *dir_rel, size_t dir_rel_len,
                     char *exe_rel, size_t exe_rel_len,
                     char *dll_dir, size_t dll_dir_len,
                     int *is32, int *files, int *exes);

/* The same, for a program we did not ship and cannot vouch for.
 *   keep_going  an unimplemented import returns 0 and the run continues, so
 *               one run names everything it needed instead of the first thing
 *   timeout_s   a runaway program cannot wedge the app (0 = no limit)
 * The captured output ends with the run report either way. */
int win_probe_run_ex(const char *exe_path, int keep_going, int timeout_s,
                     char *out, size_t out_len, uint64_t *ns);

/* Run a guest with a recorded input script (winrun's -input): keyboard and
 * mouse events aimed at particular frames. The same file the shell suite
 * uses, so the device and CI see the same events and a recording means the
 * same thing in both places. */
int win_probe_run_script(const char *exe_path, const char *script, const char *arg1,
                         char *out, size_t out_len, uint64_t *ns);

/* Load an executable, resolve its imports, and report which ones nothing here
 * can satisfy -- without running it. This is how the app answers "can this
 * program run, and if not, what is missing?" for an arbitrary .exe the user
 * picked, which is the only honest first question. */
int win_probe_imports(const char *exe_path, char *out, size_t out_len);

/* The last frame the guest presented through d3d9, as X8R8G8B8 rows (B,G,R,X
 * in memory), owned here and valid until the next win_probe_run. NULL if the
 * guest presented nothing. Only safe once the run has finished. */
const void *win_probe_frame(int *width, int *height, int *pitch);

/* --- watching a guest that is still running ---
 *
 * A guest drawing frames runs on its own thread inside win_probe_run, while
 * the display refreshes on another. Rather than calling back into the UI from
 * the guest's thread, every presented frame is copied under a lock and given
 * a sequence number; the display side asks for anything newer than what it
 * last drew. That keeps the guest free-running and the frame rate honest --
 * frames the display never got to are dropped, not waited for. */

/* Copy the newest frame into `dst` if its sequence is past *seq (which is
 * then updated). Returns 1 if a frame was copied, 0 if there is nothing
 * newer or `dst` is too small for it. */
int win_probe_copy_frame(uint64_t *seq, void *dst, size_t dst_len,
                         int *width, int *height, int *pitch);

/* Ask a presenting guest to stop: its next Present returns D3DERR_DEVICELOST.
 * Cleared automatically when the next run starts. */
void win_probe_request_stop(void);

#ifdef __cplusplus
}
#endif

#endif
