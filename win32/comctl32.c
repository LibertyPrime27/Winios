/* comctl32.dll -- the common controls, and the reason an installer stops.
 *
 * Every Windows installer built in the last thirty years calls
 * InitCommonControls before it draws anything, and almost all of them import
 * it *by ordinal*: comctl32 has exported it as ordinal 17 since Windows 95,
 * and the import table in an NSIS or Inno setup names the number, not the
 * name. An import with no name is not a mystery -- it is a documented,
 * stable contract -- so `w32_ordinal_name` turns the number back into the
 * name before the loader looks anything up. That also fixes the report:
 * "comctl32.dll!#17" tells you nothing, "comctl32.dll!InitCommonControls"
 * tells you what to write.
 *
 * Only ordinals that are genuinely fixed are listed. Guessing at one would
 * be worse than leaving it a number, because a wrong name resolves to the
 * wrong implementation and the failure moves somewhere unrelated.
 *
 * What InitCommonControls actually *does* is register window classes, so the
 * CreateWindowEx that follows finds "msctls_progress32" and gets a progress
 * bar rather than nothing. Those classes are host-implemented -- their window
 * procedures are ours, in user32.c -- which is what lets a control paint
 * itself without any guest code behind it.
 */
#define _GNU_SOURCE
#include "w32.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------- ordinal exports */

/* Ordinals that Microsoft documents as stable. Nothing speculative: an
 * ordinal we are not sure of stays a number, and the report says so. */
static const struct { int ord; const char *name; } COMCTL_ORD[] = {
    {  17, "InitCommonControls"   },
    { 410, "SetWindowSubclass"    },
    { 411, "GetWindowSubclass"    },
    { 412, "RemoveWindowSubclass" },
    { 413, "DefSubclassProc"      },
};

const char *w32_ordinal_name(const char *dll, int ordinal) {
    if (!strcmp(dll, "comctl32.dll"))
        for (size_t i = 0; i < sizeof COMCTL_ORD / sizeof COMCTL_ORD[0]; i++)
            if (COMCTL_ORD[i].ord == ordinal) return COMCTL_ORD[i].name;
    return 0;
}

/* ------------------------------------------------------------ init */

/* The classes InitCommonControls registers. A program may create any of
 * them; the ones with a window procedure behind them draw, and the rest
 * exist so that CreateWindowEx succeeds and the layout is right even where
 * the control itself is a rectangle. */
static const char *const CLASSES[] = {
    "msctls_progress32", "msctls_statusbar32", "msctls_trackbar32",
    "msctls_updown32",   "msctls_hotkey32",
    "SysListView32",     "SysTreeView32",   "SysTabControl32",
    "SysHeader32",       "SysAnimate32",    "SysLink",
    "ToolbarWindow32",   "ReBarWindow32",   "ComboBoxEx32",
    "SysDateTimePick32", "SysMonthCal32",   "SysIPAddress32",
};

static int g_inited;

static void register_all(void) {
    if (g_inited) return;
    g_inited = 1;
    for (size_t i = 0; i < sizeof CLASSES / sizeof CLASSES[0]; i++)
        w32_register_control_class(CLASSES[i]);
}

static void c_InitCommonControls(w32 *w) { (void)w; register_all(); RET(0); }

/* A new run starts with no common controls registered and no image lists,
 * because the process is reused inside the app and the previous program's
 * handles must not resolve in this one. */

/* InitCommonControlsEx names a subset in its ICC_* mask. We register the lot
 * regardless -- there is no cost to a class nobody creates -- and return TRUE,
 * which is what a caller checks before it goes on to create the controls. */
static void c_InitCommonControlsEx(w32 *w) {
    uint64_t p = ARG(0);
    if (p) {
        uint32_t size = (uint32_t)w32_read(w, p, 4);
        if (size != 8) { RET(0); return; }      /* the documented sanity check */
    }
    register_all();
    RET(1);
}

/* ------------------------------------------------------------ image lists */

/* An image list is a strip of same-sized images with a handle. Nothing here
 * draws icons yet, so what a list holds is its geometry and a count: enough
 * that Create/Add/Destroy balance, an index comes back from Add in the order
 * the caller expects, and GetImageCount agrees with how many went in. A
 * control that asks to draw image 3 gets nothing drawn rather than a wrong
 * image or a fault. */
enum { MAX_ILS = 16 };
typedef struct { int used, cx, cy, count; } imagelist;
static imagelist g_il[MAX_ILS];
enum { IL_BASE = 0x000A0000u, IL_STEP = 8 };

static imagelist *il_of(uint64_t h) {
    if (h < IL_BASE) return 0;
    uint64_t i = (h - IL_BASE) / IL_STEP;
    if (i >= MAX_ILS || !g_il[i].used) return 0;
    return &g_il[i];
}

static void c_ImageList_Create(w32 *w) {
    int cx = (int)(int32_t)(uint32_t)ARG(0), cy = (int)(int32_t)(uint32_t)ARG(1);
    if (cx <= 0 || cy <= 0 || cx > 512 || cy > 512) { RET(0); return; }
    for (int i = 0; i < MAX_ILS; i++) if (!g_il[i].used) {
        g_il[i].used = 1; g_il[i].cx = cx; g_il[i].cy = cy; g_il[i].count = 0;
        RET(IL_BASE + (uint64_t)i * IL_STEP);
        return;
    }
    RET(0);
}
static void c_ImageList_Destroy(w32 *w) {
    imagelist *l = il_of(ARG(0));
    if (!l) { RET(0); return; }
    l->used = 0;
    RET(1);
}
/* Add returns the index of the first image added, or -1. */
static void c_ImageList_Add(w32 *w) {
    imagelist *l = il_of(ARG(0));
    if (!l) { RET((uint64_t)(uint32_t)-1); return; }
    RET((uint64_t)(uint32_t)l->count++);
}
static void c_ImageList_AddMasked(w32 *w) { c_ImageList_Add(w); }
static void c_ImageList_ReplaceIcon(w32 *w) { c_ImageList_Add(w); }
static void c_ImageList_GetImageCount(w32 *w) {
    imagelist *l = il_of(ARG(0));
    RET(l ? (uint64_t)l->count : 0);
}
static void c_ImageList_SetBkColor(w32 *w) { (void)w; RET(0xFFFFFFFFu); }   /* CLR_NONE */
static void c_ImageList_GetIconSize(w32 *w) {
    imagelist *l = il_of(ARG(0));
    if (!l) { RET(0); return; }
    if (ARG(1)) w32_write(w, ARG(1), 4, (uint64_t)(uint32_t)l->cx);
    if (ARG(2)) w32_write(w, ARG(2), 4, (uint64_t)(uint32_t)l->cy);
    RET(1);
}
static void c_ImageList_Draw(w32 *w)   { (void)w; RET(1); }
static void c_ImageList_DrawEx(w32 *w) { (void)w; RET(1); }
static void c_ImageList_Remove(w32 *w) {
    imagelist *l = il_of(ARG(0));
    if (!l) { RET(0); return; }
    int32_t idx = (int32_t)(uint32_t)ARG(1);
    if (idx < 0) l->count = 0;                  /* -1 removes them all */
    else if (l->count) l->count--;
    RET(1);
}

/* ------------------------------------------------------------ subclassing */

/* SetWindowSubclass is how a modern program hooks a control without the
 * SetWindowLong dance, and Inno's wizard uses it on every page. Each entry
 * is (window, procedure, id) and the chain is walked newest first, which is
 * the order DefSubclassProc has to go back through. */
enum { MAX_SUBCLASS = 64 };
static struct { int used; uint64_t hwnd, proc, id, ref; } g_sub[MAX_SUBCLASS];

static void c_SetWindowSubclass(w32 *w) {
    uint64_t hwnd = ARG(0), proc = ARG(1), id = ARG(2), ref = ARG(3);
    if (!hwnd || !proc) { RET(0); return; }
    for (int i = 0; i < MAX_SUBCLASS; i++)
        if (g_sub[i].used && g_sub[i].hwnd == hwnd && g_sub[i].proc == proc && g_sub[i].id == id) {
            g_sub[i].ref = ref;                 /* re-registering only updates the data */
            RET(1); return;
        }
    for (int i = 0; i < MAX_SUBCLASS; i++) if (!g_sub[i].used) {
        g_sub[i].used = 1; g_sub[i].hwnd = hwnd; g_sub[i].proc = proc;
        g_sub[i].id = id; g_sub[i].ref = ref;
        w32_window_subclass(w, hwnd, proc, ref);
        RET(1); return;
    }
    RET(0);
}
static void c_GetWindowSubclass(w32 *w) {
    uint64_t hwnd = ARG(0), proc = ARG(1), id = ARG(2), out = ARG(3);
    for (int i = 0; i < MAX_SUBCLASS; i++)
        if (g_sub[i].used && g_sub[i].hwnd == hwnd && g_sub[i].proc == proc && g_sub[i].id == id) {
            if (out) w32_write(w, out, (int)w32_ptrsize(w), g_sub[i].ref);
            RET(1); return;
        }
    RET(0);
}
static void c_RemoveWindowSubclass(w32 *w) {
    uint64_t hwnd = ARG(0), proc = ARG(1), id = ARG(2);
    for (int i = 0; i < MAX_SUBCLASS; i++)
        if (g_sub[i].used && g_sub[i].hwnd == hwnd && g_sub[i].proc == proc && g_sub[i].id == id) {
            g_sub[i].used = 0;
            w32_window_unsubclass(w, hwnd, proc);
            RET(1); return;
        }
    RET(0);
}
/* Whatever the subclass chain sits on top of: our own control procedure, or
 * the window's original one. */
static void c_DefSubclassProc(w32 *w) {
    w32_ret64(w, w32_window_defproc(w, ARG(0), (uint32_t)ARG(1), ARG(2), ARG(3)));
}

/* ------------------------------------------------------------ the rest */

static void c_TrackMouseEvent(w32 *w)  { (void)w; RET(1); }
static void c_GetEffectiveClientRect(w32 *w) { (void)w; RET(0); }
static void c_DllGetVersion(w32 *w) {
    uint64_t p = ARG(0);
    if (!p) { RET((uint64_t)(uint32_t)0x80070057u); return; }
    if (w32_read(w, p, 4) < 16) { RET((uint64_t)(uint32_t)0x80070057u); return; }
    w32_write(w, p + 4,  4, 5);                 /* 5.82: the version that ships with XP onward */
    w32_write(w, p + 8,  4, 82);
    w32_write(w, p + 12, 4, 6000);
    w32_write(w, p + 16, 4, 1);                 /* DLLVER_PLATFORM_WINDOWS */
    RET(0);
}

void w32_comctl32_reset(void) {
    g_inited = 0;
    memset(g_il, 0, sizeof g_il);
    memset(g_sub, 0, sizeof g_sub);
}

#define F(n, a) { #n, a, 0, c_##n, 0 }
const w32_api w32_comctl32[] = {
    F(InitCommonControls, 0), F(InitCommonControlsEx, 1),
    F(ImageList_Create, 5), F(ImageList_Destroy, 1), F(ImageList_Add, 3),
    F(ImageList_AddMasked, 3), F(ImageList_ReplaceIcon, 3),
    F(ImageList_GetImageCount, 1), F(ImageList_SetBkColor, 2),
    F(ImageList_GetIconSize, 3), F(ImageList_Draw, 6), F(ImageList_DrawEx, 10),
    F(ImageList_Remove, 2),
    F(SetWindowSubclass, 4), F(GetWindowSubclass, 4),
    F(RemoveWindowSubclass, 3), F(DefSubclassProc, 4),
    F(GetEffectiveClientRect, 3), F(DllGetVersion, 1),
    { "_TrackMouseEvent", 1, 0, c_TrackMouseEvent, 0 },
    { 0, 0, 0, 0, 0 },
};
#undef F
