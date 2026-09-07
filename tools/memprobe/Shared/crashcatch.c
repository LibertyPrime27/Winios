/* sigaction and sigaltstack are POSIX rather than C, so a strict -std=c11
 * build hides them. Darwin exposes them regardless, which is why the Xcode
 * build never minded -- but this file is worth being able to compile on a
 * Linux runner, and a header that only works on one platform is a header
 * nobody notices is broken until the wrong day. */
#define _GNU_SOURCE
#include "crashcatch.h"

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* The file the handler writes into, opened before anything can go wrong. */
static int g_fd = -1;
static volatile sig_atomic_t g_armed = 0;
/* Set while a report is being written. A fault inside the handler -- a
 * corrupted stack that makes backtrace() itself crash is the way that
 * happens -- must not start a second report on top of the first. */
static volatile sig_atomic_t g_writing = 0;

/* What was running when it went wrong. Fixed size, filled in from Swift while
 * the process is healthy, printed verbatim by the handler. */
static char g_context[1024];

/* The handler runs on its own stack, so a stack overflow -- which arrives as
 * SIGSEGV on the guard page, with no room left to call anything -- still gets
 * a report. 64 KB rather than SIGSTKSZ so this is a compile-time constant on
 * every SDK. */
#define CRASH_STACK_BYTES (64 * 1024)
static char g_altstack[CRASH_STACK_BYTES];

static const int g_signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
#define CRASH_SIGNAL_COUNT ((int)(sizeof g_signals / sizeof g_signals[0]))

/* write(2) with the length worked out by hand. strlen is reentrant in every
 * implementation but is not on the list of functions a handler is allowed to
 * call, and the loop costs nothing. */
static void wr(const char *s) {
    if (g_fd < 0 || !s) return;
    size_t n = 0;
    while (s[n]) n++;
    if (n == 0) return;
    ssize_t ignored = write(g_fd, s, n);
    (void)ignored;
}

/* Integers formatted without snprintf, for the same reason. */
static void wr_dec(long long v) {
    if (g_fd < 0) return;
    char b[24];
    int i = (int)sizeof b;
    int neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1ULL
                               : (unsigned long long)v;
    if (u == 0) b[--i] = '0';
    while (u > 0 && i > 1) { b[--i] = (char)('0' + (int)(u % 10ULL)); u /= 10ULL; }
    if (neg && i > 0) b[--i] = '-';
    ssize_t ignored = write(g_fd, b + i, (size_t)((int)sizeof b - i));
    (void)ignored;
}

static void wr_hex(unsigned long long v) {
    if (g_fd < 0) return;
    static const char digits[] = "0123456789abcdef";
    char b[20];
    int i = (int)sizeof b;
    if (v == 0) b[--i] = '0';
    while (v > 0 && i > 0) { b[--i] = digits[(int)(v & 0xFULL)]; v >>= 4; }
    wr("0x");
    ssize_t ignored = write(g_fd, b + i, (size_t)((int)sizeof b - i));
    (void)ignored;
}

/* Spelled out, because a bare signal number in a report is a number somebody
 * has to go and look up. */
static const char *signal_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (read or wrote memory it does not own)";
    case SIGBUS:  return "SIGBUS (a valid address the hardware would not accept)";
    case SIGILL:  return "SIGILL (executed something that is not an instruction)";
    case SIGFPE:  return "SIGFPE (integer divide by zero, usually)";
    case SIGABRT: return "SIGABRT (abort: an assertion, or a C++/ObjC exception nobody caught)";
    case SIGTRAP: return "SIGTRAP (a breakpoint nothing was attached to service)";
    default:      return "unknown signal";
    }
}

static void restore_default(int sig) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static void handler(int sig, siginfo_t *info, void *uap) {
    (void)uap;
    /* Already reporting: whatever this is happened inside the handler, and
     * the half-written report is worth more than a second attempt at one. */
    if (g_writing) {
        restore_default(sig);
        raise(sig);
        return;
    }
    g_writing = 1;

    wr("==== crash ====\n");
    wr("signal      ");
    wr(signal_name(sig));
    wr(" [");
    wr_dec(sig);
    wr("]\n");
    wr("si_code     ");
    wr_dec(info ? (long long)info->si_code : 0);
    wr("\n");
    wr("fault addr  ");
    wr_hex(info ? (unsigned long long)(uintptr_t)info->si_addr : 0ULL);
    wr("\n");
    wr("pid         ");
    wr_dec((long long)getpid());
    wr("\n");
    if (g_context[0]) {
        wr("what was running\n");
        wr(g_context);
        wr("\n");
    }

    /* backtrace_symbols_fd writes straight to the descriptor and never calls
     * malloc, which is the whole reason to use it rather than
     * backtrace_symbols. The frames are the host app's -- guest x86 frames are
     * in emulated memory and mean nothing to it -- so this says which part of
     * the emulator died, not which part of the game. */
    void *frames[64];
    int n = backtrace(frames, 64);
    wr("backtrace   ");
    wr_dec(n);
    wr(" frames\n");
    if (n > 0) backtrace_symbols_fd(frames, n, g_fd);
    wr("==== end ====\n");
    fsync(g_fd);

    /* Let it die the way it was going to. Swallowing the signal would leave a
     * process whose state is already wrong still running, and would stop the
     * system writing its own report -- which is worth having too, even when
     * it names the wrong process. */
    restore_default(sig);
    raise(sig);
}

int crashcatch_install(const char *path) {
    if (g_armed) return 1;
    if (!path || !*path) return 0;

    g_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_fd < 0) return 0;

    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = g_altstack;
    ss.ss_size = sizeof g_altstack;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int i = 0; i < CRASH_SIGNAL_COUNT; i++) sigaction(g_signals[i], &sa, NULL);

    g_armed = 1;
    return 1;
}

void crashcatch_set_context(const char *text) {
    if (!text) { g_context[0] = 0; return; }
    strlcpy(g_context, text, sizeof g_context);
}

void crashcatch_write_exception(const char *name, const char *reason, const char *stack) {
    if (g_fd < 0) return;
    wr("==== uncaught exception ====\n");
    wr("name        ");
    wr(name ? name : "(none)");
    wr("\n");
    wr("reason      ");
    wr(reason ? reason : "(none)");
    wr("\n");
    if (g_context[0]) {
        wr("what was running\n");
        wr(g_context);
        wr("\n");
    }
    wr("backtrace\n");
    wr(stack ? stack : "(none)");
    wr("\n==== end ====\n");
    fsync(g_fd);
}

int crashcatch_armed(void) { return g_armed; }
