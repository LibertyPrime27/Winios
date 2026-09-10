/* Guest threads.
 *
 * `CreateThread` was the last import a game-shaped program asked for that
 * was not here, and it is the largest compatibility gap by a distance:
 * nearly every game after 2000 starts one, for loading, for audio, for its
 * own job system. Without it the runnable catalogue is roughly pre-2000.
 *
 * The design decision worth stating
 * ---------------------------------
 * Each guest thread is a real host thread with its own registers, TEB and
 * stack -- but only one of them executes guest instructions at a time. They
 * take turns under one lock, handed over at every execution slice and at
 * every blocking call.
 *
 * That is deliberately not the fastest possible answer. What it buys is that
 * everything the runtime already owns stays single-threaded: the decoded
 * block cache, the dynarec's code arena and its block-chaining patches, the
 * guest heap's bump allocator, the handle table, the import stub table. None
 * of those were written to be shared, and making them safe is a separate
 * piece of work with its own tests -- whereas *semantics* are what
 * compatibility needs first. A game whose loading thread makes progress and
 * whose WaitForSingleObject returns when it should does not care that the two
 * threads did not run on two cores; a game running against a subtly racy
 * block cache fails in ways nobody can reproduce.
 *
 * So the cost is parallelism, which is a performance limit and not a
 * compatibility one, and the emulator is the bottleneck anyway. Removing the
 * lock later is an optimisation with a clear test to write first: run the
 * differential suite with two threads compiling at once.
 *
 * What a thread owns, and what it shares
 * --------------------------------------
 * Registers, TEB, stack, the last-error value and TLS *values* are per
 * thread; guest memory, the heap, handles, modules and the TLS index
 * allocation are per process. TLS values needed no work: they already lived
 * in the TEB, so giving each thread its own TEB gave each thread its own TLS.
 *
 * Blocking, which is the part that has to be right
 * ------------------------------------------------
 * A wait releases the lock and sleeps on a condition variable, so the thread
 * that will signal it can run. Every state change broadcasts. That is more
 * wakeups than necessary and it is the right trade at this scale: a game has
 * a handful of threads, and a missed wakeup is a hang nobody can debug.
 */
#include "w32.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Wait return values */
enum { WAIT_OBJECT_0_ = 0, WAIT_ABANDONED_0_ = 0x80, WAIT_TIMEOUT_ = 0x102,
       WAIT_FAILED_ = 0xFFFFFFFFu };
enum { INFINITE_ = 0xFFFFFFFFu };

/* CRITICAL_SECTION, from the real header (32-bit / 64-bit):
 *   DebugInfo 0/0, LockCount 4/8, RecursionCount 8/12, OwningThread 12/16,
 *   LockSemaphore 16/24, SpinCount 20/32.
 * The CRT reads LockCount and RecursionCount in some versions, so they are
 * maintained rather than ignored. */
static uint64_t cs_off_lock(w32 *w)  { return w->is32 ? 4 : 8; }
static uint64_t cs_off_recur(w32 *w) { return w->is32 ? 8 : 12; }
static uint64_t cs_off_owner(w32 *w) { return w->is32 ? 12 : 16; }

/* --- the threads ---------------------------------------------------------- */

static w32_thread g_threads[W32_MAX_THREADS];
static int g_nthreads;
static pthread_key_t g_self_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* One lock for executing guest code, one condvar for every state change. */
static pthread_mutex_t g_guest = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_change = PTHREAD_COND_INITIALIZER;
static int g_exiting;                     /* ExitProcess: every thread unwinds */

static void make_key(void) { pthread_key_create(&g_self_key, 0); }

w32_thread *w32_self(void) {
    pthread_once(&g_once, make_key);
    w32_thread *t = pthread_getspecific(g_self_key);
    /* Before the main thread has registered -- and on any host thread that is
     * not a guest thread at all -- thread 0 is the answer. It is the only one
     * that exists then, and returning NULL would make every accessor a
     * null-check for a case that cannot happen once a process is running. */
    return t ? t : &g_threads[0];
}
/* The w32 * is unused: which CPU you get depends on which thread is asking,
 * not on which process it belongs to (there is one process). It stays in the
 * signature because every caller has a w32 * to hand and because a second
 * process would need it. const, so callers that only read the guest can ask. */
xc_cpu *w32_cpu(const w32 *w) { (void)w; return w32_self()->c; }

void w32_guest_lock(void)   { pthread_mutex_lock(&g_guest); }
void w32_guest_unlock(void) { pthread_mutex_unlock(&g_guest); }

/* Hand the lock over between execution slices. Without this a thread that
 * never blocks would keep the others from ever starting -- which is exactly
 * what a game's main loop does while a loading thread is supposed to be
 * working. */
void w32_guest_yield(void) {
    if (g_nthreads <= 1) return;              /* nobody to yield to */
    pthread_cond_broadcast(&g_change);
    pthread_mutex_unlock(&g_guest);
    sched_yield();
    pthread_mutex_lock(&g_guest);
}

int w32_exiting(void) { return g_exiting; }
void w32_thread_exit_all(void) {
    g_exiting = 1;
    pthread_cond_broadcast(&g_change);
}

/* --- registering threads ------------------------------------------------- */

/* Thread 0: the one the process started on. Called from process setup, before
 * any guest code runs, so no lock is needed or held. */
w32_thread *w32_thread_main(w32 *w, xc_cpu *c, uint64_t teb, uint64_t stack_lo, uint64_t stack_hi) {
    pthread_once(&g_once, make_key);
    memset(g_threads, 0, sizeof g_threads);
    g_nthreads = 1;
    g_exiting = 0;
    w32_thread *t = &g_threads[0];
    t->c = c;
    t->teb = teb;
    t->stack_limit = stack_lo;
    t->stack_base = stack_hi;
    t->id = 4243;                             /* what GetCurrentThreadId reported before threads */
    t->used = 1;
    t->running = 1;
    t->host = pthread_self();
    pthread_setspecific(g_self_key, t);
    (void)w;
    return t;
}

static w32_thread *thread_alloc(void) {
    for (int i = 0; i < W32_MAX_THREADS; i++)
        if (!g_threads[i].used) { memset(&g_threads[i], 0, sizeof g_threads[i]); g_threads[i].used = 1; return &g_threads[i]; }
    return 0;
}

static w32_thread *thread_by_handle(uint64_t h) {
    for (int i = 0; i < W32_MAX_THREADS; i++)
        if (g_threads[i].used && g_threads[i].handle == h) return &g_threads[i];
    return 0;
}

/* --- CreateThread -------------------------------------------------------- */

static w32 *g_w;                              /* the process, for the thread body */

/* The body of a guest thread.
 *
 * It takes the guest lock before touching anything and holds it for as long
 * as it is running guest code. w32_call_guest pushes a return-to-host stub
 * and re-enters the run loop, which is exactly the shape a thread procedure
 * needs: run until it returns, then take its return value as the exit code.
 */
static uint64_t pool_body(w32 *w, w32_thread *t);
static void *thread_body(void *arg) {
    w32_thread *t = arg;
    pthread_once(&g_once, make_key);
    pthread_setspecific(g_self_key, t);

    pthread_mutex_lock(&g_guest);
    w32 *w = g_w;
    xc_cpu *c = t->c;

    /* Same starting state the initial thread gets, minus the process-wide
     * parts: the TEB in the right segment base, a stack, 53-bit FPU
     * precision (which MSVC-built code relies on), and a return address that
     * ends the thread rather than the process. */
    if (w->is32) {
        c->fs_base = t->teb;
        c->sreg[1] = 0x1b; c->sreg[0] = c->sreg[2] = c->sreg[3] = 0x23; c->sreg[4] = 0x3b;
    } else {
        c->gs_base = t->teb;
    }
    c->fcw = 0x027F;
    c->gpr[XC_RSP] = w->is32 ? (uint32_t)((t->stack_base - 0x100) & ~15u)
                             : ((t->stack_base - 0x100) & ~15ull);
    t->running = 1;

    w32_thread_notify(w, 2);                          /* DLL_THREAD_ATTACH, before the body */
    uint64_t rc;
    if (t->pool_kind) rc = pool_body(w, t);
    else { uint64_t args[1] = { t->param }; rc = w32_call_guest(w, t->entry, 1, args); }
    if (!w->exited) w32_thread_notify(w, 3);          /* DLL_THREAD_DETACH, after it */

    t->exit_code = (uint32_t)rc;
    t->running = 0;
    t->finished = 1;
    /* the handle becomes signalled, which is what a join waits on */
    w32_handle *h = w32_handle_get(w, t->handle);
    if (h) h->flags |= 1;
    g_nthreads--;
    pthread_cond_broadcast(&g_change);
    pthread_mutex_unlock(&g_guest);
    return 0;
}

/* --- the thread pool ------------------------------------------------------
 *
 * Vista's thread pool: work items, timers and waits, each a callback the
 * pool runs on a thread of its own choosing. The C runtime probes for these
 * and ConcRT (std::async, PPL) lives on them, so a program that finds them
 * missing either falls back or fails, and one that finds a fake fails fast.
 *
 * Here every callback runs on a guest thread made for it, which is the
 * pool's contract kept exactly if not efficiently: SubmitThreadpoolWork
 * starts a thread that calls the work callback, SetThreadpoolTimer one that
 * sleeps until the due time and calls the timer callback (again every
 * period, until the timer is set to nothing), SetThreadpoolWait one that
 * waits on the handle and calls the wait callback with the result. A pool
 * object is 48 bytes of guest memory: the callback, its context, the kind,
 * how many callbacks are outstanding (what the WaitFor* functions wait on),
 * a generation that a new Set bumps so the previous timer thread ends, and
 * an event to set when the callback returns. Environments, cleanup groups
 * and pools proper are accepted and not modelled -- there is nothing to
 * configure about a pool that makes a thread per callback.
 */
enum { POOL_WORK = 1, POOL_TIMER, POOL_WAIT, POOL_SIMPLE };
enum { PO_CB = 0, PO_CTX = 8, PO_KIND = 16, PO_PENDING = 24, PO_GEN = 28, PO_EVENT = 32, PO_SIZE = 48 };
static uint64_t spawn_pool(w32 *w, w32_thread **out);
static int spawn_launch(w32 *w, w32_thread *t);
static uint64_t now_ms(void);
static uint64_t spawn(w32 *w, uint64_t entry, uint64_t param, uint64_t stack_size, uint32_t *out_id);

/* sleep with the guest lock released, until the time is up, the process
 * exits, or the object's generation moves on; 1 if the wait ran out */
static int pool_sleep(w32 *w, w32_thread *t, uint32_t ms) {
    uint64_t until = now_ms() + ms;
    for (;;) {
        if (g_exiting || w->exited) return 0;
        if (t->pool_obj && (uint32_t)w32_read(w, t->pool_obj + PO_GEN, 4) != t->pool_gen) return 0;
        uint64_t now = now_ms();
        if (now >= until) return 1;
        uint64_t left = until - now; if (left > 20) left = 20;
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)left * 1000000L;
        while (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_change, &g_guest, &ts);
    }
}
static uint32_t wait_one(w32 *w, uint64_t hv, uint32_t ms);
static void pool_done(w32 *w, w32_thread *t) {
    if (!t->pool_obj) return;
    uint32_t p = (uint32_t)w32_read(w, t->pool_obj + PO_PENDING, 4);
    if (p) w32_write(w, t->pool_obj + PO_PENDING, 4, p - 1);
    uint64_t ev = w32_read(w, t->pool_obj + PO_EVENT, 8);
    if (ev) { w32_event_set(w, ev); w32_write(w, t->pool_obj + PO_EVENT, 8, 0); }
    pthread_cond_broadcast(&g_change);
}
static uint64_t pool_body(w32 *w, w32_thread *t) {
    uint64_t rc = 0;
    switch (t->pool_kind) {
    case POOL_TIMER:
        if (!pool_sleep(w, t, t->pool_delay_ms)) break;
        for (;;) {
            rc = w32_call_guest(w, t->entry, t->pool_nargs, t->pool_args);
            if (!t->pool_period_ms || w->exited) break;
            if (!pool_sleep(w, t, t->pool_period_ms)) break;
        }
        break;
    case POOL_WAIT: {
        uint32_t r = wait_one(w, t->pool_wait_handle, t->pool_wait_ms);
        if (w->exited || g_exiting) break;
        if (t->pool_obj && (uint32_t)w32_read(w, t->pool_obj + PO_GEN, 4) != t->pool_gen) break;   /* re-set or cancelled meanwhile */
        t->pool_args[3] = r;
        rc = w32_call_guest(w, t->entry, t->pool_nargs, t->pool_args);
        break;
    }
    default:
        rc = w32_call_guest(w, t->entry, t->pool_nargs, t->pool_args);
        break;
    }
    if (!w->exited) pool_done(w, t);
    return rc;
}
static uint64_t pool_new(w32 *w, int kind, uint64_t cb, uint64_t ctx) {
    uint64_t o = w32_heap_alloc(w, PO_SIZE);
    if (!o) return 0;
    for (int i = 0; i < PO_SIZE; i += 8) w32_write(w, o + i, 8, 0);
    w32_write(w, o + PO_CB, 8, cb); w32_write(w, o + PO_CTX, 8, ctx); w32_write(w, o + PO_KIND, 4, (uint32_t)kind);
    return o;
}
/* start the thread that runs one callback of `obj`; the arguments are the
 * callback's (instance, context, object[, result]) */
static int pool_start(w32 *w, uint64_t obj, int kind, int nargs, uint32_t delay_ms, uint32_t period_ms, uint64_t wait_h, uint32_t wait_ms) {
    uint64_t cb = w32_read(w, obj + PO_CB, 8), ctx = w32_read(w, obj + PO_CTX, 8);
    if (!cb) return 0;
    w32_thread *t = 0;
    uint32_t pending = (uint32_t)w32_read(w, obj + PO_PENDING, 4);
    w32_write(w, obj + PO_PENDING, 4, pending + 1);
    uint64_t h = spawn_pool(w, &t);
    if (!h || !t) { w32_write(w, obj + PO_PENDING, 4, pending); return 0; }
    t->pool_kind = kind; t->pool_nargs = nargs; t->pool_obj = obj;
    t->pool_args[0] = obj; t->pool_args[1] = ctx; t->pool_args[2] = obj; t->pool_args[3] = 0;
    t->pool_delay_ms = delay_ms; t->pool_period_ms = period_ms;
    t->pool_wait_handle = wait_h; t->pool_wait_ms = wait_ms;
    t->pool_gen = (uint32_t)w32_read(w, obj + PO_GEN, 4);
    t->entry = cb;
    /* the thread was made without being started; now that it knows its job, off it goes */
    if (!spawn_launch(w, t)) { w32_write(w, obj + PO_PENDING, 4, pending); return 0; }
    return 1;
}
/* wait until no callback of `obj` is outstanding */
static void pool_wait_idle(w32 *w, uint64_t obj) {
    while (!g_exiting && !w->exited && obj && w32_read(w, obj + PO_PENDING, 4)) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_change, &g_guest, &ts);
    }
}
/* a FILETIME due time in milliseconds from now: negative is relative (in
 * 100 ns units), positive is absolute since 1601 */
static uint32_t due_ms(w32 *w, uint64_t pft) {
    if (!pft || !w32_mem_ok(w, pft, 8)) return 0;
    int64_t ft = (int64_t)w32_read(w, pft, 8);
    if (ft < 0) return (uint32_t)((-ft) / 10000);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    int64_t now = (int64_t)ts.tv_sec * 10000000 + ts.tv_nsec / 100 + 116444736000000000LL;
    return ft > now ? (uint32_t)((ft - now) / 10000) : 0;
}
static void k_CreateThreadpoolWork(w32 *w)  { RET(pool_new(w, POOL_WORK, ARG(0), ARG(1))); }
static void k_CreateThreadpoolTimer(w32 *w) { RET(pool_new(w, POOL_TIMER, ARG(0), ARG(1))); }
static void k_CreateThreadpoolWait(w32 *w)  { RET(pool_new(w, POOL_WAIT, ARG(0), ARG(1))); }
static void k_SubmitThreadpoolWork(w32 *w) {
    /* the callback is (instance, context, work) */
    if (ARG(0)) pool_start(w, ARG(0), POOL_WORK, 3, 0, 0, 0, 0);
}
/* SetThreadpoolTimer(timer, pftDueTime, msPeriod, msWindowLength): a NULL
 * due time cancels; setting again supersedes the previous setting. */
static void k_SetThreadpoolTimer(w32 *w) {
    uint64_t o = ARG(0);
    if (!o) return;
    w32_write(w, o + PO_GEN, 4, (uint32_t)w32_read(w, o + PO_GEN, 4) + 1);      /* ends the thread of the previous setting */
    if (!ARG(1)) return;
    pool_start(w, o, POOL_TIMER, 3, due_ms(w, ARG(1)), (uint32_t)ARG(2), 0, 0);
}
static void k_IsThreadpoolTimerSet(w32 *w) { RET(ARG(0) && w32_read(w, ARG(0) + PO_PENDING, 4) ? 1 : 0); }
/* SetThreadpoolWait(wait, handle, pftTimeout): the callback is
 * (instance, context, wait, waitResult); a NULL handle cancels. */
static void k_SetThreadpoolWait(w32 *w) {
    uint64_t o = ARG(0);
    if (!o) return;
    w32_write(w, o + PO_GEN, 4, (uint32_t)w32_read(w, o + PO_GEN, 4) + 1);
    if (!ARG(1)) return;
    uint32_t ms = ARG(2) ? due_ms(w, ARG(2)) : 0xFFFFFFFFu;
    if (ARG(2) && w32_mem_ok(w, ARG(2), 8) && w32_read(w, ARG(2), 8) == 0) ms = 0;            /* a zero timeout: report now */
    pool_start(w, o, POOL_WAIT, 4, 0, 0, ARG(1), ms);
}
static void k_WaitForThreadpoolCallbacks(w32 *w) {
    /* (object, cancelPending): pending callbacks that have not started
     * would be dropped by `cancel`; here every submitted callback has its
     * thread already, so all of them run, and the wait is for all */
    pool_wait_idle(w, ARG(0));
}
static void k_CloseThreadpoolObject(w32 *w) {
    /* the memory stays: a thread may still be finishing on it */
    if (ARG(0)) w32_write(w, ARG(0) + PO_GEN, 4, (uint32_t)w32_read(w, ARG(0) + PO_GEN, 4) + 1);
}
/* TrySubmitThreadpoolCallback(callback, context, environment): a one-off
 * work item without an object to wait on; the callback is (instance, context). */
static void k_TrySubmitThreadpoolCallback(w32 *w) {
    uint64_t o = pool_new(w, POOL_SIMPLE, ARG(0), ARG(1));
    RET(o && pool_start(w, o, POOL_SIMPLE, 2, 0, 0, 0, 0) ? 1 : 0);
}
static void k_CreateThreadpool(w32 *w) { RET(w32_heap_alloc(w, 16)); }
static void k_CloseThreadpool(w32 *w) { (void)w; }
static void k_SetThreadpoolThreadMaximum(w32 *w) { (void)w; }
static void k_SetThreadpoolThreadMinimum(w32 *w) { RET(1); }
static void k_CreateThreadpoolCleanupGroup(w32 *w) { RET(w32_heap_alloc(w, 16)); }
/* CloseThreadpoolCleanupGroupMembers waits for every outstanding callback of
 * the group's objects; the group is not tracked, so it waits for every pool
 * thread there is, which includes them. */
static void k_CloseThreadpoolCleanupGroupMembers(w32 *w) {
    for (;;) {
        int busy = 0;
        for (int i = 0; i < W32_MAX_THREADS; i++) if (g_threads[i].used && g_threads[i].pool_kind && !g_threads[i].finished && &g_threads[i] != w32_self()) busy = 1;
        if (!busy || g_exiting || w->exited) break;
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_change, &g_guest, &ts);
    }
}
static void k_CloseThreadpoolCleanupGroup(w32 *w) { (void)w; }
static void k_CallbackMayRunLong(w32 *w) { RET(1); }
static void k_DisassociateCurrentThreadFromCallback(w32 *w) { (void)w; }
static void k_FreeLibraryWhenCallbackReturns(w32 *w) { (void)w; }
/* (instance, event): the instance is the object; the event is set when the
 * callback returns, in pool_done */
static void k_SetEventWhenCallbackReturns(w32 *w) { if (ARG(0) && w32_mem_ok(w, ARG(0), PO_SIZE)) w32_write(w, ARG(0) + PO_EVENT, 8, ARG(1)); }
static void k_ReleaseSemaphoreWhenCallbackReturns(w32 *w) {
    /* released now rather than at return: a semaphore released a little
     * early is a race nobody has lost yet, and there is one slot to remember one thing in */
    w32_handle *h = w32_handle_get(w, ARG(1));
    if (h) { h->u1 += ARG(2); h->flags |= 1u; pthread_cond_broadcast(&g_change); }
}
static void k_ReleaseMutexWhenCallbackReturns(w32 *w) { (void)w; }
static void k_LeaveCriticalSectionWhenCallbackReturns(w32 *w) { (void)w; }

/* The one path that starts a thread. CreateThread, _beginthreadex and
 * _beginthread all reach it; they differ only in which argument is which, so
 * they read their own arguments and call this with them named. Returns the
 * handle, or 0 with the last error set. */
static int spawn_launch(w32 *w, w32_thread *t);
static uint64_t spawn_prepare(w32 *w, uint64_t entry, uint64_t param, uint64_t stack_size, uint32_t *out_id, w32_thread **out_thread) {
    g_w = w;
    if (!entry) { w32_set_last_error(w, 87); return 0; }        /* ERROR_INVALID_PARAMETER */
    if (g_nthreads >= W32_MAX_THREADS) {
        fprintf(stderr, "winrun: CreateThread: already %d threads\n", g_nthreads);
        w32_set_last_error(w, 8);                               /* ERROR_NOT_ENOUGH_MEMORY */
        return 0;
    }
    w32_thread *t = thread_alloc();
    if (!t) { w32_set_last_error(w, 8); return 0; }

    uint64_t size = stack_size ? stack_size : (1u << 20);
    if (size < (64u << 10)) size = 64u << 10;
    uint64_t stack = w32_alloc(w, size, 0);
    uint64_t teb = w32_alloc(w, 0x2000, 0);
    if (!stack || !teb) {
        t->used = 0;
        fprintf(stderr, "winrun: CreateThread: no guest memory for a stack and TEB\n");
        w32_set_last_error(w, 8);
        return 0;
    }
    t->stack_limit = stack;
    t->stack_base = stack + size;
    t->teb = teb;
    t->entry = entry;
    t->param = param;
    t->id = 5000 + (uint32_t)(t - g_threads);
    t->c = &t->cpu;
    xc_cpu_init(t->c, w->is32 ? XC_MODE_32 : XC_MODE_64, w->mem);
    t->c->jit_base = w->is32 ? (uint64_t)(uintptr_t)w->base : 0;

    /* The TEB fields a thread reads about itself. The stack bounds matter:
     * the CRT probes them, and SEH checks that a registration record lies
     * inside them -- with the main thread's bounds, a handler on this stack
     * would be rejected as a corrupt chain. */
    if (w->is32) {
        w32_write(w, teb + 0x00, 4, 0xFFFFFFFFu);      /* ExceptionList: end of chain */
        w32_write(w, teb + 0x04, 4, t->stack_base);
        w32_write(w, teb + 0x08, 4, t->stack_limit);
        w32_write(w, teb + 0x18, 4, teb);              /* Self */
        w32_write(w, teb + 0x20, 4, 4242);             /* pid */
        w32_write(w, teb + 0x24, 4, t->id);            /* tid */
        w32_write(w, teb + TEB32_PEB, 4, w->peb);
    } else {
        w32_write(w, teb + 0x08, 8, t->stack_base);
        w32_write(w, teb + 0x10, 8, t->stack_limit);
        w32_write(w, teb + 0x30, 8, teb);              /* Self */
        w32_write(w, teb + 0x40, 4, 4242);
        w32_write(w, teb + 0x48, 4, t->id);
        w32_write(w, teb + TEB64_PEB, 8, w->peb);
    }
    w32_tls_thread_init(w, t);                         /* its own copies of every static TLS block */

    uint64_t h = w32_handle_new(w, H_THREAD, -1);
    w32_handle *hh = w32_handle_get(w, h);
    if (hh) { hh->flags = 0; hh->u1 = t->id; }         /* unsignalled until it finishes */
    t->handle = h;
    if (out_id) *out_id = t->id;
    if (out_thread) { *out_thread = t; return h; }     /* the pool fills in the job, then launches */
    return spawn_launch(w, t) ? h : 0;
}
static int spawn_launch(w32 *w, w32_thread *t) {
    g_nthreads++;
    if (pthread_create(&t->host, 0, thread_body, t)) {
        g_nthreads--;
        t->used = 0;
        w32_handle_close(w, t->handle);
        fprintf(stderr, "winrun: CreateThread: the host refused a thread\n");
        w32_set_last_error(w, 8);
        return 0;
    }
    pthread_detach(t->host);
    if (w->verbose) fprintf(stderr, "winrun: thread %u started at %#llx%s\n",
                            t->id, (unsigned long long)t->entry, t->pool_kind ? " (thread pool)" : "");
    return 1;
}
static uint64_t spawn(w32 *w, uint64_t entry, uint64_t param, uint64_t stack_size, uint32_t *out_id) {
    return spawn_prepare(w, entry, param, stack_size, out_id, 0);
}
/* a thread for the pool: a small stack, made but not yet running */
static uint64_t spawn_pool(w32 *w, w32_thread **out) {
    return spawn_prepare(w, 1, 0, 256u << 10, 0, out);
}

/* CreateThread(sa, stackSize, start, param, flags, pTid) */
static void k_CreateThread(w32 *w) {
    uint32_t id = 0;
    uint64_t h = spawn(w, ARG(2), ARG(3), ARG(1), &id);
    if (h && ARG(5)) w32_write(w, ARG(5), 4, id);
    /* CREATE_SUSPENDED is not honoured: nothing here can resume one, and a
     * thread that never starts is worse than one that starts early. Said out
     * loud, because a game relying on it would be subtly wrong. */
    if (h && ((uint32_t)ARG(4) & 4)) fprintf(stderr, "winrun: CreateThread: CREATE_SUSPENDED ignored\n");
    RET(h);
}

static void k_ExitThread(w32 *w) {
    w32_thread *t = w32_self();
    t->exit_code = (uint32_t)ARG(0);
    /* Returning to the return-to-host stub is how a thread body ends here, so
     * ExitThread jumps there rather than unwinding the host stack. */
    if (t == &g_threads[0]) { w32_exit(w, (int)(uint32_t)ARG(0)); return; }
    w32_return_to_host(w);
}
static void k_GetCurrentThreadId(w32 *w) { RET(w32_self()->id); }
/* The pseudo-handle Windows returns for "this thread": -2, and the bitness
 * matters because a 32-bit guest compares it against a 32-bit value. */
static void k_GetCurrentThread(w32 *w) { RET(w->is32 ? 0xFFFFFFFEu : ~1ull); }
static void k_GetThreadId(w32 *w) {
    w32_thread *t = thread_by_handle(ARG(0));
    RET(t ? t->id : 0);
}
static void k_GetExitCodeThread(w32 *w) {
    w32_thread *t = thread_by_handle(ARG(0));
    if (ARG(1)) w32_write(w, ARG(1), 4, t ? (t->finished ? t->exit_code : 259u) : 0u);  /* STILL_ACTIVE */
    RET(t ? 1 : 0);
}
/* Killing a thread from outside is not something this can do safely: it is
 * somewhere in guest code holding guest locks. Windows deprecated it for the
 * same reason. Saying so beats pretending. */
static void k_TerminateThread(w32 *w) {
    fprintf(stderr, "winrun: TerminateThread: not supported (the thread keeps running)\n");
    RET(0);
}
static void k_SuspendThread(w32 *w) { (void)w; RET(0xFFFFFFFFu); }
static void k_ResumeThread(w32 *w) { (void)w; RET(0xFFFFFFFFu); }
static void k_SwitchToThread(w32 *w) { w32_guest_yield(); RET(1); }

/* --- waiting ------------------------------------------------------------- */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* Is this handle signalled, and if the wait consumes it, consume it.
 * flags bit 0 = signalled, bit 1 = manual reset (an auto-reset object is
 * consumed by the wait that sees it, which is the whole difference). */
static int handle_ready(w32 *w, uint64_t hv, int consume) {
    w32_handle *h = w32_handle_get(w, hv);
    if (!h) return -1;                                  /* a bad handle is not a wait */
    if (h->type == H_THREAD) {
        w32_thread *t = thread_by_handle(hv);
        return t ? t->finished : 1;
    }
    if (h->type == H_MUTEX) {
        if (h->u2 == 0 || h->u2 == w32_self()->id) {    /* free, or already ours */
            if (consume) { h->u2 = w32_self()->id; h->u1++; }
            return 1;
        }
        return 0;
    }
    if (h->type == H_TIMER) {
        /* Ready once the due time has passed, and not before -- a timer that
         * says "ready" the moment it is set turns a frame-paced game into a
         * spin loop, and with one guest thread running at a time that starves
         * every other thread. GameMaker's frame thread is exactly that loop. */
        if (!h->u1) return 0;                           /* not armed */
        if (now_ms() < h->u1) return 0;
        if (consume) {
            if (h->u2) h->u1 = now_ms() + h->u2;        /* periodic: the next tick */
            else if (!(h->flags & 2)) h->u1 = 0;        /* a synchronisation timer is consumed */
        }
        return 1;                                       /* a manual-reset one stays ready until Set or Cancel */
    }
    if (!(h->flags & 1)) return 0;
    if (consume && !(h->flags & 2)) h->flags &= ~1u;    /* auto-reset */
    return 1;
}

/* How long a wait on this handle may sleep before looking again. Everything
 * but a timer becomes ready only when another thread signals it and
 * broadcasts, so the poll interval is just a backstop; a timer becomes ready
 * on its own, and sleeping past its due time is what makes it late. */
static uint64_t wait_cap_ms(w32 *w, uint64_t hv, uint64_t cap) {
    w32_handle *h = w32_handle_get(w, hv);
    if (!h || h->type != H_TIMER || !h->u1) return cap;
    uint64_t now = now_ms(), left = h->u1 > now ? h->u1 - now : 0;
    return left < cap ? left : cap;
}

/* Wait for one handle, releasing the guest lock so whatever will signal it
 * can run. */
static uint32_t wait_one(w32 *w, uint64_t hv, uint32_t ms) {
    uint64_t nap;
    uint64_t deadline = ms == INFINITE_ ? 0 : now_ms() + ms;
    for (;;) {
        w32_dsound_tick(w);                 /* a wait is where a sound's position event is noticed */
        w32_xaudio2_tick(w);
        w32_mmdevapi_tick(w);
        int r = handle_ready(w, hv, 1);
        if (r < 0) return WAIT_FAILED_;
        if (r) return WAIT_OBJECT_0_;
        if (g_exiting) return WAIT_FAILED_;
        if (ms == 0) return WAIT_TIMEOUT_;
        if (deadline && now_ms() >= deadline) return WAIT_TIMEOUT_;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        /* A bounded wait even when the caller said INFINITE: a guest that
         * waits forever on something nothing will signal should show up as a
         * hang in the run's own deadline, not as a thread the process can
         * never join. */
        nap = wait_cap_ms(w, hv, 20);
        if (!nap) nap = 1;
        ts.tv_nsec += (long)nap * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += ts.tv_nsec / 1000000000L; ts.tv_nsec %= 1000000000L; }
        pthread_cond_timedwait(&g_change, &g_guest, &ts);
    }
}

static void k_WaitForSingleObject(w32 *w) { RET(wait_one(w, ARG(0), (uint32_t)ARG(1))); }
static void k_WaitForSingleObjectEx(w32 *w) { RET(wait_one(w, ARG(0), (uint32_t)ARG(1))); }

/* WaitForMultipleObjects(count, handles, waitAll, ms) */
static void k_WaitForMultipleObjects(w32 *w) {
    uint32_t n = (uint32_t)ARG(0);
    uint64_t arr = ARG(1);
    int all = (int)ARG(2);
    uint32_t ms = (uint32_t)ARG(3);
    int psz = (int)w32_ptrsize(w);
    if (!n || n > 64 || !arr) { RET(WAIT_FAILED_); return; }
    uint64_t hs[64];
    for (uint32_t i = 0; i < n; i++) hs[i] = w32_read(w, arr + (uint64_t)psz * i, psz);

    uint64_t deadline = ms == INFINITE_ ? 0 : now_ms() + ms;
    for (;;) {
        w32_dsound_tick(w);
        w32_xaudio2_tick(w);
        w32_mmdevapi_tick(w);
        if (all) {
            /* Every one has to be ready *before* any is consumed, or a
             * partial wait eats a signal it is not going to act on. */
            int ready = 1;
            for (uint32_t i = 0; i < n && ready; i++) if (handle_ready(w, hs[i], 0) <= 0) ready = 0;
            if (ready) {
                for (uint32_t i = 0; i < n; i++) handle_ready(w, hs[i], 1);
                RET(WAIT_OBJECT_0_); return;
            }
        } else {
            for (uint32_t i = 0; i < n; i++) {
                int r = handle_ready(w, hs[i], 1);
                if (r < 0) { RET(WAIT_FAILED_); return; }
                if (r) { RET(WAIT_OBJECT_0_ + i); return; }
            }
        }
        if (g_exiting) { RET(WAIT_FAILED_); return; }
        if (ms == 0) { RET(WAIT_TIMEOUT_); return; }
        if (deadline && now_ms() >= deadline) { RET(WAIT_TIMEOUT_); return; }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t nap = 20;
        for (uint32_t i = 0; i < n; i++) nap = wait_cap_ms(w, hs[i], nap);
        if (!nap) nap = 1;
        ts.tv_nsec += (long)nap * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += ts.tv_nsec / 1000000000L; ts.tv_nsec %= 1000000000L; }
        pthread_cond_timedwait(&g_change, &g_guest, &ts);
    }
}

/* --- events, mutexes, semaphores ---------------------------------------- */

/* CreateEvent(sa, manualReset, initialState, name) */
static void ev_create(w32 *w, int wide) {
    uint64_t h = w32_handle_new(w, H_EVENT, -1);
    w32_handle *hh = w32_handle_get(w, h);
    if (hh) hh->flags = (ARG(2) ? 1u : 0u) | (ARG(1) ? 2u : 0u);
    (void)wide;
    RET(h);
}
static void k_CreateEventA(w32 *w) { ev_create(w, 0); }
static void k_CreateEventW(w32 *w) { ev_create(w, 1); }
/* CreateEventExW(attributes, name, flags, access): CREATE_EVENT_MANUAL_RESET
 * is 1, CREATE_EVENT_INITIAL_SET is 2 -- the same two facts as CreateEventW
 * carries in its two BOOLs. */
static void k_CreateEventExW(w32 *w) {
    uint32_t flags = (uint32_t)ARG(2);
    uint64_t h = w32_handle_new(w, H_EVENT, -1);
    w32_handle *hh = w32_handle_get(w, h);
    if (hh) hh->flags = ((flags & 2) ? 1u : 0u) | ((flags & 1) ? 2u : 0u);
    RET(h);
}
/* SetEvent from host code: dsound.c signals a buffer's position events from
 * the guest thread, where a program waiting on one is in the loop below. */
void w32_event_set(w32 *w, uint64_t h) {
    w32_handle *hh = w32_handle_get(w, h);
    if (hh) hh->flags |= 1u;
    pthread_cond_broadcast(&g_change);
}
static void k_SetEvent(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0));
    if (h) h->flags |= 1u;
    pthread_cond_broadcast(&g_change);
    RET(h ? 1 : 0);
}
static void k_ResetEvent(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0));
    if (h) h->flags &= ~1u;
    RET(h ? 1 : 0);
}
/* PulseEvent: release whoever is waiting now and go straight back to unset.
 * Broadcasting and clearing is the closest honest approximation; the exact
 * semantics are racy on Windows too, which is why it is deprecated there. */
static void k_PulseEvent(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0));
    if (h) { h->flags |= 1u; pthread_cond_broadcast(&g_change); h->flags &= ~1u; }
    RET(h ? 1 : 0);
}
static void mx_create(w32 *w) {
    uint64_t h = w32_handle_new(w, H_MUTEX, -1);
    w32_handle *hh = w32_handle_get(w, h);
    if (hh) { hh->u1 = 0; hh->u2 = ARG(1) ? w32_self()->id : 0; if (ARG(1)) hh->u1 = 1; }
    RET(h);
}
static void k_CreateMutexA(w32 *w) { mx_create(w); }
static void k_CreateMutexW(w32 *w) { mx_create(w); }
static void k_ReleaseMutex(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0));
    if (!h || h->u2 != w32_self()->id) { RET(0); return; }
    if (h->u1 && --h->u1 == 0) h->u2 = 0;
    pthread_cond_broadcast(&g_change);
    RET(1);
}
/* CreateSemaphore(sa, initial, max, name): u1 is the count, u2 the maximum. */
static void sem_create(w32 *w) {
    uint64_t h = w32_handle_new(w, H_EVENT, -1);
    w32_handle *hh = w32_handle_get(w, h);
    if (hh) { hh->flags = ARG(1) ? 1u : 0u; hh->u1 = ARG(1); hh->u2 = ARG(2); }
    RET(h);
}
static void k_CreateSemaphoreA(w32 *w) { sem_create(w); }
static void k_CreateSemaphoreW(w32 *w) { sem_create(w); }
static void k_CreateSemaphoreExW(w32 *w) { sem_create(w); }      /* (attributes, initial, maximum, name, flags, access): the first three are what matter */
static void k_ReleaseSemaphore(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0));
    if (!h) { RET(0); return; }
    if (ARG(2)) w32_write(w, ARG(2), 4, (uint32_t)h->u1);
    h->u1 += ARG(1);
    h->flags |= 1u;
    pthread_cond_broadcast(&g_change);
    RET(1);
}

/* --- critical sections --------------------------------------------------- */

/* Held in the guest's own CRITICAL_SECTION, at the offsets the real one uses,
 * because some CRT versions read LockCount and RecursionCount directly. */
static void k_InitializeCriticalSection(w32 *w) {
    uint64_t p = ARG(0);
    if (!p) return;
    int psz = (int)w32_ptrsize(w);
    w32_write(w, p + 0, psz, 0);
    w32_write(w, p + cs_off_lock(w), 4, 0xFFFFFFFFu);   /* -1 = unowned, as Windows */
    w32_write(w, p + cs_off_recur(w), 4, 0);
    w32_write(w, p + cs_off_owner(w), psz, 0);
}
static void k_InitializeCriticalSectionAndSpinCount(w32 *w) { k_InitializeCriticalSection(w); RET(1); }
static void k_InitializeCriticalSectionEx(w32 *w) { k_InitializeCriticalSection(w); RET(1); }
static void k_DeleteCriticalSection(w32 *w) { (void)w; }

static void k_EnterCriticalSection(w32 *w) {
    uint64_t p = ARG(0);
    if (!p) return;
    int psz = (int)w32_ptrsize(w);
    uint32_t me = w32_self()->id;
    for (;;) {
        uint64_t owner = w32_read(w, p + cs_off_owner(w), psz);
        if (!owner || owner == me) {
            w32_write(w, p + cs_off_owner(w), psz, me);
            uint32_t rec = (uint32_t)w32_read(w, p + cs_off_recur(w), 4) + 1;
            w32_write(w, p + cs_off_recur(w), 4, rec);
            w32_write(w, p + cs_off_lock(w), 4, (uint32_t)(rec - 1));
            return;
        }
        if (g_exiting) return;
        /* Owned by another thread, which can only make progress if this one
         * lets go of the guest lock. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 5 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_change, &g_guest, &ts);
    }
}
static void k_TryEnterCriticalSection(w32 *w) {
    uint64_t p = ARG(0);
    if (!p) { RET(0); return; }
    int psz = (int)w32_ptrsize(w);
    uint32_t me = w32_self()->id;
    uint64_t owner = w32_read(w, p + cs_off_owner(w), psz);
    if (owner && owner != me) { RET(0); return; }
    k_EnterCriticalSection(w);
    RET(1);
}
static void k_LeaveCriticalSection(w32 *w) {
    uint64_t p = ARG(0);
    if (!p) return;
    int psz = (int)w32_ptrsize(w);
    uint32_t rec = (uint32_t)w32_read(w, p + cs_off_recur(w), 4);
    if (rec) rec--;
    w32_write(w, p + cs_off_recur(w), 4, rec);
    if (!rec) {
        w32_write(w, p + cs_off_owner(w), psz, 0);
        w32_write(w, p + cs_off_lock(w), 4, 0xFFFFFFFFu);
        pthread_cond_broadcast(&g_change);
    }
}

/* --- sleeping ------------------------------------------------------------ */

/* Sleep has to give the lock up, or a game's frame limiter stops every other
 * thread for the length of the frame. */
static void k_Sleep(w32 *w) {
    uint32_t ms = (uint32_t)ARG(0);
    if (!ms) { w32_guest_yield(); return; }
    uint64_t until = now_ms() + ms;
    while (!g_exiting && now_ms() < until) {
        /* A sleeping thread is as good a place as a waiting one for the
         * sound queues to be serviced: an audio thread that paces itself
         * with Sleep rather than an event still has to be told what the
         * mixer finished. */
        w32_dsound_tick(w);
        w32_xaudio2_tick(w);
        w32_mmdevapi_tick(w);
        if (w->exited) return;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t left = until - now_ms();
        if (left > 20) left = 20;
        ts.tv_nsec += (long)left * 1000000L;
        while (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_change, &g_guest, &ts);
    }
}
static void k_SleepEx(w32 *w) { k_Sleep(w); RET(0); }

/* --- condition variables -------------------------------------------------
 *
 * SleepConditionVariable releases a lock, waits to be woken, and takes the
 * lock back. The C runtime's own threading is built on these, so a program
 * that never calls one directly still deadlocks without them.
 *
 * `g_change` is broadcast by everything that changes shared state here -- an
 * event being set, a critical section being released, a thread ending -- so
 * waiting on it is waiting for "something happened", which is a superset of
 * what the guest's own Wake call means. Waking too often is correct for a
 * condition variable: the contract is that the predicate must be rechecked
 * in a loop, and every caller does. Waking too rarely would not be.
 *
 * The lock is either a critical section or an SRW lock. An SRW lock here is
 * a pointer-sized word the guest owns and we do not interpret, so releasing
 * it is writing zero and taking it is waiting for zero.
 */
static void cs_leave(w32 *w, uint64_t p) {
    int psz = (int)w32_ptrsize(w);
    uint32_t rec = (uint32_t)w32_read(w, p + cs_off_recur(w), 4);
    if (rec) rec--;
    w32_write(w, p + cs_off_recur(w), 4, rec);
    if (!rec) {
        w32_write(w, p + cs_off_owner(w), psz, 0);
        w32_write(w, p + cs_off_lock(w), 4, 0xFFFFFFFFu);
        pthread_cond_broadcast(&g_change);
    }
}
static void cs_enter(w32 *w, uint64_t p) {
    int psz = (int)w32_ptrsize(w);
    uint32_t me = w32_self()->id;
    for (;;) {
        uint64_t owner = w32_read(w, p + cs_off_owner(w), psz);
        if (!owner || owner == me) {
            w32_write(w, p + cs_off_owner(w), psz, me);
            uint32_t rec = (uint32_t)w32_read(w, p + cs_off_recur(w), 4) + 1;
            w32_write(w, p + cs_off_recur(w), 4, rec);
            w32_write(w, p + cs_off_lock(w), 4, (uint32_t)(rec - 1));
            return;
        }
        if (g_exiting) return;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 5 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_change, &g_guest, &ts);
    }
}

/* Non-zero if woken, zero on timeout -- with the last error set, which is
 * how a caller tells the two apart. */
int w32_cond_sleep(w32 *w, uint64_t cv, uint64_t lock, uint32_t ms, int srw) {
    (void)cv;
    if (!lock) return 0;
    int psz = (int)w32_ptrsize(w);
    if (srw) w32_write(w, lock, psz, 0);
    else cs_leave(w, lock);
    pthread_cond_broadcast(&g_change);

    uint64_t until = ms == 0xFFFFFFFFu ? 0 : now_ms() + ms;
    int woken = 0;
    for (;;) {
        if (g_exiting) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t slice = 20;
        if (until) {
            uint64_t nowv = now_ms();
            if (nowv >= until) break;
            if (until - nowv < slice) slice = until - nowv;
        }
        ts.tv_nsec += (long)slice * 1000000L;
        while (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        if (pthread_cond_timedwait(&g_change, &g_guest, &ts) == 0) { woken = 1; break; }
        /* An infinite wait must still give the caller a chance to recheck its
         * predicate: one pass is a spurious wake-up, which the contract
         * explicitly allows and every caller handles. */
        if (!until) { woken = 1; break; }
    }
    if (srw) {
        while (!g_exiting && w32_read(w, lock, psz)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 5 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&g_change, &g_guest, &ts);
        }
        w32_write(w, lock, psz, 1);
    } else cs_enter(w, lock);

    if (!woken) w32_set_last_error(w, 1460);       /* ERROR_TIMEOUT */
    return woken;
}

/* An event made from the host side: a waitable timer is one. */
uint64_t w32_make_event(w32 *w, int manual, int set) {
    uint64_t h = w32_handle_new(w, H_EVENT, -1);
    w32_handle *hh = w32_handle_get(w, h);
    if (hh) hh->flags = (set ? 1u : 0u) | (manual ? 2u : 0u);
    return h;
}
void w32_set_event(w32 *w, uint64_t h, int on) {
    w32_handle *hh = w32_handle_get(w, h);
    if (hh) { if (on) hh->flags |= 1u; else hh->flags &= ~1u; }
    pthread_cond_broadcast(&g_change);
}
uint64_t w32_make_timer(w32 *w, int manual) {
    uint64_t h = w32_handle_new(w, H_TIMER, -1);
    w32_handle *hh = w32_handle_get(w, h);
    if (hh) { hh->flags = manual ? 2u : 0u; hh->u1 = 0; hh->u2 = 0; }   /* created disarmed */
    return h;
}
void w32_timer_set(w32 *w, uint64_t h, uint64_t delay_ms, uint32_t period_ms) {
    w32_handle *hh = w32_handle_get(w, h);
    if (!hh || hh->type != H_TIMER) return;
    hh->u1 = now_ms() + delay_ms;
    hh->u2 = period_ms;
    hh->flags &= ~1u;
    pthread_cond_broadcast(&g_change);
}
void w32_timer_cancel(w32 *w, uint64_t h) {
    w32_handle *hh = w32_handle_get(w, h);
    if (!hh || hh->type != H_TIMER) return;
    hh->u1 = 0; hh->u2 = 0;
    pthread_cond_broadcast(&g_change);
}
/* ExitThread's body, callable from elsewhere: FreeLibraryAndExitThread is
 * ExitThread with a FreeLibrary in front of it, and nothing here unmaps. */
void w32_thread_exit_self(w32 *w, uint32_t code) {
    w32_thread *t = w32_self();
    t->exit_code = code;
    if (t == &g_threads[0]) { w32_exit(w, (int)code); return; }
    w32_return_to_host(w);
}

/* --- interlocked -------------------------------------------------------- */

/* Under the guest lock these cannot be interrupted by another guest thread,
 * so a plain read-modify-write is already atomic with respect to the only
 * other writers there are. Written as one when the lock goes away. */
static void k_InterlockedIncrement(w32 *w) { uint64_t p = ARG(0); uint32_t v = (uint32_t)w32_read(w, p, 4) + 1; w32_write(w, p, 4, v); RET((uint64_t)(int64_t)(int32_t)v); }
static void k_InterlockedDecrement(w32 *w) { uint64_t p = ARG(0); uint32_t v = (uint32_t)w32_read(w, p, 4) - 1; w32_write(w, p, 4, v); RET((uint64_t)(int64_t)(int32_t)v); }
static void k_InterlockedExchange(w32 *w) { uint64_t p = ARG(0); uint32_t old = (uint32_t)w32_read(w, p, 4); w32_write(w, p, 4, ARG(1)); RET(old); }
static void k_InterlockedExchangeAdd(w32 *w) { uint64_t p = ARG(0); uint32_t old = (uint32_t)w32_read(w, p, 4); w32_write(w, p, 4, old + (uint32_t)ARG(1)); RET(old); }
static void k_InterlockedCompareExchange(w32 *w) { uint64_t p = ARG(0); uint32_t old = (uint32_t)w32_read(w, p, 4); if (old == (uint32_t)ARG(2)) w32_write(w, p, 4, ARG(1)); RET(old); }
static void k_InterlockedExchangePointer(w32 *w) {
    uint64_t p = ARG(0); int psz = (int)w32_ptrsize(w);
    uint64_t old = w32_read(w, p, psz);
    w32_write(w, p, psz, ARG(1));
    w32_ret64(w, old);
}
static void k_InterlockedCompareExchangePointer(w32 *w) {
    uint64_t p = ARG(0); int psz = (int)w32_ptrsize(w);
    uint64_t old = w32_read(w, p, psz);
    if (old == ARG(2)) w32_write(w, p, psz, ARG(1));
    w32_ret64(w, old);
}

/* --- msvcrt's thread entry points --------------------------------------- */

/* _beginthreadex(security, stackSize, start, arg, initflag, pTid) has
 * CreateThread's arguments in the same order and returns the handle as a
 * uintptr_t; _beginthread(start, stackSize, arg) does not, so it names its
 * own. Both go through spawn rather than through each other, because
 * "identical apart from the argument order" is exactly the kind of sameness
 * that stops being true. */
static void k_beginthreadex(w32 *w) {
    uint32_t id = 0;
    uint64_t h = spawn(w, ARG(2), ARG(3), ARG(1), &id);
    if (h && ARG(5)) w32_write(w, ARG(5), 4, id);
    RET(h);
}
static void k_beginthread(w32 *w) { RET(spawn(w, ARG(0), ARG(2), ARG(1), 0)); }
static void k_endthreadex(w32 *w) { k_ExitThread(w); }
static void k_endthread(w32 *w) {
    w32_thread *t = w32_self();
    t->exit_code = 0;
    if (t == &g_threads[0]) w32_exit(w, 0);
    else w32_return_to_host(w);
}

void w32_thread_reset(void) {
    memset(g_threads, 0, sizeof g_threads);
    g_nthreads = 0;
    g_exiting = 0;
    g_w = 0;
}

int w32_thread_count(void) { return g_nthreads; }

#define F(n, a)  { #n, a, 0, k_##n, 0 }
#define FC(n, a) { #n, a, 1, k_##n, 0 }          /* cdecl: msvcrt */
const w32_api w32_thread_api[] = {
    F(CreateThread, 6), F(ExitThread, 1), F(TerminateThread, 2),
    F(GetCurrentThreadId, 0), F(GetCurrentThread, 0), F(GetThreadId, 1),
    F(GetExitCodeThread, 2), F(SuspendThread, 1), F(ResumeThread, 1), F(SwitchToThread, 0),
    F(WaitForSingleObject, 2), F(WaitForSingleObjectEx, 3), F(WaitForMultipleObjects, 4),
    F(CreateEventA, 4), F(CreateEventW, 4), F(SetEvent, 1), F(ResetEvent, 1), F(PulseEvent, 1),
    F(CreateMutexA, 3), F(CreateMutexW, 3), F(ReleaseMutex, 1),
    F(CreateSemaphoreA, 4), F(CreateSemaphoreW, 4), F(ReleaseSemaphore, 3),
    F(InitializeCriticalSection, 1), F(InitializeCriticalSectionAndSpinCount, 2),
    F(InitializeCriticalSectionEx, 3), F(DeleteCriticalSection, 1),
    F(EnterCriticalSection, 1), F(LeaveCriticalSection, 1), F(TryEnterCriticalSection, 1),
    F(Sleep, 1), F(SleepEx, 2),
    F(InterlockedIncrement, 1), F(InterlockedDecrement, 1), F(InterlockedExchange, 2),
    F(InterlockedExchangeAdd, 2), F(InterlockedCompareExchange, 3),
    F(InterlockedExchangePointer, 2), F(InterlockedCompareExchangePointer, 3),
    F(CreateEventExW, 4), F(CreateSemaphoreExW, 6),
    /* the thread pool */
    F(CreateThreadpoolWork, 3), F(SubmitThreadpoolWork, 1), F(CreateThreadpoolTimer, 3), F(SetThreadpoolTimer, 4),
    F(IsThreadpoolTimerSet, 1), F(CreateThreadpoolWait, 3), F(SetThreadpoolWait, 3), F(TrySubmitThreadpoolCallback, 3),
    { "WaitForThreadpoolWorkCallbacks",  2, 0, k_WaitForThreadpoolCallbacks, 0 },
    { "WaitForThreadpoolTimerCallbacks", 2, 0, k_WaitForThreadpoolCallbacks, 0 },
    { "WaitForThreadpoolWaitCallbacks",  2, 0, k_WaitForThreadpoolCallbacks, 0 },
    { "CloseThreadpoolWork",  1, 0, k_CloseThreadpoolObject, 0 },
    { "CloseThreadpoolTimer", 1, 0, k_CloseThreadpoolObject, 0 },
    { "CloseThreadpoolWait",  1, 0, k_CloseThreadpoolObject, 0 },
    F(CreateThreadpool, 1), F(CloseThreadpool, 1), F(SetThreadpoolThreadMaximum, 2), F(SetThreadpoolThreadMinimum, 2),
    F(CreateThreadpoolCleanupGroup, 0), F(CloseThreadpoolCleanupGroupMembers, 3), F(CloseThreadpoolCleanupGroup, 1),
    F(CallbackMayRunLong, 1), F(DisassociateCurrentThreadFromCallback, 1), F(FreeLibraryWhenCallbackReturns, 2),
    F(SetEventWhenCallbackReturns, 2), F(ReleaseSemaphoreWhenCallbackReturns, 3), F(ReleaseMutexWhenCallbackReturns, 2),
    F(LeaveCriticalSectionWhenCallbackReturns, 2),
    { 0, 0, 0, 0, 0 },
};
const w32_api w32_thread_crt[] = {
    FC(beginthreadex, 6), FC(beginthread, 3), FC(endthreadex, 1), FC(endthread, 0),
    { 0, 0, 0, 0, 0 },
};
