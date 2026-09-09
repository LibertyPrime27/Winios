/* C++ exceptions, which on x86-64 Windows are structured exceptions.
 * (sehtest.c next door is the C-level test of RaiseException and the 32-bit
 * frame list; this is the 64-bit table-driven path, through a real personality.)
 *
 * A throw is RaiseException with a code of its own; catching it is the
 * dispatcher finding the frame's language handler in the unwind tables,
 * that handler (libgcc's, linked in statically) deciding it has a catch,
 * and RtlUnwindEx running every destructor between here and there before
 * landing on it. So this exercises RtlLookupFunctionEntry, RtlVirtualUnwind,
 * RtlUnwindEx and RtlCaptureContext with a real personality routine, not a
 * test double, and the destructor lines are the proof that the unwind ran
 * the frames in between rather than jumping over them.
 *
 * On 32-bit mingw the same source uses DWARF unwinding, which needs nothing
 * from the OS; it is here so the two recordings can be compared. */
#include <cstdio>
#include <stdexcept>

struct Guard { const char *n; ~Guard() { std::printf("destroyed %s\n", n); } };

static int depth3(int x) { Guard g{"depth3"}; if (x > 2) throw std::runtime_error("boom"); return x; }
static int depth2(int x) { Guard g{"depth2"}; return depth3(x) + 1; }

int main() {
    try {
        std::printf("no throw: %d\n", depth2(1));
        depth2(5);
        std::printf("NOT REACHED\n");
    } catch (const std::exception &e) {
        std::printf("caught: %s\n", e.what());
    }
    try {
        try { throw 42; }
        catch (int v) { std::printf("inner caught %d\n", v); throw; }
    } catch (int v) {
        std::printf("outer caught %d again\n", v);
    }
    try { throw 1.5; } catch (...) { std::printf("caught something\n"); }
    for (int i = 0; i < 3; i++) {
        try { throw i; } catch (int v) { if (v != i) std::printf("WRONG value %d\n", v); }
    }
    std::printf("three more, still running\n");
    return 0;
}
