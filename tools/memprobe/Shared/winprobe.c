/* Run a Windows executable on the device and hand its output back.
 *
 * winrun writes the guest's stdout to the process's stdout, which on an iOS
 * app goes nowhere anyone can see. So fd 1 is redirected to a temp file for
 * the duration of the run and restored afterwards -- the guest's own printf
 * path (win32/msvcrt.c -> fwrite -> fd 1) needs no knowledge of this.
 *
 * Keeping the argv marshalling and the descriptor juggling here rather than in
 * Swift is deliberate: both are easy to get subtly wrong across the bridge,
 * and neither is interesting enough to be worth doing twice.
 */
/* clock_gettime is POSIX rather than C, so a strict -std=c11 build hides it.
 * Darwin exposes it regardless, which is why the iOS build never minded --
 * but the same omission in drivediff.c turned a pointer into an int on a
 * 64-bit target, and a latent one is not worth keeping. */
#define _GNU_SOURCE
#include "winprobe.h"
#include "winrun.h"
#include "w32.h"
#include "import.h"
#include "xcore/cpu.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Forward declarations for the helpers at the bottom of the file. They were
 * being called from above without them, which older compilers accepted as an
 * implicit declaration and newer ones reject outright -- and this file is only
 * ever built by Xcode, so nothing in the Linux checks would have caught it. */
static uint64_t now_ns(void);
typedef int (*cap_fn)(void *ctx);
static int with_capture(cap_fn body, void *ctx, char *out, size_t out_len);
static int run_capture(int argc, char **argv, char *out, size_t out_len);

/* Presented frames are copied out rather than referenced: the guest memory
 * they live in is unmapped when the next run resets the process, and the UI
 * keeps showing the last frame until then. */
static uint8_t *g_frame;
static size_t g_frame_cap;
static int g_fw, g_fh, g_fpitch;
static uint64_t g_seq;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void grab_frame(void *ctx, const void *pixels, int width, int height, int pitch) {
    (void)ctx;
    size_t n = (size_t)height * (size_t)pitch;
    pthread_mutex_lock(&g_lock);
    if (n > g_frame_cap) {
        uint8_t *p = realloc(g_frame, n);
        if (!p) { pthread_mutex_unlock(&g_lock); return; }
        g_frame = p; g_frame_cap = n;
    }
    memcpy(g_frame, pixels, n);
    g_fw = width; g_fh = height; g_fpitch = pitch;
    g_seq++;
    pthread_mutex_unlock(&g_lock);
}

int win_probe_copy_frame(uint64_t *seq, void *dst, size_t dst_len,
                         int *width, int *height, int *pitch) {
    int got = 0;
    pthread_mutex_lock(&g_lock);
    size_t n = (size_t)g_fh * (size_t)g_fpitch;
    if (g_frame && n && (!seq || g_seq > *seq) && n <= dst_len) {
        memcpy(dst, g_frame, n);
        if (seq) *seq = g_seq;
        if (width) *width = g_fw;
        if (height) *height = g_fh;
        if (pitch) *pitch = g_fpitch;
        got = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}

/* Both halves of stopping: a guest drawing frames leaves its loop when Present
 * fails, and one that is not drawing anything needs the run loop to end it. */
void win_probe_request_stop(void) { w32_d3d9_device_lost(1); w32_request_stop(); }

int win_probe_run_dir(const char *exe_path, const char *dll_dir, int keep_going,
                      int timeout_s, char *out, size_t out_len, uint64_t *ns) {
    char tbuf[16];
    char *argv[10];
    int argc = 0;
    argv[argc++] = (char *)"winrun";
    if (keep_going) argv[argc++] = (char *)"-k";
    if (timeout_s > 0) {
        snprintf(tbuf, sizeof tbuf, "%d", timeout_s);
        argv[argc++] = (char *)"-t";
        argv[argc++] = tbuf;
    }
    /* Where the program's own DLLs are. An imported game keeps them beside
     * its executable, which for something like an Unreal title is three
     * directories below the folder the user sees -- so this cannot be
     * inferred from the entry and has to be carried. */
    if (dll_dir && *dll_dir) { argv[argc++] = (char *)"-L"; argv[argc++] = (char *)dll_dir; }
    argv[argc++] = (char *)exe_path;

    g_fw = g_fh = 0;
    w32_d3d9_device_lost(0);
    w32_set_present(grab_frame, 0);
    uint64_t t0 = now_ns();
    int rc = run_capture(argc, argv, out, out_len);
    if (ns) *ns = now_ns() - t0;
    return rc;
}

int win_probe_run_ex(const char *exe_path, int keep_going, int timeout_s,
                     char *out, size_t out_len, uint64_t *ns) {
    return win_probe_run_dir(exe_path, 0, keep_going, timeout_s, out, out_len, ns);
}

/* ---- importing --------------------------------------------------------------
 *
 * The importer runs a guest (installer mode does, at least), so it belongs on
 * this side of the bridge where the capture machinery already is. The report
 * it produces and the guest output it caused are concatenated deliberately:
 * when an install fails, the reason is in the guest's run report and the
 * importer's own summary only says that nothing appeared.
 */
/* --- progress, polled rather than pushed -------------------------------------
 *
 * A 580 MB download unpacks for a long time, and a spinner with no numbers on
 * it is indistinguishable from a hang -- which on a phone is when people force
 * quit. So the importer's progress is recorded here and the UI asks for it on
 * a timer.
 *
 * Polled and not a callback on purpose: handing Swift a C function pointer
 * with a context pointer to marshal is a lot of moving parts for something
 * whose only job is to update a label, and none of it can be compiled or
 * tested on the machine this was written on. A static, a mutex and a getter
 * can be reasoned about. */
static pthread_mutex_t g_prog_lock = PTHREAD_MUTEX_INITIALIZER;
static char     g_prog_stage[64];
static uint64_t g_prog_done, g_prog_total;
static int      g_prog_cancel;

static void prog_set(const char *stage, uint64_t done, uint64_t total) {
    pthread_mutex_lock(&g_prog_lock);
    snprintf(g_prog_stage, sizeof g_prog_stage, "%.*s", (int)sizeof g_prog_stage - 1,
             stage ? stage : "");
    g_prog_done = done;
    g_prog_total = total;
    pthread_mutex_unlock(&g_prog_lock);
}

/* The importer's callback. Returning non-zero cancels, which is how the Stop
 * button reaches a copy that is halfway through 2000 files. */
static int import_progress(void *ctx, const char *stage, uint64_t done, uint64_t total) {
    (void)ctx;
    prog_set(stage, done, total);
    pthread_mutex_lock(&g_prog_lock);
    int stop = g_prog_cancel;
    pthread_mutex_unlock(&g_prog_lock);
    return stop;
}

void win_probe_progress(char *stage, size_t stage_len, uint64_t *done, uint64_t *total) {
    pthread_mutex_lock(&g_prog_lock);
    if (stage && stage_len) snprintf(stage, stage_len, "%.*s", (int)stage_len - 1, g_prog_stage);
    if (done) *done = g_prog_done;
    if (total) *total = g_prog_total;
    pthread_mutex_unlock(&g_prog_lock);
}

void win_probe_cancel_import(void) {
    pthread_mutex_lock(&g_prog_lock);
    g_prog_cancel = 1;
    pthread_mutex_unlock(&g_prog_lock);
}

typedef struct {
    const char *src, *drive_c;
    int installer, visible, keep_going, timeout_s;
    wi_result *out;
} import_args;

static int call_import(void *ctx) {
    import_args *a = (import_args *)ctx;
    if (a->installer)
        return wi_import_installer(a->src, a->drive_c, winrun_main, a->visible,
                                   a->keep_going, a->timeout_s, import_progress, 0, a->out);
    return wi_import_game(a->src, a->drive_c, import_progress, 0, a->out);
}

/* Copy a C string into a caller's buffer, truncating rather than overrunning.
 * Every string leaving this file goes through it. */
static void put(char *dst, size_t n, const char *src) {
    if (!dst || !n) return;
    snprintf(dst, n, "%.*s", (int)n - 1, src ? src : "");
}

int win_probe_import(const char *src, const char *drive_c, int installer,
                     int visible, int keep_going, int timeout_s,
                     char *detail, size_t detail_len,
                     char *name, size_t name_len,
                     char *dir_rel, size_t dir_rel_len,
                     char *exe_rel, size_t exe_rel_len,
                     char *dll_dir, size_t dll_dir_len,
                     int *is32, int *files, int *exes) {
    /* wi_result is 11 KB, which is more than belongs on a stack shared with a
     * guest's own frames. */
    wi_result *r = (wi_result *)calloc(1, sizeof *r);
    if (!r) { put(detail, detail_len, "out of memory\n"); return -1; }
    r->is32 = -1;
    /* A cancel left over from a previous import would stop this one on its
     * first callback. */
    pthread_mutex_lock(&g_prog_lock);
    g_prog_cancel = 0;
    pthread_mutex_unlock(&g_prog_lock);
    prog_set("starting", 0, 0);

    import_args a = { src, drive_c, installer, visible, keep_going, timeout_s, r };
    /* A visible install draws, so its frames have to reach the app the same
     * way a game's do -- otherwise the installer is up on a screen nobody can
     * see and the person is looking at a spinner. */
    if (visible) {
        g_fw = g_fh = 0;
        w32_d3d9_device_lost(0);
        w32_set_present(grab_frame, 0);
    }
    char *guest = (char *)malloc(128 * 1024);
    int rc = with_capture(call_import, &a, guest, guest ? 128 * 1024 : 0);

    /* The importer's own report, then what the program printed. Both, because
     * an install that produced nothing is explained by the second half. */
    put(detail, detail_len, r->detail);
    if (guest && guest[0] && detail && detail_len) {
        size_t used = strlen(detail);
        size_t room = used + 80 < detail_len ? detail_len - used - 80 : 0;
        if (room) {
            /* The tail, not the head. An installer prints progress and *then*
             * the run report, and the run report -- which names what it called
             * that is not implemented -- is the part worth keeping. */
            size_t n = strlen(guest);
            const char *from = n > room ? guest + (n - room) : guest;
            snprintf(detail + used, detail_len - used,
                     "\n---- what the program printed%s ----\n%s",
                     n > room ? " (last part)" : "", from);
        }
    }
    free(guest);

    prog_set("", 0, 0);
    put(name, name_len, r->name);
    put(dir_rel, dir_rel_len, r->dir_rel);
    put(exe_rel, exe_rel_len, r->exe_rel);
    put(dll_dir, dll_dir_len, r->dll_dir);
    if (is32)  *is32  = r->is32;
    if (files) *files = r->files;
    if (exes)  *exes  = r->exes;
    int ok = rc == 0 && r->ok;
    free(r);
    return ok ? 0 : -1;
}

int win_probe_look(const char *path, int *is32, int *is_installer,
                   char *family, size_t family_len,
                   char *note, size_t note_len) {
    wi_probe_result p = wi_probe(path);
    if (is32) *is32 = p.is32;
    if (is_installer) *is_installer = p.looks_like_installer;
    put(family, family_len, p.setup.name);
    put(note, note_len, p.setup.note);
    switch (p.src) {
    case WI_SRC_FOLDER: return WIN_LOOK_FOLDER;
    case WI_SRC_ZIP:    return WIN_LOOK_ARCHIVE;
    case WI_SRC_EXE:    return WIN_LOOK_EXE;
    default:            return WIN_LOOK_UNKNOWN;
    }
}

/* A guest driven by a recorded input script. The script is the same file the
 * shell suite uses, so the diagnostics on the device and the check in CI are
 * looking at the same events -- which is the only way the recorded output can
 * mean the same thing in both places. */
int win_probe_run_script(const char *exe_path, const char *script, const char *arg1,
                         char *out, size_t out_len, uint64_t *ns) {
    char *argv[6];
    int argc = 0;
    argv[argc++] = (char *)"winrun";
    argv[argc++] = (char *)"-input";
    argv[argc++] = (char *)script;
    argv[argc++] = (char *)exe_path;
    if (arg1) argv[argc++] = (char *)arg1;

    g_fw = g_fh = 0;
    w32_d3d9_device_lost(0);
    w32_set_present(grab_frame, 0);
    uint64_t t0 = now_ns();
    int rc = run_capture(argc, argv, out, out_len);
    if (ns) *ns = now_ns() - t0;
    return rc;
}

const void *win_probe_frame(int *width, int *height, int *pitch) {
    if (width) *width = g_fw;
    if (height) *height = g_fh;
    if (pitch) *pitch = g_fpitch;
    return g_fw && g_fh ? g_frame : 0;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* stdout is a file for the duration, because winrun writes its report there
 * and an iOS app's stdout goes nowhere anyone can see. Same trick as
 * win_probe_run; factored out so both use one copy of the descriptor juggling. */
/* Run something with fd 1 pointed at a temp file, and hand back what it
 * wrote. Written as "any callable" rather than "winrun_main with an argv"
 * because the importer also has to be run this way: it starts a guest of its
 * own, and the guest's output -- the run report, which names everything the
 * installer wanted and did not get -- is the most useful part of an import
 * that failed. */
static int with_capture(cap_fn body, void *ctx, char *out, size_t out_len) {
    const char *tmpdir = getenv("TMPDIR");
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%swinprobe.%d.out", tmpdir && *tmpdir ? tmpdir : "/tmp/", (int)getpid());

    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    int fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) { dup2(fd, STDOUT_FILENO); close(fd); }

    int rc = body(ctx);

    fflush(stdout);
    if (saved >= 0) { dup2(saved, STDOUT_FILENO); close(saved); }

    if (out && out_len) {
        out[0] = 0;
        FILE *f = fopen(tmp, "rb");
        if (f) { size_t got = fread(out, 1, out_len - 1, f); out[got] = 0; fclose(f); }
    }
    remove(tmp);
    return rc;
}

typedef struct { int argc; char **argv; } run_args;
static int call_winrun(void *ctx) {
    run_args *a = (run_args *)ctx;
    return winrun_main(a->argc, a->argv);
}
static int run_capture(int argc, char **argv, char *out, size_t out_len) {
    run_args a = { argc, argv };
    return with_capture(call_winrun, &a, out, out_len);
}

int win_probe_imports(const char *exe_path, char *out, size_t out_len) {
    char *argv[3] = { (char *)"winrun", (char *)"-imports", (char *)exe_path };
    return run_capture(3, argv, out, out_len);
}

int win_probe_run(const char *exe_path, const char *arg1, const char *arg2,
                  char *out, size_t out_len, uint64_t *ns, uint64_t *x87_native,
                  uint64_t *x87_callout) {
    if (out && out_len) out[0] = 0;

    const char *tmpdir = getenv("TMPDIR");
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%swinprobe.%d.out", tmpdir && *tmpdir ? tmpdir : "/tmp/", (int)getpid());

    pthread_mutex_lock(&g_lock);
    g_fw = g_fh = 0;
    pthread_mutex_unlock(&g_lock);
    w32_d3d9_device_lost(0);
    w32_set_present(grab_frame, 0);

    uint64_t n0 = 0, c0 = 0;
    xc_jit_x87_stats(&n0, &c0);

    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    int fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) { dup2(fd, STDOUT_FILENO); close(fd); }

    char *argv[4];
    int argc = 0;
    argv[argc++] = (char *)"winrun";
    argv[argc++] = (char *)exe_path;
    if (arg1) argv[argc++] = (char *)arg1;
    if (arg2) argv[argc++] = (char *)arg2;

    uint64_t t0 = now_ns();
    int rc = winrun_main(argc, argv);
    uint64_t t1 = now_ns();

    fflush(stdout);
    if (saved >= 0) { dup2(saved, STDOUT_FILENO); close(saved); }

    if (ns) *ns = t1 - t0;
    uint64_t n1 = 0, c1 = 0;
    xc_jit_x87_stats(&n1, &c1);
    if (x87_native) *x87_native = n1 - n0;
    if (x87_callout) *x87_callout = c1 - c0;

    if (out && out_len) {
        FILE *f = fopen(tmp, "rb");
        if (f) {
            size_t got = fread(out, 1, out_len - 1, f);
            out[got] = 0;
            fclose(f);
        }
    }
    remove(tmp);
    return rc;
}
