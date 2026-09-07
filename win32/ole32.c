/* ole32.dll -- the small part of COM that a non-COM program still needs.
 *
 * This is not a COM runtime. `win32/com.c` builds the vtables that d3d9 and
 * DirectSound hand out, and nothing here creates an object. What is here is
 * the handful of calls a program makes on the way past COM:
 *
 *  - CoTaskMemAlloc/Free, because that is who owns an item ID list once the
 *    shell has handed one over (see SHGetSpecialFolderLocation in shell32.c).
 *    Without it, an installer that correctly frees what it was given stops on
 *    the free rather than on anything it wanted to do.
 *  - CoInitialize/CoUninitialize, which almost everything calls and which
 *    have nothing to do here: there is one thread apartment and no marshalling.
 *  - CoCreateInstance, which fails, because a class we do not implement is a
 *    class we cannot return -- and REGDB_E_CLASSNOTREG is exactly what a
 *    program is prepared for when a component is not installed.
 */
#define _GNU_SOURCE
#include "w32.h"

#include <stdio.h>
#include <string.h>

enum {
    S_OK_          = 0,
    S_FALSE_       = 1,
    E_OUTOFMEMORY_ = (int)0x8007000E,
    E_INVALIDARG_  = (int)0x80070057,
    E_NOINTERFACE_ = (int)0x80004002,
    REGDB_E_CLASSNOTREG_ = (int)0x80040154,
};

static void o_CoTaskMemAlloc(w32 *w) {
    uint64_t n = ARG(0);
    if (!n) { RET(0); return; }            /* documented: may return NULL */
    RET(w32_heap_alloc(w, n));
}
static void o_CoTaskMemFree(w32 *w) {
    if (ARG(0)) w32_heap_free(w, ARG(0));
    RET(0);
}
static void o_CoTaskMemRealloc(w32 *w) {
    uint64_t p = ARG(0), n = ARG(1);
    if (!p) { RET(n ? w32_heap_alloc(w, n) : 0); return; }
    if (!n) { w32_heap_free(w, p); RET(0); return; }
    RET(w32_heap_realloc(w, p, n));
}

/* One apartment, no marshalling, nothing to set up. S_OK the first time and
 * S_FALSE afterwards is what the API promises, and a program that balances
 * its Uninitialize calls against the return value gets that right. */
static int g_co_depth;
static void o_CoInitialize(w32 *w)   { (void)w; RET(g_co_depth++ ? S_FALSE_ : S_OK_); }
static void o_CoInitializeEx(w32 *w) { (void)w; RET(g_co_depth++ ? S_FALSE_ : S_OK_); }
static void o_CoUninitialize(w32 *w) { (void)w; if (g_co_depth) g_co_depth--; RET(0); }

/* Nothing here can create a class object. Saying so is better than handing
 * back a null pointer with S_OK, which is how a program ends up faulting
 * somewhere unrelated. */
static void o_CoCreateInstance(w32 *w) {
    w32_note_refused(w, "ole32!CoCreateInstance (no COM class factory here)");
    if (ARG(4)) w32_write(w, ARG(4), w->is32 ? 4 : 8, 0);   /* *ppv = NULL */
    RET((uint64_t)(uint32_t)REGDB_E_CLASSNOTREG_);
}
static void o_CoGetClassObject(w32 *w) {
    w32_note_refused(w, "ole32!CoCreateInstance (no COM class factory here)");
    if (ARG(4)) w32_write(w, ARG(4), w->is32 ? 4 : 8, 0);
    RET((uint64_t)(uint32_t)REGDB_E_CLASSNOTREG_);
}

/* A GUID a caller can use as a unique tag. Not cryptographically anything;
 * unique within a run, which is what it is used for. */
static void o_CoCreateGuid(w32 *w) {
    static uint32_t seq;
    uint64_t g = ARG(0);
    if (!g) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    seq++;
    w32_write(w, g + 0,  4, 0x0f000000u | seq);
    w32_write(w, g + 4,  2, 0x1111);
    w32_write(w, g + 6,  2, 0x4222);            /* version 4, by the nibble */
    w32_write(w, g + 8,  4, 0x80333333u);
    w32_write(w, g + 12, 4, 0x44444444u ^ seq);
    RET(S_OK_);
}

/* OLE's own allocator, asked for by name. There is one allocator and it is
 * the guest heap, so there is no object to hand back; a program that wants
 * IMalloc rather than CoTaskMemAlloc is rare enough to be told no clearly. */
static void o_CoGetMalloc(w32 *w) {
    w32_note_refused(w, "ole32!CoGetMalloc (use CoTaskMemAlloc)");
    if (ARG(1)) w32_write(w, ARG(1), w->is32 ? 4 : 8, 0);
    RET((uint64_t)(uint32_t)E_NOINTERFACE_);
}

#define F(n, a) { #n, a, 0, o_##n, 0 }
const w32_api w32_ole32[] = {
    F(CoTaskMemAlloc, 1), F(CoTaskMemFree, 1), F(CoTaskMemRealloc, 2),
    F(CoInitialize, 1), F(CoInitializeEx, 2), F(CoUninitialize, 0),
    F(CoCreateInstance, 5), F(CoGetClassObject, 5),
    F(CoCreateGuid, 1), F(CoGetMalloc, 2),
    { 0, 0, 0, 0, 0 },
};
#undef F
