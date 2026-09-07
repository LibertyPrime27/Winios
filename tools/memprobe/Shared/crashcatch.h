/* Recording a crash from inside the crash.
 *
 * Four .ips reports came off a device and between them said almost nothing
 * about this app. Two were cpu_resource_fatal -- the process killed for
 * averaging 99% CPU over 48 seconds against iOS's 80%-over-60 limit -- with
 * the heaviest stack in somebody else's SwiftUI. One was a SIGSEGV inside
 * StikDebug's own log manager, a Combine array mutated off its queue. One was
 * SIGABRT in SwiftUI's RenderBox failing to load a Metal library, which is
 * what running under LiveContainer looks like. A report that reaches a person
 * days later names the process that died, and under LiveContainer plus
 * StikDebug that is very often not us. So the app has to write its own.
 *
 * This half is C on purpose. A signal handler may call async-signal-safe
 * functions and nothing else, and almost everything Swift does on the way to
 * producing a line of text -- allocating a String, retaining an object,
 * taking the runtime's locks, going near Foundation or os_log -- is none of
 * those things. A handler that allocates deadlocks against the malloc lock
 * the faulting thread was already holding often enough that it is not a
 * theoretical concern; when it happens the process dies with no report at
 * all, which is the exact failure this exists to prevent. So the handler is
 * plain C over a file descriptor opened while everything still worked, it
 * formats its own integers, and the only library calls it makes are write(2),
 * backtrace() and backtrace_symbols_fd().
 */
#ifndef CRASHCATCH_H
#define CRASHCATCH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Arm the handlers for SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT and SIGTRAP.
 *
 * `path` is opened here, now, and held open: open() inside a handler can
 * block, can fail for reasons that need errno, and would be one more thing to
 * go wrong at the worst moment. The file is truncated on the way in, so the
 * caller must have moved any report from the previous launch out of the way
 * first.
 *
 * Returns 1 when armed, 0 when the file could not be opened. Calling it twice
 * does nothing the second time. */
int crashcatch_install(const char *path);

/* What was running, in text, to be printed above the backtrace. Copied into a
 * fixed buffer here -- the handler cannot go and ask Swift for a string. Safe
 * to call as often as it changes; it is not safe to call from a handler. */
void crashcatch_set_context(const char *text);

/* An Objective-C exception, from NSSetUncaughtExceptionHandler. That runs in
 * ordinary context rather than in a signal handler, so the caller may build
 * the strings however it likes; this only puts them in the same file as the
 * signal reports so there is one place to look. */
void crashcatch_write_exception(const char *name, const char *reason,
                                const char *stack);

/* 1 once install() has succeeded. */
int crashcatch_armed(void);

#ifdef __cplusplus
}
#endif
#endif /* CRASHCATCH_H */
