/* The DLLs a game links against and mostly does not use.
 *
 * A modern game engine links a long tail of Windows libraries because its
 * runtime supports features this particular game does not: networking it
 * never opens, a file dialog it never shows, a crash reporter that only runs
 * after a crash, an IME for languages the game does not ship. Every one of
 * those is in the import table whether or not it is ever called, and an
 * import that cannot be resolved is a program that does not start.
 *
 * So the point of this file is *starting*. Each function here is either a
 * real answer (there is no IME; there is no network adapter; the composition
 * timing is unavailable) or a refusal recorded in the run report. What none
 * of them do is claim to have done something. A game told its file dialog
 * was cancelled carries on correctly; a game told a download succeeded and
 * handed an empty buffer does not.
 *
 * Where a whole subsystem is missing rather than empty -- Direct3D 11 is the
 * one that matters -- it says so in its own file and in the report, because
 * "started and could not create a graphics device" is a useful outcome and
 * "started and drew nothing" is not.
 */
#define _GNU_SOURCE
#include "w32.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

enum { S_OK_ = 0, S_FALSE_ = 1, E_FAIL_ = (int)0x80004005, E_NOTIMPL_ = (int)0x80004001,
       E_INVALIDARG_ = (int)0x80070057 };

/* ------------------------------------------------------------------ imm32
 *
 * The input method editor: how you type Japanese. There is none, and a
 * program is entitled to be told that clearly -- a null context is the
 * documented answer for a window with no IME, and every caller handles it
 * because plenty of real Windows installs have no IME either. */
static void i_ImmGetContext(w32 *w)          { (void)w; RET(0); }
static void i_ImmReleaseContext(w32 *w)      { (void)w; RET(1); }
static void i_ImmAssociateContext(w32 *w)    { (void)w; RET(0); }
static void i_ImmAssociateContextEx(w32 *w)  { (void)w; RET(1); }
static void i_ImmSetCompositionWindow(w32 *w) { (void)w; RET(0); }
static void i_ImmSetCandidateWindow(w32 *w)  { (void)w; RET(0); }
static void i_ImmSetCompositionFontW(w32 *w) { (void)w; RET(0); }
static void i_ImmGetCompositionStringW(w32 *w) { (void)w; RET(0); }
static void i_ImmNotifyIME(w32 *w)           { (void)w; RET(0); }
static void i_ImmDisableIME(w32 *w)          { (void)w; RET(1); }
static void i_ImmIsIME(w32 *w)               { (void)w; RET(0); }
static void i_ImmGetDefaultIMEWnd(w32 *w)    { (void)w; RET(0); }

#define I(n, a) { #n, a, 0, i_##n, 0 }
const w32_api w32_imm32[] = {
    I(ImmGetContext, 1), I(ImmReleaseContext, 2), I(ImmAssociateContext, 2),
    I(ImmAssociateContextEx, 3), I(ImmSetCompositionWindow, 2),
    I(ImmSetCandidateWindow, 2), I(ImmSetCompositionFontW, 2),
    I(ImmGetCompositionStringW, 4), I(ImmNotifyIME, 4), I(ImmDisableIME, 1),
    I(ImmIsIME, 1), I(ImmGetDefaultIMEWnd, 1),
    { 0, 0, 0, 0, 0 },
};
#undef I

/* ----------------------------------------------------------------- dwmapi
 *
 * Desktop composition: what a game asks so it can line its frames up with
 * the compositor's refresh. There is no compositor here -- frames go to a
 * Metal layer -- so the honest answer is the documented "composition is not
 * available", which sends the game down its own fallback path. Answering
 * with invented timing would make it pace itself against a clock that does
 * not exist. */
static void d_DwmIsCompositionEnabled(w32 *w) {
    if (ARG(0)) w32_write(w, ARG(0), 4, 0);
    RET(S_OK_);
}
static void d_DwmGetCompositionTimingInfo(w32 *w) {
    w32_note_refused(w, "dwmapi!DwmGetCompositionTimingInfo (no compositor to sync to)");
    RET((uint64_t)(uint32_t)0x800706F4u);          /* DWM_E_COMPOSITIONDISABLED */
}
static void d_DwmGetWindowAttribute(w32 *w) {
    /* The one attribute worth answering is the extended frame bounds, and
     * with no decorations that is the window rectangle. */
    uint64_t out = ARG(2);
    uint32_t cb = (uint32_t)ARG(3);
    if ((uint32_t)ARG(1) == 9 && out && cb >= 16) {   /* DWMWA_EXTENDED_FRAME_BOUNDS */
        int x = 0, y = 0, cx = 0, cy = 0;
        if (w32_window_area(ARG(0), 1, &x, &y, &cx, &cy)) {
            w32_write(w, out, 4, (uint64_t)(uint32_t)x);
            w32_write(w, out + 4, 4, (uint64_t)(uint32_t)y);
            w32_write(w, out + 8, 4, (uint64_t)(uint32_t)(x + cx));
            w32_write(w, out + 12, 4, (uint64_t)(uint32_t)(y + cy));
            RET(S_OK_);
            return;
        }
    }
    RET((uint64_t)(uint32_t)E_INVALIDARG_);
}
static void d_DwmSetWindowAttribute(w32 *w) { (void)w; RET(S_OK_); }
static void d_DwmFlush(w32 *w) { (void)w; RET(S_OK_); }
static void d_DwmEnableMMCSS(w32 *w) { (void)w; RET(S_OK_); }
static void d_DwmExtendFrameIntoClientArea(w32 *w) { (void)w; RET(S_OK_); }

#define D(n, a) { #n, a, 0, d_##n, 0 }
const w32_api w32_dwmapi[] = {
    D(DwmIsCompositionEnabled, 1), D(DwmGetCompositionTimingInfo, 2),
    D(DwmGetWindowAttribute, 4), D(DwmSetWindowAttribute, 4),
    D(DwmFlush, 0), D(DwmEnableMMCSS, 1), D(DwmExtendFrameIntoClientArea, 2),
    { 0, 0, 0, 0, 0 },
};
#undef D

/* ------------------------------------------------------------------- avrt
 *
 * Multimedia Class Scheduler: an audio thread asking the scheduler to treat
 * it as latency-critical. There is no such scheduler here, and the thread
 * runs at whatever priority it already had -- so the handle is real enough
 * to be released and the priority call succeeds having changed nothing.
 * That is the same outcome as running on a Windows install with the MMCSS
 * service disabled, which is a supported configuration. */
static void a_AvSetMmThreadCharacteristicsA(w32 *w) {
    if (ARG(1)) w32_write(w, ARG(1), 4, 1);        /* a task index */
    RET(0x0AF70001u);
}
static void a_AvSetMmThreadCharacteristicsW(w32 *w) { a_AvSetMmThreadCharacteristicsA(w); }
static void a_AvRevertMmThreadCharacteristics(w32 *w) { (void)w; RET(1); }
static void a_AvSetMmThreadPriority(w32 *w) { (void)w; RET(1); }

#define A(n, a) { #n, a, 0, a_##n, 0 }
const w32_api w32_avrt[] = {
    A(AvSetMmThreadCharacteristicsA, 2), A(AvSetMmThreadCharacteristicsW, 2),
    A(AvRevertMmThreadCharacteristics, 1), A(AvSetMmThreadPriority, 2),
    { 0, 0, 0, 0, 0 },
};
#undef A

/* ---------------------------------------------------------------- version
 *
 * A program reading its own version resource, usually to put a number in a
 * title bar or a crash report. The resource is in its own image, so this is
 * a real lookup rather than a stub -- it walks RT_VERSION the same way the
 * dialog loader walks RT_DIALOG. */
enum { RT_VERSION_ = 16 };

static uint64_t version_resource(w32 *w, uint64_t module, uint32_t *size) {
    uint64_t hr = w32_find_resource(w, module, RT_VERSION_, 1, 0);
    if (!hr) return 0;
    return w32_resource_data(w, hr, size);
}
static void v_GetFileVersionInfoSizeW(w32 *w) {
    uint32_t size = 0;
    if (ARG(1)) w32_write(w, ARG(1), 4, 0);
    /* The path argument names a file; the only one we can read a resource
     * out of is the running image, which is what a program asking about its
     * own version means. */
    if (!version_resource(w, 0, &size)) { w32_set_last_error(w, 1813); RET(0); return; }
    RET((uint64_t)size);
}
static void v_GetFileVersionInfoSizeA(w32 *w) { v_GetFileVersionInfoSizeW(w); }
static void v_GetFileVersionInfoW(w32 *w) {
    uint32_t size = 0;
    uint64_t src = version_resource(w, 0, &size);
    uint64_t dst = ARG(3);
    uint32_t cap = (uint32_t)ARG(2);
    if (!src || !dst || cap < size) { w32_set_last_error(w, 1813); RET(0); return; }
    for (uint32_t i = 0; i < size; i++) w32_write(w, dst + i, 1, w32_read(w, src + i, 1));
    RET(1);
}
static void v_GetFileVersionInfoA(w32 *w) { v_GetFileVersionInfoW(w); }
/* VerQueryValue picks a field out of that block. "\\" asks for the fixed
 * VS_FIXEDFILEINFO, which is the one a program actually reads -- it is at a
 * known offset inside the block, found by its signature rather than by
 * walking the whole variable-length structure. */
static void v_VerQueryValueW(w32 *w) {
    uint64_t block = ARG(0), out = ARG(2), len = ARG(3);
    if (!block || !out) { RET(0); return; }
    for (uint32_t off = 0; off < 512; off += 4) {
        if ((uint32_t)w32_read(w, block + off, 4) == 0xFEEF04BDu) {
            w32_write(w, out, (int)w32_ptrsize(w), block + off);
            if (len) w32_write(w, len, 4, 52);      /* sizeof VS_FIXEDFILEINFO */
            RET(1);
            return;
        }
    }
    RET(0);
}
static void v_VerQueryValueA(w32 *w) { v_VerQueryValueW(w); }

#define V(n, a) { #n, a, 0, v_##n, 0 }
const w32_api w32_version[] = {
    V(GetFileVersionInfoSizeW, 2), V(GetFileVersionInfoSizeA, 2),
    V(GetFileVersionInfoW, 4), V(GetFileVersionInfoA, 4),
    V(VerQueryValueW, 4), V(VerQueryValueA, 4),
    { 0, 0, 0, 0, 0 },
};
#undef V

/* ----------------------------------------------------------------- rpcrt4
 *
 * UUIDs. A game makes one to identify an install or a session. Version 4,
 * from the same counter-and-clock source CoCreateGuid uses, so two calls
 * never collide and the value looks like what it claims to be. */
static void r_UuidCreate(w32 *w) {
    static uint32_t seq;
    uint64_t p = ARG(0);
    if (!p) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    seq++;
    uint32_t t = (uint32_t)time(0);
    w32_write(w, p + 0,  4, t ^ (seq * 2654435761u));
    w32_write(w, p + 4,  2, (uint64_t)(seq & 0xFFFF));
    w32_write(w, p + 6,  2, 0x4000u | (seq & 0x0FFF));    /* version 4 */
    w32_write(w, p + 8,  4, 0x80000000u | (t ^ seq));     /* variant 1 */
    w32_write(w, p + 12, 4, 0x5758494Eu ^ seq);
    RET(0);                                               /* RPC_S_OK */
}
static void r_UuidCreateSequential(w32 *w) { r_UuidCreate(w); }
static void r_UuidToStringW(w32 *w) {
    uint64_t g = ARG(0), out = ARG(1);
    if (!g || !out) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    uint8_t b[16];
    for (int i = 0; i < 16; i++) b[i] = (uint8_t)w32_read(w, g + (unsigned)i, 1);
    char buf[40];
    snprintf(buf, sizeof buf,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    /* The caller gets a pointer to a string rpcrt4 owns and frees with
     * RpcStringFree, so it has to live on the guest heap. */
    size_t n = strlen(buf);
    uint64_t str = w32_heap_alloc(w, (n + 1) * 2);
    for (size_t i = 0; i <= n; i++) w32_write(w, str + i * 2, 2, (uint8_t)buf[i]);
    w32_write(w, out, (int)w32_ptrsize(w), str);
    RET(0);
}
static void r_UuidToStringA(w32 *w) {
    uint64_t g = ARG(0), out = ARG(1);
    if (!g || !out) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    uint8_t b[16];
    for (int i = 0; i < 16; i++) b[i] = (uint8_t)w32_read(w, g + (unsigned)i, 1);
    char buf[40];
    snprintf(buf, sizeof buf,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    uint64_t str = w32_strdup(w, buf);
    w32_write(w, out, (int)w32_ptrsize(w), str);
    RET(0);
}
static void r_RpcStringFreeW(w32 *w) {
    if (ARG(0)) {
        uint64_t p = w32_read(w, ARG(0), (int)w32_ptrsize(w));
        if (p) w32_heap_free(w, p);
        w32_write(w, ARG(0), (int)w32_ptrsize(w), 0);
    }
    RET(0);
}
static void r_RpcStringFreeA(w32 *w) { r_RpcStringFreeW(w); }

#define R(n, a) { #n, a, 0, r_##n, 0 }
const w32_api w32_rpcrt4[] = {
    R(UuidCreate, 1), R(UuidCreateSequential, 1),
    R(UuidToStringW, 2), R(UuidToStringA, 2),
    R(RpcStringFreeW, 1), R(RpcStringFreeA, 1),
    { 0, 0, 0, 0, 0 },
};
#undef R

/* ---------------------------------------------------------------- gdiplus
 *
 * A game uses GDI+ to load a PNG for a splash screen or an icon. Startup
 * succeeds -- refusing it makes a game abort before it draws anything -- and
 * the calls that would decode an image report failure, so the game falls
 * back or skips the splash rather than drawing from an empty bitmap. */
static void gp_GdiplusStartup(w32 *w) {
    if (ARG(0)) w32_write(w, ARG(0), (int)w32_ptrsize(w), 0x6470u);   /* a token */
    RET(0);                                                            /* Ok */
}
static void gp_GdiplusShutdown(w32 *w) { (void)w; RET(0); }
static void gp_GdipCreateBitmapFromFile(w32 *w) {
    w32_note_refused(w, "gdiplus (no image decoder here)");
    if (ARG(1)) w32_write(w, ARG(1), (int)w32_ptrsize(w), 0);
    RET(3);                                                            /* InvalidParameter */
}
static void gp_GdipDisposeImage(w32 *w) { (void)w; RET(0); }

#define G(n, a) { #n, a, 0, gp_##n, 0 }
const w32_api w32_gdiplus[] = {
    G(GdiplusStartup, 3), G(GdiplusShutdown, 1),
    G(GdipCreateBitmapFromFile, 2), G(GdipDisposeImage, 1),
    { 0, 0, 0, 0, 0 },
};
#undef G

/* --------------------------------------------------------------- comdlg32
 *
 * The Open and Save dialogs. There is no file picker to show and no person
 * to answer it, so both are cancelled -- which is what a person pressing
 * Escape produces, and every caller handles that because it is the common
 * case. Returning success with an empty path would make a game try to open
 * "". */
static void c_GetOpenFileNameW(w32 *w) {
    w32_note_refused(w, "comdlg32!GetOpenFileName (no file picker; treated as cancelled)");
    RET(0);
}
static void c_GetSaveFileNameW(w32 *w) {
    w32_note_refused(w, "comdlg32!GetSaveFileName (no file picker; treated as cancelled)");
    RET(0);
}
static void c_CommDlgExtendedError(w32 *w) { (void)w; RET(0); }   /* cancelled, not failed */

#define C(n, a) { #n, a, 0, c_##n, 0 }
const w32_api w32_comdlg32[] = {
    C(GetOpenFileNameW, 1), C(GetSaveFileNameW, 1),
    { "GetOpenFileNameA", 1, 0, c_GetOpenFileNameW, 0 },
    { "GetSaveFileNameA", 1, 0, c_GetSaveFileNameW, 0 },
    C(CommDlgExtendedError, 0),
    { 0, 0, 0, 0, 0 },
};
#undef C

/* ---------------------------------------------------------------- dbghelp
 *
 * MiniDumpWriteDump runs from inside a crash handler. Writing a real minidump
 * would mean writing the Windows crash-dump format from a process that is
 * already broken; refusing is safe, and the run report this layer produces
 * has more in it than a minidump nobody here can open anyway. */
static void db_MiniDumpWriteDump(w32 *w) {
    w32_note_refused(w, "dbghelp!MiniDumpWriteDump (the run report above is the crash dump)");
    RET(0);
}
static void db_SymInitialize(w32 *w) { (void)w; RET(0); }
static void db_SymCleanup(w32 *w) { (void)w; RET(1); }

#define B(n, a) { #n, a, 0, db_##n, 0 }
const w32_api w32_dbghelp[] = {
    B(MiniDumpWriteDump, 7), B(SymInitialize, 3), B(SymCleanup, 1),
    { 0, 0, 0, 0, 0 },
};
#undef B

/* --------------------------------------------------------------- iphlpapi
 *
 * Enumerating network adapters. There are none reachable from here, and
 * ERROR_NO_DATA is the documented way to say so -- a game asking for a MAC
 * address to key an install by gets nothing and falls back to something
 * else, rather than getting an adapter that does not exist. */
static void ip_GetAdaptersAddresses(w32 *w) {
    if (ARG(4)) w32_write(w, ARG(4), 4, 0);
    RET(232);                                       /* ERROR_NO_DATA */
}
static void ip_GetAdaptersInfo(w32 *w) {
    if (ARG(1)) w32_write(w, ARG(1), 4, 0);
    RET(232);
}
static void ip_NotifyIpInterfaceChange(w32 *w) {
    /* Nothing will ever change, so a registration that never fires is
     * accurate. The handle has to be non-null or the caller may not
     * unregister it. */
    if (ARG(4)) w32_write(w, ARG(4), (int)w32_ptrsize(w), 0x0E700001u);
    RET(0);                                         /* NO_ERROR */
}
static void ip_CancelMibChangeNotify2(w32 *w) { (void)w; RET(0); }

#define P(n, a) { #n, a, 0, ip_##n, 0 }
const w32_api w32_iphlpapi[] = {
    P(GetAdaptersAddresses, 5), P(GetAdaptersInfo, 2),
    P(NotifyIpInterfaceChange, 5), P(CancelMibChangeNotify2, 1),
    { 0, 0, 0, 0, 0 },
};
#undef P

/* ---------------------------------------------------------------- propsys
 *
 * PROPVARIANT accessors, which arrive with WASAPI device enumeration: a
 * device's friendly name and format come back as one. Nothing here
 * enumerates a device, so the variants a program holds are the empty ones it
 * was given, and reading a value out of an empty variant fails -- which is
 * what it should do. */
static void ps_PropVariantToInt64(w32 *w) {
    if (ARG(1)) { w32_write(w, ARG(1), 4, 0); w32_write(w, ARG(1) + 4, 4, 0); }
    RET((uint64_t)(uint32_t)E_FAIL_);
}
static void ps_PropVariantToUInt32(w32 *w) {
    if (ARG(1)) w32_write(w, ARG(1), 4, 0);
    RET((uint64_t)(uint32_t)E_FAIL_);
}
static void ps_PropVariantToStringAlloc(w32 *w) {
    if (ARG(1)) w32_write(w, ARG(1), (int)w32_ptrsize(w), 0);
    RET((uint64_t)(uint32_t)E_FAIL_);
}

#define S(n, a) { #n, a, 0, ps_##n, 0 }
const w32_api w32_propsys[] = {
    S(PropVariantToInt64, 2), S(PropVariantToUInt32, 2), S(PropVariantToStringAlloc, 2),
    { 0, 0, 0, 0, 0 },
};
#undef S
