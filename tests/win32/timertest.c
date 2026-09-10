/* Waitable timers: a wait on one has to block until the timer is due.
 *
 * This is the property that matters, and the one that was missing. A timer
 * that reports ready the moment it is set makes WaitForSingleObject return
 * instantly, and a game that paces its frames on one then spins as fast as
 * the emulator will go -- which, with one guest thread running at a time,
 * starves whatever thread is doing the real work. GameMaker's runner sat on
 * a black screen for exactly that reason.
 *
 * Every check is one-sided: a wait may take longer than asked (a shared
 * machine, a slow emulator, a coarse poll), so lateness is never a failure.
 * Returning *early* is, and that is what is measured -- against
 * QueryPerformanceCounter rather than against a Sleep that was trusted to
 * take the time it was given. Nothing prints a duration, so the recording
 * does not depend on this machine's speed.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static LARGE_INTEGER g_freq;
static double now_ms(void) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return 1000.0 * (double)t.QuadPart / (double)g_freq.QuadPart;
}
/* relative due time: 100 ns units, negative */
static LARGE_INTEGER after_ms(int ms) { LARGE_INTEGER d; d.QuadPart = -(LONGLONG)ms * 10000; return d; }

int fail;
static void ok(const char *what, int cond) { printf("%s: %s\n", what, cond ? "yes" : "NO"); if (!cond) fail++; }

int main(void) {
    QueryPerformanceFrequency(&g_freq);
    printf("%d-bit\n\n", (int)(8 * sizeof(void *)));

    HANDLE t = CreateWaitableTimerA(NULL, TRUE, NULL);      /* manual reset */
    ok("CreateWaitableTimer", t != NULL);
    if (!t) return 1;

    /* an unset timer is not signalled */
    ok("an unset timer is not signalled", WaitForSingleObject(t, 0) == WAIT_TIMEOUT);

    /* the one that matters: a 200 ms timer does not fire early */
    LARGE_INTEGER due = after_ms(200);
    ok("SetWaitableTimer", SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE) != 0);
    ok("not signalled before it is due", WaitForSingleObject(t, 0) == WAIT_TIMEOUT);
    double t0 = now_ms();
    DWORD r = WaitForSingleObject(t, 10000);
    double waited = now_ms() - t0;
    ok("the wait returned because the timer fired", r == WAIT_OBJECT_0);
    ok("it waited the time it was given, not zero", waited >= 180.0);

    /* manual reset: it stays signalled once it has fired */
    ok("a manual-reset timer stays signalled", WaitForSingleObject(t, 0) == WAIT_OBJECT_0 &&
                                               WaitForSingleObject(t, 0) == WAIT_OBJECT_0);
    ok("CancelWaitableTimer clears it", CancelWaitableTimer(t) && WaitForSingleObject(t, 0) == WAIT_TIMEOUT);
    CloseHandle(t);

    /* a synchronisation timer is consumed by the wait that takes it */
    HANDLE s = CreateWaitableTimerA(NULL, FALSE, NULL);
    due = after_ms(50);
    SetWaitableTimer(s, &due, 0, NULL, NULL, FALSE);
    ok("a synchronisation timer fires once", WaitForSingleObject(s, 10000) == WAIT_OBJECT_0);
    ok("...and the wait consumed it", WaitForSingleObject(s, 0) == WAIT_TIMEOUT);
    CloseHandle(s);

    /* periodic: five 60 ms ticks cannot arrive in less than four periods */
    HANDLE p = CreateWaitableTimerA(NULL, FALSE, NULL);
    due = after_ms(60);
    ok("SetWaitableTimer with a period", SetWaitableTimer(p, &due, 60, NULL, NULL, FALSE) != 0);
    t0 = now_ms();
    int ticks = 0;
    for (int i = 0; i < 5; i++) if (WaitForSingleObject(p, 10000) == WAIT_OBJECT_0) ticks++;
    waited = now_ms() - t0;
    ok("five ticks arrived", ticks == 5);
    ok("they took at least four periods", waited >= 240.0);
    ok("CancelWaitableTimer stops the period", CancelWaitableTimer(p) && WaitForSingleObject(p, 0) == WAIT_TIMEOUT);
    CloseHandle(p);

    /* a timeout shorter than the timer wins the race */
    HANDLE q = CreateWaitableTimerA(NULL, TRUE, NULL);
    due = after_ms(5000);
    SetWaitableTimer(q, &due, 0, NULL, NULL, FALSE);
    t0 = now_ms();
    ok("a wait shorter than the timer times out", WaitForSingleObject(q, 100) == WAIT_TIMEOUT);
    ok("...and it did not return instantly", now_ms() - t0 >= 90.0);
    CloseHandle(q);

    printf("\n%d failures\n", fail);
    return fail ? 1 : 0;
}
