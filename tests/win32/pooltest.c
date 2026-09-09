/* The thread pool, and an honest GetProcAddress.
 *
 * A C runtime probes kernel32 for the functions newer than its minimum
 * Windows -- CreateEventExW, CreateThreadpoolTimer and friends -- and takes
 * a fallback when GetProcAddress says NULL. It used to be handed a stub for
 * anything, called it, got a made-up value and failed fast; that was the
 * Spamton runner's exit at 147 ms. So: NULL for a name that is not here,
 * with ERROR_PROC_NOT_FOUND, and a real address for one that is.
 *
 * Then the pool itself: a work item runs and can be waited for, a timer
 * fires once and then periodically until it is set to nothing, a wait fires
 * when its handle is signalled and says so, TrySubmitThreadpoolCallback
 * runs, and the "when the callback returns" event is set.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static int fails;
static void ok(int c, const char *what) { printf("%s %s\n", c ? "ok  " : "FAIL", what); if (!c) fails++; }

static volatile LONG work_runs, timer_runs, wait_runs, simple_runs;
static volatile DWORD wait_result = 12345;
static void CALLBACK work_cb(PTP_CALLBACK_INSTANCE inst, void *ctx, PTP_WORK work) { (void)inst; (void)work; InterlockedIncrement((LONG *)ctx); }
static void CALLBACK timer_cb(PTP_CALLBACK_INSTANCE inst, void *ctx, PTP_TIMER timer) { (void)inst; (void)ctx; (void)timer; InterlockedIncrement(&timer_runs); }
static void CALLBACK wait_cb(PTP_CALLBACK_INSTANCE inst, void *ctx, PTP_WAIT wait, TP_WAIT_RESULT r) { (void)inst; (void)ctx; (void)wait; wait_result = r; InterlockedIncrement(&wait_runs); }
static void CALLBACK simple_cb(PTP_CALLBACK_INSTANCE inst, void *ctx) { (void)ctx; SetEventWhenCallbackReturns(inst, (HANDLE)ctx); InterlockedIncrement(&simple_runs); }

int main(void) {
    printf("%d-bit\n", (int)(8 * sizeof(void *)));
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    ok(k32 != NULL, "GetModuleHandle(kernel32)");
    SetLastError(0);
    FARPROC none = GetProcAddress(k32, "NoSuchFunctionAnywhere");
    ok(none == NULL && GetLastError() == ERROR_PROC_NOT_FOUND, "GetProcAddress of a name that does not exist: NULL, ERROR_PROC_NOT_FOUND");
    ok(GetProcAddress(k32, "CreateEventExW") != NULL, "GetProcAddress(CreateEventExW): found");
    ok(GetProcAddress(k32, "CreateThreadpoolTimer") != NULL, "GetProcAddress(CreateThreadpoolTimer): found");
    ok(GetProcAddress(k32, "GetTickCount") != NULL, "GetProcAddress(GetTickCount): found");

    HANDLE ev = CreateEventExW(NULL, NULL, CREATE_EVENT_MANUAL_RESET | CREATE_EVENT_INITIAL_SET, EVENT_ALL_ACCESS);
    ok(ev != NULL && WaitForSingleObject(ev, 0) == WAIT_OBJECT_0 && WaitForSingleObject(ev, 0) == WAIT_OBJECT_0, "CreateEventExW: manual reset, initially set, stays set");
    HANDLE sem = CreateSemaphoreExW(NULL, 1, 4, NULL, 0, SEMAPHORE_ALL_ACCESS);
    ok(sem != NULL && WaitForSingleObject(sem, 0) == WAIT_OBJECT_0 && WaitForSingleObject(sem, 0) == WAIT_TIMEOUT, "CreateSemaphoreExW: one count, then none");

    /* work */
    PTP_WORK work = CreateThreadpoolWork(work_cb, (void *)&work_runs, NULL);
    ok(work != NULL, "CreateThreadpoolWork");
    for (int i = 0; i < 4; i++) SubmitThreadpoolWork(work);
    WaitForThreadpoolWorkCallbacks(work, FALSE);
    ok(work_runs == 4, "four submissions ran by the time WaitForThreadpoolWorkCallbacks returned");
    if (work_runs != 4) printf("  (%ld ran)\n", (long)work_runs);
    CloseThreadpoolWork(work);

    /* timer: due in 20 ms, then every 30 ms */
    PTP_TIMER timer = CreateThreadpoolTimer(timer_cb, NULL, NULL);
    ok(timer != NULL, "CreateThreadpoolTimer");
    ok(!IsThreadpoolTimerSet(timer), "  not set yet");
    LARGE_INTEGER due; due.QuadPart = -200000LL;                /* 20 ms, relative */
    FILETIME ft; ft.dwLowDateTime = due.LowPart; ft.dwHighDateTime = (DWORD)due.HighPart;
    SetThreadpoolTimer(timer, &ft, 30, 0);
    ok(IsThreadpoolTimerSet(timer), "  set");
    Sleep(400);
    LONG runs = timer_runs;
    ok(runs >= 3, "  a periodic timer fired repeatedly in 400 ms");
    printf("  (%ld firings)\n", (long)runs);
    SetThreadpoolTimer(timer, NULL, 0, 0);                        /* cancel */
    WaitForThreadpoolTimerCallbacks(timer, TRUE);
    LONG after = timer_runs;
    Sleep(150);
    ok(timer_runs == after, "  and stopped once set to nothing");
    if (timer_runs != after) printf("  (%ld more after the cancel)\n", (long)(timer_runs - after));
    CloseThreadpoolTimer(timer);

    /* wait: on an event we set from here */
    HANDLE wev = CreateEventW(NULL, TRUE, FALSE, NULL);
    PTP_WAIT wait = CreateThreadpoolWait(wait_cb, NULL, NULL);
    ok(wait != NULL, "CreateThreadpoolWait");
    SetThreadpoolWait(wait, wev, NULL);
    Sleep(50);
    ok(wait_runs == 0, "  nothing fires before the handle is signalled");
    SetEvent(wev);
    WaitForThreadpoolWaitCallbacks(wait, FALSE);
    ok(wait_runs == 1 && wait_result == WAIT_OBJECT_0, "  the callback ran with WAIT_OBJECT_0 once the event was set");
    if (wait_runs != 1 || wait_result != WAIT_OBJECT_0) printf("  (runs %ld, result %lu)\n", (long)wait_runs, (unsigned long)wait_result);
    CloseThreadpoolWait(wait);

    /* a one-off callback, and the event it sets on return */
    HANDLE done = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(TrySubmitThreadpoolCallback(simple_cb, done, NULL), "TrySubmitThreadpoolCallback");
    DWORD dw = WaitForSingleObject(done, 2000);
    ok(dw == WAIT_OBJECT_0 && simple_runs == 1, "  SetEventWhenCallbackReturns set the event after it ran");
    if (dw != WAIT_OBJECT_0 || simple_runs != 1) printf("  (wait %lu, runs %ld)\n", (unsigned long)dw, (long)simple_runs);

    /* cleanup groups and a pool: accepted */
    PTP_POOL pool = CreateThreadpool(NULL);
    ok(pool != NULL && SetThreadpoolThreadMinimum(pool, 1), "CreateThreadpool, SetThreadpoolThreadMinimum");
    SetThreadpoolThreadMaximum(pool, 4);
    PTP_CLEANUP_GROUP cg = CreateThreadpoolCleanupGroup();
    ok(cg != NULL, "CreateThreadpoolCleanupGroup");
    CloseThreadpoolCleanupGroupMembers(cg, FALSE, NULL);
    CloseThreadpoolCleanupGroup(cg);
    CloseThreadpool(pool);

    printf("%d failures\n", fails);
    return fails ? 1 : 0;
}
