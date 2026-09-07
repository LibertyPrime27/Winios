/* Not a test -- a hunt. Deliberately not in the suite.
 *
 * threadtest checks the semantics with the race *forced*, which is what makes
 * it a recording anyone can check. This is the other half: the same work with
 * nothing forced, so the only handovers are the run loop's own slice
 * boundaries, run long enough and often enough that a rare failure shows up.
 *
 * It is how the first real threading bug here was found. The guarded counter
 * came out three short of a hundred thousand while the interlocked one was
 * exact -- and the reason the interlocked one was exact is that the compiler
 * turned it into a single `lock add`, which cannot be split, where the
 * guarded increment is three instructions and needs the critical section to
 * actually exclude. It did not: every thread took the "one at a time" guest
 * lock except the thread the process started on.
 *
 * Run it in a loop when touching anything about threads:
 *
 *     for i in $(seq 20); do build/winrun -t 180 tests/win32/threadstress32.exe; done
 *
 * `lost` must be 0 every time. It is slow on the interpreter by design --
 * the point is volume, not speed.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#define NT 4
#define PER 25000
static CRITICAL_SECTION cs;
static volatile LONG guarded, inter;
static DWORD WINAPI worker(LPVOID p) {
    (void)p;
    for (int i = 0; i < PER; i++) {
        EnterCriticalSection(&cs);
        guarded = guarded + 1;
        LeaveCriticalSection(&cs);
        InterlockedIncrement(&inter);
    }
    return 0;
}
int main(void) {
    InitializeCriticalSection(&cs);
    HANDLE th[NT];
    for (int i = 0; i < NT; i++) th[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    DWORD r = WaitForMultipleObjects(NT, th, TRUE, 120000);
    printf("wait %s  guarded %ld  interlocked %ld  want %d  lost %ld\n",
           r == WAIT_OBJECT_0 ? "ok" : "TIMEOUT", (long)guarded, (long)inter,
           NT * PER, (long)(inter - guarded));
    return 0;
}
