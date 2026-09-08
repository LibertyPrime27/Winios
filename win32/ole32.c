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
/* The CLSID spelled the way the registry spells it, so a report names the
 * class rather than "a class" -- which is the difference between knowing
 * what to implement next and guessing. A small pool, because the report
 * counts calls by the pointer it was given. */
static const char *class_note(const uint8_t *g, const char *what) {
    static char pool[16][128]; static int n;
    char s[128];
    if (!g) snprintf(s, sizeof s, "ole32!%s (no such COM class here)", what);
    else snprintf(s, sizeof s, "ole32!%s {%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x} (no such COM class here)",
                  what, g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    for (int i = 0; i < n; i++) if (!strcmp(pool[i], s)) return pool[i];
    if (n < 16) { snprintf(pool[n], sizeof pool[n], "%s", s); return pool[n++]; }
    return "ole32!CoCreateInstance (no such COM class here; more distinct classes than this report lists)";
}
static void o_CoCreateInstance(w32 *w) {
    const uint8_t *clsid = W32PN(w, ARG(0), 16), *iid = W32PN(w, ARG(3), 16);
    uint64_t out = ARG(4);
    if (!out) { RET((uint64_t)(uint32_t)0x80004003u); return; }               /* E_POINTER */
    w32_write(w, out, w->is32 ? 4 : 8, 0);                                      /* *ppv = NULL */
    if (clsid && w32_com_create(w, clsid, iid, out)) { RET(0); return; }
    w32_note_refused(w, class_note(clsid, "CoCreateInstance"));
    RET((uint64_t)(uint32_t)REGDB_E_CLASSNOTREG_);
}
static void o_CoGetClassObject(w32 *w) {
    const uint8_t *clsid = W32PN(w, ARG(0), 16);
    if (ARG(4)) w32_write(w, ARG(4), w->is32 ? 4 : 8, 0);
    w32_note_refused(w, class_note(clsid, "CoGetClassObject"));
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

/* OleInitialize is CoInitialize plus the OLE libraries, and there are no OLE
 * libraries here, so it is CoInitialize. A program that calls it is usually
 * on its way to a drag-and-drop it will never do inside an installer. */
static void o_OleInitialize(w32 *w)   { (void)w; RET(g_co_depth++ ? S_FALSE_ : S_OK_); }
static void o_OleUninitialize(w32 *w) { (void)w; if (g_co_depth) g_co_depth--; RET(0); }

/* "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" back into a GUID. The layout is
 * the awkward part and the reason a program cannot just memcpy it: the first
 * three fields are little-endian numbers and the last eight are bytes in
 * written order, so half the string reverses and half does not. */
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int guid_from(const char *s, uint8_t out[16]) {
    if (*s == '{') s++;
    static const int RUN[5] = { 8, 4, 4, 4, 12 };
    char hex[33];
    int n = 0;
    for (int f = 0; f < 5; f++) {
        for (int i = 0; i < RUN[f]; i++) {
            if (hexval((unsigned char)*s) < 0) return 0;
            hex[n++] = *s++;
        }
        if (f < 4) { if (*s != '-') return 0; s++; }
    }
    hex[n] = 0;
    uint8_t b[16];
    for (int i = 0; i < 16; i++) b[i] = (uint8_t)((hexval(hex[i * 2]) << 4) | hexval(hex[i * 2 + 1]));
    /* Data1 (4 bytes) and Data2, Data3 (2 each) are stored least significant
     * byte first; Data4's eight bytes are in the order they are written. */
    out[0] = b[3]; out[1] = b[2]; out[2] = b[1]; out[3] = b[0];
    out[4] = b[5]; out[5] = b[4];
    out[6] = b[7]; out[7] = b[6];
    for (int i = 8; i < 16; i++) out[i] = b[i];
    return 1;
}
static void guid_parse(w32 *w) {
    char buf[64];
    if (!ARG(0) || !ARG(1)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    w32_wtoa(w, ARG(0), buf, sizeof buf);
    uint8_t g[16];
    if (!guid_from(buf, g)) { RET((uint64_t)(uint32_t)0x800401F3u); return; }   /* CO_E_CLASSSTRING */
    for (int i = 0; i < 16; i++) w32_write(w, ARG(1) + (unsigned)i, 1, g[i]);
    RET(S_OK_);
}
static void o_IIDFromString(w32 *w)   { guid_parse(w); }
static void o_CLSIDFromString(w32 *w) { guid_parse(w); }
/* And back the other way, which is how a program writes a CLSID into the
 * registry. Returns the character count including the terminator. */
static void o_StringFromGUID2(w32 *w) {
    uint64_t g = ARG(0), out = ARG(1);
    int cap = (int)(int32_t)(uint32_t)ARG(2);
    if (!g || !out || cap < 39) { RET(0); return; }
    uint8_t b[16];
    for (int i = 0; i < 16; i++) b[i] = (uint8_t)w32_read(w, g + (unsigned)i, 1);
    char buf[40];
    snprintf(buf, sizeof buf,
             "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    for (int i = 0; i <= 38; i++) w32_write(w, out + (unsigned)i * 2, 2, (uint8_t)buf[i]);
    RET(39);
}

/* A PROPVARIANT is a tagged union; clearing one is releasing whatever the
 * tag says it holds and setting the tag to VT_EMPTY. Nothing here ever puts
 * anything in one, so clearing is setting the tag -- and a caller that
 * clears a variant it was never given a value for is doing the right thing
 * either way. */
static void o_PropVariantClear(w32 *w) {
    if (ARG(0)) { w32_write(w, ARG(0), 2, 0); w32_write(w, ARG(0) + 2, 2, 0); }
    RET(S_OK_);
}
static void o_PropVariantInit(w32 *w) {
    void *p = W32PN(w, ARG(0), 24);
    if (p) memset(p, 0, 24);
    RET(0);
}
static void o_VariantClear(w32 *w) { o_PropVariantClear(w); }
/* A free-threaded marshaller lets an object be used from any apartment.
 * There is one apartment and no marshalling, so there is nothing to make --
 * and a caller told so uses the object directly, which is correct here. */
static void o_CoCreateFreeThreadedMarshaler(w32 *w) {
    if (ARG(1)) w32_write(w, ARG(1), (int)w32_ptrsize(w), 0);
    RET((uint64_t)(uint32_t)E_NOINTERFACE_);
}
static void o_CoInitializeSecurity(w32 *w) { (void)w; RET(S_OK_); }
static void o_CoTaskMemSize(w32 *w) { (void)w; RET(0); }

#define F(n, a) { #n, a, 0, o_##n, 0 }
const w32_api w32_ole32[] = {
    F(CoTaskMemAlloc, 1), F(CoTaskMemFree, 1), F(CoTaskMemRealloc, 2),
    F(CoInitialize, 1), F(CoInitializeEx, 2), F(CoUninitialize, 0),
    F(CoCreateInstance, 5), F(CoGetClassObject, 5),
    F(CoCreateGuid, 1), F(CoGetMalloc, 2),
    F(OleInitialize, 1), F(OleUninitialize, 0),
    F(IIDFromString, 2), F(CLSIDFromString, 2), F(StringFromGUID2, 3),
    F(PropVariantClear, 1), F(PropVariantInit, 1), F(VariantClear, 1),
    F(CoCreateFreeThreadedMarshaler, 2), F(CoInitializeSecurity, 9), F(CoTaskMemSize, 1),
    { 0, 0, 0, 0, 0 },
};
#undef F
