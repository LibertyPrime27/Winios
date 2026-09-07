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
#include "winprobe.h"
#include "winrun.h"
#include "w32.h"
#include "xcore/cpu.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

int win_probe_run_ex(const char *exe_path, int keep_going, int timeout_s,
                     char *out, size_t out_len, uint64_t *ns) {
    char tbuf[16];
    char *argv[8];
    int argc = 0;
    argv[argc++] = (char *)"winrun";
    if (keep_going) argv[argc++] = (char *)"-k";
    if (timeout_s > 0) {
        snprintf(tbuf, sizeof tbuf, "%d", timeout_s);
        argv[argc++] = (char *)"-t";
        argv[argc++] = tbuf;
    }
    argv[argc++] = (char *)exe_path;

    g_fw = g_fh = 0;
    w32_d3d9_device_lost(0);
    w32_set_present(grab_frame, 0);
    uint64_t t0 = now_ns();
    int rc = run_capture(argc, argv, out, out_len);
    if (ns) *ns = now_ns() - t0;
    return rc;
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
static int run_capture(int argc, char **argv, char *out, size_t out_len) {
    const char *tmpdir = getenv("TMPDIR");
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%swinprobe.%d.out", tmpdir && *tmpdir ? tmpdir : "/tmp/", (int)getpid());

    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    int fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) { dup2(fd, STDOUT_FILENO); close(fd); }

    int rc = winrun_main(argc, argv);

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
