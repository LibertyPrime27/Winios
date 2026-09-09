/* Guest threads.
 *
 * Everything here is written to be *order-independent*, because thread
 * interleaving is not deterministic and a recorded expectation that depended
 * on it would be a test of this machine's scheduler. So nothing prints from a
 * worker thread and nothing prints a count that a race could change: what is
 * checked is the arithmetic that must come out the same however the threads
 * were interleaved.
 *
 * That is the whole discipline of testing concurrency against a recording.
 * "Four threads each added 25000, so the total is 100000" is true under every
 * interleaving *if the locking works* and reliably false if it does not --
 * which makes it a better test than watching them take turns.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define NTHREADS 4
#define PER_THREAD 400

static CRITICAL_SECTION g_cs;
static volatile LONG g_guarded;        /* incremented under the critical section */
static volatile LONG g_interlocked;    /* incremented with InterlockedIncrement */
static volatile LONG g_ran;            /* how many threads actually started */

/* Static (compiler) TLS, read the way MSVC-built code reads a
 * __declspec(thread) variable: TEB.ThreadLocalStoragePointer -> the module's
 * slot (_tls_index) -> the thread's copy of the image's TLS template, with
 * no null check anywhere. A thread that was not given its own copy faults on
 * the first access; a thread given the loader's copy sees another thread's
 * writes. GameMaker's frame timer died exactly this way.
 *
 * mingw's own __thread goes through its emulated-TLS runtime and never
 * touches the TEB, so it cannot stand in for that. Instead the variables are
 * placed in the .tls$ section by hand -- the linker sorts them between the
 * CRT's _tls_start and _tls_end, so they are in the template the loader
 * copies -- and read through the TEB explicitly. */
extern ULONG _tls_index;                             /* the CRT's; the loader writes it */
extern int _tls_start;                               /* first byte of the template */
__attribute__((section(".tls$B"))) static LONG t_mine = 7;
__attribute__((section(".tls$B"))) static LONG t_zero = 0;
static volatile LONG g_tls_ok;                       /* threads whose copy was theirs alone */
static volatile LONG g_tls_null;                     /* threads with no TLS pointer at all */

static LONG *tls_here(LONG *templ) {
    /* mingw keeps struct _TEB opaque; ThreadLocalStoragePointer is at 0x58
     * in the 64-bit TEB and 0x2C in the 32-bit one, the offsets the compiler
     * hard-codes into every __declspec(thread) access */
    char **slots = *(char ***)((char *)NtCurrentTeb() + (sizeof(void *) == 8 ? 0x58 : 0x2C));
    if (!slots || !slots[_tls_index]) return NULL;
    return (LONG *)(slots[_tls_index] + ((char *)templ - (char *)&_tls_start));
}

static DWORD WINAPI worker(LPVOID p) {
    (void)p;
    InterlockedIncrement(&g_ran);
    LONG *mine = tls_here(&t_mine), *zero = tls_here(&t_zero);
    if (!mine || !zero) { InterlockedIncrement(&g_tls_null); return 2; }
    int ok = *mine == 7 && *zero == 0;                /* a fresh copy of the template */
    *mine = 1000 + (LONG)GetCurrentThreadId();
    *zero = 1;
    for (int i = 0; i < 20; i++) {                    /* let the others run over it, if it is shared */
        Sleep(0);
        if (*mine != 1000 + (LONG)GetCurrentThreadId() || *zero != 1) ok = 0;
    }
    if (ok) InterlockedIncrement(&g_tls_ok);
    for (int i = 0; i < PER_THREAD; i++) {
        /* A read-modify-write with a deliberate handover in the middle of it.
         *
         * Sleep(0) yields, so if the critical section does not exclude, this
         * loses almost every increment rather than three in a hundred
         * thousand. Making the race *certain* is the point: a concurrency bug
         * that shows up one time in thirty thousand is one that passes CI and
         * fails on a device, and a test that only catches it sometimes is
         * worse than no test, because it teaches you to rerun.
         *
         * With the section working, the total is exact under every possible
         * interleaving. That is what makes it checkable against a recording. */
        EnterCriticalSection(&g_cs);
        LONG v = g_guarded;
        Sleep(0);
        g_guarded = v + 1;
        LeaveCriticalSection(&g_cs);
        InterlockedIncrement(&g_interlocked);
    }
    return 0x1234;
}

/* An event ping-pong: each side waits for the other, so neither can finish
 * unless waits really block and signals really wake. A broken wait that
 * returns immediately makes this spin and finish with the wrong count; a
 * broken signal makes it hang, which the run's time limit catches. */
static HANDLE g_ping, g_pong;
static volatile LONG g_bounces;
static DWORD WINAPI bouncer(LPVOID p) {
    (void)p;
    for (int i = 0; i < 50; i++) {
        if (WaitForSingleObject(g_ping, 5000) != WAIT_OBJECT_0) return 1;
        InterlockedIncrement(&g_bounces);
        SetEvent(g_pong);
    }
    return 0;
}

int main(void) {
    printf("%d-bit\n", (int)(8 * sizeof(void *)));

    /* --- do threads run at all --- */
    InitializeCriticalSection(&g_cs);
    HANDLE th[NTHREADS];
    DWORD tid[NTHREADS];
    int made = 0;
    for (int i = 0; i < NTHREADS; i++) {
        th[i] = CreateThread(NULL, 0, worker, NULL, 0, &tid[i]);
        if (th[i]) made++;
    }
    printf("\nCreateThread x%d: %d created\n", NTHREADS, made);
    if (made != NTHREADS) return 1;

    /* every thread id distinct, and none of them ours */
    DWORD me = GetCurrentThreadId();
    int distinct = 1;
    for (int i = 0; i < made; i++) {
        if (tid[i] == me) distinct = 0;
        for (int j = i + 1; j < made; j++) if (tid[i] == tid[j]) distinct = 0;
    }
    printf("thread ids distinct and none is the caller's: %s\n", distinct ? "yes" : "NO");

    DWORD wr = WaitForMultipleObjects((DWORD)made, th, TRUE, 30000);
    printf("WaitForMultipleObjects(all): %s\n",
           wr == WAIT_OBJECT_0 ? "all finished" : wr == WAIT_TIMEOUT ? "TIMED OUT" : "FAILED");

    printf("threads that ran: %ld (want %d)\n", (long)g_ran, NTHREADS);
    printf("guarded total: %ld (want %d)\n", (long)g_guarded, NTHREADS * PER_THREAD);
    printf("interlocked total: %ld (want %d)\n", (long)g_interlocked, NTHREADS * PER_THREAD);
    printf("both totals agree: %s\n", g_guarded == g_interlocked ? "yes" : "NO");
    {
        LONG *mine = tls_here(&t_mine), *zero = tls_here(&t_zero);
        printf("static TLS: %ld of %d threads had their own copy, %ld had none; ours still %ld (want 7), %ld (want 0)\n",
               (long)g_tls_ok, NTHREADS, (long)g_tls_null, mine ? (long)*mine : -1L, zero ? (long)*zero : -1L);
    }

    /* the exit code the thread returned */
    DWORD ec = 0;
    GetExitCodeThread(th[0], &ec);
    printf("GetExitCodeThread: %#lx (want 0x1234)\n", (unsigned long)ec);

    /* a finished thread's handle stays signalled, so waiting again is instant */
    printf("waiting again on a finished thread: %s\n",
           WaitForSingleObject(th[0], 0) == WAIT_OBJECT_0 ? "returns at once" : "DID NOT");
    for (int i = 0; i < made; i++) CloseHandle(th[i]);
    DeleteCriticalSection(&g_cs);

    /* --- do waits really block --- */
    g_ping = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset */
    g_pong = CreateEventA(NULL, FALSE, FALSE, NULL);
    printf("\nCreateEvent x2: %s\n", g_ping && g_pong ? "ok" : "FAILED");
    printf("an unsignalled event times out: %s\n",
           WaitForSingleObject(g_ping, 20) == WAIT_TIMEOUT ? "yes" : "NO");

    HANDLE b = CreateThread(NULL, 0, bouncer, NULL, 0, NULL);
    int ok = b != NULL;
    for (int i = 0; ok && i < 50; i++) {
        SetEvent(g_ping);
        if (WaitForSingleObject(g_pong, 5000) != WAIT_OBJECT_0) { ok = 0; break; }
    }
    if (b) WaitForSingleObject(b, 5000);
    printf("ping-pong 50 times: %s, %ld bounces\n", ok ? "ok" : "FAILED", (long)g_bounces);
    if (b) CloseHandle(b);

    /* an auto-reset event does not stay signalled */
    SetEvent(g_ping);
    DWORD first = WaitForSingleObject(g_ping, 0);
    DWORD second = WaitForSingleObject(g_ping, 0);
    printf("auto-reset consumed by the first wait: %s\n",
           first == WAIT_OBJECT_0 && second == WAIT_TIMEOUT ? "yes" : "NO");

    /* a manual-reset one does */
    HANDLE man = CreateEventA(NULL, TRUE, TRUE, NULL);
    printf("manual-reset stays signalled: %s\n",
           WaitForSingleObject(man, 0) == WAIT_OBJECT_0 &&
           WaitForSingleObject(man, 0) == WAIT_OBJECT_0 ? "yes" : "NO");
    ResetEvent(man);
    printf("...until reset: %s\n", WaitForSingleObject(man, 0) == WAIT_TIMEOUT ? "yes" : "NO");
    CloseHandle(man);
    CloseHandle(g_ping);
    CloseHandle(g_pong);

    /* --- a mutex is recursive to its owner and excludes everyone else --- */
    HANDLE mx = CreateMutexA(NULL, FALSE, NULL);
    printf("\nmutex: acquire %s, again (recursive) %s\n",
           WaitForSingleObject(mx, 100) == WAIT_OBJECT_0 ? "ok" : "FAILED",
           WaitForSingleObject(mx, 100) == WAIT_OBJECT_0 ? "ok" : "FAILED");
    printf("release twice: %s\n",
           ReleaseMutex(mx) && ReleaseMutex(mx) ? "ok" : "FAILED");
    CloseHandle(mx);

    /* --- a critical section is recursive too --- */
    CRITICAL_SECTION cs2;
    InitializeCriticalSection(&cs2);
    EnterCriticalSection(&cs2);
    EnterCriticalSection(&cs2);
    LeaveCriticalSection(&cs2);
    LeaveCriticalSection(&cs2);
    printf("critical section entered twice and left twice: ok\n");
    printf("TryEnter on a free one: %s\n", TryEnterCriticalSection(&cs2) ? "ok" : "FAILED");
    LeaveCriticalSection(&cs2);
    DeleteCriticalSection(&cs2);

    /* --- TLS is per thread, which per-thread TEBs give for free --- */
    DWORD slot = TlsAlloc();
    printf("\nTlsAlloc: %s\n", slot != TLS_OUT_OF_INDEXES ? "ok" : "FAILED");
    TlsSetValue(slot, (LPVOID)(ULONG_PTR)0xABCD);
    printf("TlsGetValue here: %#lx (want 0xabcd)\n", (unsigned long)(ULONG_PTR)TlsGetValue(slot));
    TlsFree(slot);
    return 0;
}
