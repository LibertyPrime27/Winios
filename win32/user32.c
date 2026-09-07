/* user32.dll -- a window, a message pump, and where input comes from.
 *
 * A Windows game's first act is to register a class, make a window, and then
 * loop: pump messages, read the keyboard and mouse, draw a frame. None of
 * that existed here, which is why `winrun -imports` on a game-shaped program
 * named nine user32 functions as the largest hole left.
 *
 * The shape of it
 * ---------------
 * A window is host-side state behind an HWND the guest holds, the same trick
 * COM objects use: the guest gets an opaque value, we keep the fields. The
 * window's WndProc, though, is *guest* code, so DispatchMessage calls back
 * into the guest through w32_call_guest -- the machinery TLS callbacks and
 * DllMain already go through.
 *
 * Input arrives from outside: the iOS app's key, pointer and touch handlers
 * call w32_input_* from the UI thread while the guest runs on its own. So the
 * input state and the message queue are behind a mutex, and the injection
 * side needs no `w32 *` at all -- it only touches host memory. Only delivery
 * (PeekMessage writing a MSG into guest memory) runs on the guest's thread.
 *
 * Two ways in, because games use both
 * ----------------------------------
 * Messages (WM_KEYDOWN, WM_MOUSEMOVE) are the queue. But a game's frame loop
 * mostly does not read them: it asks GetAsyncKeyState "is W down right now"
 * and GetCursorPos "where is the mouse". So an injected event does two
 * things -- updates the state array immediately, and queues a message. The
 * state is what a frame loop sees; the queue is what a message loop sees; a
 * program that uses both sees the same events through either.
 *
 * Relative motion is kept separately from the pointer position. A menu wants
 * absolute coordinates; mouselook wants deltas, and wants them even when the
 * pointer is against the edge of the screen. DirectInput (which is what
 * Fallout 3 and New Vegas actually read the mouse through) is the next layer
 * up and will read the same accumulator.
 *
 * Structure layouts (WNDCLASS, MSG, DEVMODE, ...) were read out of
 * mingw-w64's headers with offsetof, by a program compiled for Windows and
 * run on this emulator, rather than from memory.
 */
#include "w32.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The display we claim to be. Matches d3d9.c, which reports the same mode. */
enum { SCREEN_W = 1280, SCREEN_H = 720 };

/* --- messages ------------------------------------------------------------ */

enum {
    WM_CREATE = 0x0001, WM_DESTROY = 0x0002, WM_MOVE = 0x0003, WM_SIZE = 0x0005,
    WM_ACTIVATE = 0x0006, WM_SETFOCUS = 0x0007, WM_KILLFOCUS = 0x0008,
    WM_ENABLE = 0x000A, WM_PAINT = 0x000F, WM_CLOSE = 0x0010, WM_QUIT = 0x0012,
    WM_ERASEBKGND = 0x0014, WM_SHOWWINDOW = 0x0018, WM_ACTIVATEAPP = 0x001C,
    WM_SETCURSOR = 0x0020, WM_GETMINMAXINFO = 0x0024, WM_WINDOWPOSCHANGED = 0x0047,
    WM_NCCREATE = 0x0081, WM_NCDESTROY = 0x0082, WM_NCPAINT = 0x0085,
    WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102,
    WM_SYSKEYDOWN = 0x0104, WM_SYSKEYUP = 0x0105, WM_SYSCHAR = 0x0106,
    WM_SYSCOMMAND = 0x0112,
    WM_MOUSEFIRST = 0x0200, WM_MOUSEMOVE = 0x0200,
    WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202, WM_LBUTTONDBLCLK = 0x0203,
    WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205, WM_RBUTTONDBLCLK = 0x0206,
    WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208, WM_MBUTTONDBLCLK = 0x0209,
    WM_MOUSEWHEEL = 0x020A, WM_MOUSELAST = 0x020E,
};
enum { MK_LBUTTON = 1, MK_RBUTTON = 2, MK_SHIFT = 4, MK_CONTROL = 8, MK_MBUTTON = 0x10 };
enum { PM_NOREMOVE = 0, PM_REMOVE = 1 };
enum { SW_HIDE = 0, SW_SHOWNORMAL = 1, SW_SHOW = 5 };

/* Virtual keys we name. The rest are passed through as numbers. */
enum {
    VK_LBUTTON = 0x01, VK_RBUTTON = 0x02, VK_MBUTTON = 0x04,
    VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D,
    VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12, VK_CAPITAL = 0x14,
    VK_ESCAPE = 0x1B, VK_SPACE = 0x20,
    VK_PRIOR = 0x21, VK_NEXT = 0x22, VK_END = 0x23, VK_HOME = 0x24,
    VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
    VK_INSERT = 0x2D, VK_DELETE = 0x2E,
    VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1, VK_LCONTROL = 0xA2, VK_RCONTROL = 0xA3,
    VK_LMENU = 0xA4, VK_RMENU = 0xA5,
};

typedef struct {
    uint64_t hwnd;              /* 0 = whichever window has focus at delivery */
    uint32_t msg;
    uint64_t wparam, lparam;
    uint32_t time;
    int32_t  x, y;              /* screen point, for MSG.pt */
} qmsg;

/* --- state --------------------------------------------------------------- */

enum { MAX_CLASSES = 32, MAX_WINDOWS = 16, MAX_MSGS = 512, GWL_SLOTS = 8 };

typedef struct {
    int      used;
    char     name[96];
    uint64_t wndproc;
    uint32_t style;
    uint64_t cursor, icon, brush, instance;
    int      cls_extra, wnd_extra;
} wclass;

typedef struct {
    int      used;
    char     cls[96];
    uint64_t wndproc;           /* copied from the class, editable by SetWindowLong */
    int      x, y, w, h;        /* window rect */
    int      cw, ch;            /* client size */
    uint32_t style, exstyle;
    int      visible;
    uint64_t instance, param, menu, parent;
    uint64_t longs[GWL_SLOTS];  /* GWL_USERDATA and the window's extra bytes */
} wwin;

static wclass g_cls[MAX_CLASSES];
static wwin   g_win[MAX_WINDOWS];
enum { HW_BASE = 0x00050000u, HW_STEP = 4 };

/* Everything the UI thread and the guest thread both touch. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static qmsg    g_q[MAX_MSGS];
static int     g_qhead, g_qtail;        /* head == tail: empty */
static uint8_t g_keys[256];             /* 0x80 down, 0x01 toggled */
static uint8_t g_keys_hit[256];         /* pressed since the last GetAsyncKeyState */
static uint32_t g_keychar[256];         /* the character the host resolved for this key, if any */
static int32_t g_mx, g_my;              /* pointer, in client pixels */
static int32_t g_rel_dx, g_rel_dy;      /* relative motion not yet consumed */
static uint32_t g_buttons;              /* bit 0 left, 1 right, 2 middle */
static int32_t g_wheel;
static int     g_quit, g_quit_code;
static uint64_t g_focus;                /* the window input goes to */
static uint64_t g_capture;
static int     g_cursor_shown = 1;
static int     g_cursor_count;          /* ShowCursor's counter */

static uint32_t tick_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull);
}

void w32_input_reset(void) {
    pthread_mutex_lock(&g_lock);
    memset(g_cls, 0, sizeof g_cls);
    memset(g_win, 0, sizeof g_win);
    g_qhead = g_qtail = 0;
    memset(g_keys, 0, sizeof g_keys);
    memset(g_keys_hit, 0, sizeof g_keys_hit);
    memset(g_keychar, 0, sizeof g_keychar);
    g_mx = SCREEN_W / 2; g_my = SCREEN_H / 2;
    g_rel_dx = g_rel_dy = 0;
    g_buttons = 0; g_wheel = 0;
    g_quit = 0; g_quit_code = 0;
    g_focus = 0; g_capture = 0;
    g_cursor_shown = 1; g_cursor_count = 0;
    pthread_mutex_unlock(&g_lock);
}

/* Caller holds the lock. A full queue drops the *oldest* message: input that
 * nobody is reading is stale, and the newest is the one that matters. */
static void push(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int x, int y) {
    int next = (g_qtail + 1) % MAX_MSGS;
    if (next == g_qhead) g_qhead = (g_qhead + 1) % MAX_MSGS;
    g_q[g_qtail].hwnd = hwnd;
    g_q[g_qtail].msg = msg;
    g_q[g_qtail].wparam = wp;
    g_q[g_qtail].lparam = lp;
    g_q[g_qtail].time = tick_ms();
    g_q[g_qtail].x = x; g_q[g_qtail].y = y;
    g_qtail = next;
}

static uint32_t mouse_wp(void) {
    uint32_t k = 0;
    if (g_buttons & 1) k |= MK_LBUTTON;
    if (g_buttons & 2) k |= MK_RBUTTON;
    if (g_buttons & 4) k |= MK_MBUTTON;
    if (g_keys[VK_SHIFT] & 0x80) k |= MK_SHIFT;
    if (g_keys[VK_CONTROL] & 0x80) k |= MK_CONTROL;
    return k;
}
static uint64_t xy_lp(int x, int y) { return ((uint64_t)(uint16_t)y << 16) | (uint16_t)x; }

/* --- the host side of input ---------------------------------------------
 *
 * Called from whatever thread the app's input handlers run on, which is not
 * the guest's. Nothing here touches guest memory, so there is no `w32 *` and
 * no possibility of racing the guest's own view of its address space.
 */

/* A key transition, and optionally the character it produces.
 *
 * The character matters because deriving one means knowing the layout, and
 * the host does: iOS hands over both the raw HID usage and the character its
 * keyboard layout resolved. But WM_CHAR is not ours to queue -- Windows
 * produces it inside TranslateMessage, and a program that never calls
 * TranslateMessage is entitled to never see one. So the character is recorded
 * against the key and TranslateMessage uses it instead of guessing; injecting
 * it directly would give a text field two of every letter. */
void w32_input_key_ch(int vk, int down, uint32_t ch) {
    if (vk < 0 || vk > 255) return;
    pthread_mutex_lock(&g_lock);
    if (down) g_keychar[vk] = ch;
    int was = (g_keys[vk] & 0x80) != 0;
    if (down) {
        g_keys[vk] |= 0x80;
        g_keys_hit[vk] = 1;
        if (!was) g_keys[vk] ^= 0x01;                  /* toggle bit, for CapsLock and friends */
    } else g_keys[vk] &= (uint8_t)~0x80;
    /* the left/right pairs and their generic key move together, so a game
     * that asks about either gets a consistent answer */
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) g_keys[VK_SHIFT] = (uint8_t)((g_keys[VK_LSHIFT] | g_keys[VK_RSHIFT]) & 0x80);
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) g_keys[VK_CONTROL] = (uint8_t)((g_keys[VK_LCONTROL] | g_keys[VK_RCONTROL]) & 0x80);
    if (vk == VK_LMENU || vk == VK_RMENU) g_keys[VK_MENU] = (uint8_t)((g_keys[VK_LMENU] | g_keys[VK_RMENU]) & 0x80);
    /* Alt-modified keys are system keys, which is how a game sees Alt+F4 */
    int sys = (g_keys[VK_MENU] & 0x80) && vk != VK_MENU;
    uint32_t msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
    /* lParam: repeat count 1, scan code, and the transition/extended bits */
    uint64_t lp = 1u | ((uint64_t)(vk & 0xFF) << 16);
    if (!down) lp |= 0xC0000000u;                      /* previous state + transition */
    else if (was) lp |= 0x40000000u;
    push(0, msg, (uint64_t)vk, lp, g_mx, g_my);
    pthread_mutex_unlock(&g_lock);
}
void w32_input_key(int vk, int down) { w32_input_key_ch(vk, down, 0); }

/* A character with no key behind it: the system keyboard, or text pasted in.
 * This one really is a WM_CHAR, because there is no key for TranslateMessage
 * to translate. */
void w32_input_char(uint32_t ch) {
    pthread_mutex_lock(&g_lock);
    push(0, WM_CHAR, ch, 1, g_mx, g_my);
    pthread_mutex_unlock(&g_lock);
}

void w32_input_mouse_move(int x, int y) {
    pthread_mutex_lock(&g_lock);
    g_rel_dx += x - g_mx; g_rel_dy += y - g_my;
    g_mx = x; g_my = y;
    push(0, WM_MOUSEMOVE, mouse_wp(), xy_lp(x, y), x, y);
    pthread_mutex_unlock(&g_lock);
}

/* Relative motion with no pointer to move: a trackpad or mouse in the
 * "captured" state a game puts one in for mouselook. The pointer still moves,
 * clamped to the screen, so a program that reads GetCursorPos sees something
 * sensible -- but the deltas are kept whole, because clamping them is what
 * makes a view stop turning at the edge of the screen. */
void w32_input_mouse_delta(int dx, int dy) {
    pthread_mutex_lock(&g_lock);
    g_rel_dx += dx; g_rel_dy += dy;
    g_mx += dx; g_my += dy;
    if (g_mx < 0) g_mx = 0;
    if (g_mx >= SCREEN_W) g_mx = SCREEN_W - 1;
    if (g_my < 0) g_my = 0;
    if (g_my >= SCREEN_H) g_my = SCREEN_H - 1;
    push(0, WM_MOUSEMOVE, mouse_wp(), xy_lp(g_mx, g_my), g_mx, g_my);
    pthread_mutex_unlock(&g_lock);
}

void w32_input_mouse_button(int button, int down) {
    if (button < 0 || button > 2) return;
    static const uint32_t DN[3] = { WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN };
    static const uint32_t UP[3] = { WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP };
    static const int VKB[3] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
    pthread_mutex_lock(&g_lock);
    if (down) { g_buttons |= 1u << button; g_keys[VKB[button]] |= 0x80; g_keys_hit[VKB[button]] = 1; }
    else { g_buttons &= ~(1u << button); g_keys[VKB[button]] &= (uint8_t)~0x80; }
    push(0, down ? DN[button] : UP[button], mouse_wp(), xy_lp(g_mx, g_my), g_mx, g_my);
    pthread_mutex_unlock(&g_lock);
}

void w32_input_mouse_wheel(int delta) {
    pthread_mutex_lock(&g_lock);
    g_wheel += delta;
    /* wParam is delta in the high word, key flags in the low word */
    push(0, WM_MOUSEWHEEL, ((uint64_t)(uint16_t)(int16_t)delta << 16) | mouse_wp(),
         xy_lp(g_mx, g_my), g_mx, g_my);
    pthread_mutex_unlock(&g_lock);
}

/* Where the app should map a touch or pointer into: the focus window's client
 * area, or the display when the guest has not made a window yet. */
void w32_client_size(int *cw, int *ch) {
    pthread_mutex_lock(&g_lock);
    int w = SCREEN_W, h = SCREEN_H;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_win[i].used && (!g_focus || HW_BASE + (uint64_t)i * HW_STEP == g_focus) && g_win[i].cw > 0) {
            w = g_win[i].cw; h = g_win[i].ch;
            if (g_focus) break;
        }
    pthread_mutex_unlock(&g_lock);
    if (cw) *cw = w;
    if (ch) *ch = h;
}

/* Has the guest asked for a window at all? The app uses this to decide
 * whether showing a pointer means anything yet. */
int w32_has_window(void) {
    pthread_mutex_lock(&g_lock);
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (g_win[i].used) n++;
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* --- windows ------------------------------------------------------------- */

static wwin *win_of(uint64_t hwnd) {
    if (hwnd < HW_BASE) return 0;
    uint64_t i = (hwnd - HW_BASE) / HW_STEP;
    if (i >= MAX_WINDOWS || !g_win[i].used) return 0;
    return &g_win[i];
}
static uint64_t hwnd_of(const wwin *p) { return HW_BASE + (uint64_t)(p - g_win) * HW_STEP; }

static wclass *class_of(const char *name) {
    for (int i = 0; i < MAX_CLASSES; i++)
        if (g_cls[i].used && !strcmp(g_cls[i].name, name)) return &g_cls[i];
    return 0;
}

/* WNDCLASS / WNDCLASSEX field offsets, per bitness and per structure. Read
 * from the real headers rather than derived, because a wrong WndProc offset
 * gives a jump to whatever happened to be next in the struct. */
typedef struct { int style, proc, clsx, wndx, inst, icon, cursor, brush, menu, name; } wc_off;
static const wc_off WC32  = {  0,  4,  8, 12, 16, 20, 24, 28, 32, 36 };
static const wc_off WC64  = {  0,  8, 16, 20, 24, 32, 40, 48, 56, 64 };
static const wc_off WCX32 = {  4,  8, 12, 16, 20, 24, 28, 32, 36, 40 };
static const wc_off WCX64 = {  4,  8, 16, 20, 24, 32, 40, 48, 56, 64 };

static void reg_class(w32 *w, int ex, int wide) {
    uint64_t p = ARG(0);
    if (!p) { RET(0); return; }
    const wc_off *o = ex ? (w->is32 ? &WCX32 : &WCX64) : (w->is32 ? &WC32 : &WC64);
    int ps = (int)w32_ptrsize(w);
    char name[96];
    uint64_t np = w32_read(w, p + (unsigned)o->name, ps);
    if (!np) { RET(0); return; }
    if (wide) w32_wtoa(w, np, name, sizeof name);
    else snprintf(name, sizeof name, "%.95s", w32_str(w, np));

    pthread_mutex_lock(&g_lock);
    wclass *c = class_of(name);
    if (!c) for (int i = 0; i < MAX_CLASSES && !c; i++) if (!g_cls[i].used) c = &g_cls[i];
    if (!c) { pthread_mutex_unlock(&g_lock); RET(0); return; }
    c->used = 1;
    snprintf(c->name, sizeof c->name, "%s", name);
    c->style     = (uint32_t)w32_read(w, p + (unsigned)o->style, 4);
    c->wndproc   = w32_read(w, p + (unsigned)o->proc, ps);
    c->cls_extra = (int)(int32_t)w32_read(w, p + (unsigned)o->clsx, 4);
    c->wnd_extra = (int)(int32_t)w32_read(w, p + (unsigned)o->wndx, 4);
    c->instance  = w32_read(w, p + (unsigned)o->inst, ps);
    c->icon      = w32_read(w, p + (unsigned)o->icon, ps);
    c->cursor    = w32_read(w, p + (unsigned)o->cursor, ps);
    c->brush     = w32_read(w, p + (unsigned)o->brush, ps);
    pthread_mutex_unlock(&g_lock);
    if (w->verbose) fprintf(stderr, "winrun: user32: class \"%s\", WndProc %#llx\n",
                            name, (unsigned long long)c->wndproc);
    RET(0xC000 + (c - g_cls));                   /* an ATOM, non-zero for success */
}
static void u_RegisterClassA(w32 *w)   { reg_class(w, 0, 0); }
static void u_RegisterClassW(w32 *w)   { reg_class(w, 0, 1); }
static void u_RegisterClassExA(w32 *w) { reg_class(w, 1, 0); }
static void u_RegisterClassExW(w32 *w) { reg_class(w, 1, 1); }
static void u_UnregisterClassA(w32 *w) { RET(1); }
static void u_UnregisterClassW(w32 *w) { RET(1); }

/* CreateWindowEx(exStyle, class, name, style, x, y, w, h, parent, menu, inst, param).
 * CW_USEDEFAULT and a zero size become the display, which is what a
 * fullscreen game asks for and what the app is going to show anyway. */
static void create_window(w32 *w, int wide) {
    int ps = (int)w32_ptrsize(w);
    char cls[96];
    uint64_t cp = ARG(1);
    if (!cp) { RET(0); return; }
    /* a class can be named or given as an ATOM in the low word */
    if (cp < 0x10000) snprintf(cls, sizeof cls, "#%llu", (unsigned long long)cp);
    else if (wide) w32_wtoa(w, cp, cls, sizeof cls);
    else snprintf(cls, sizeof cls, "%.95s", w32_str(w, cp));

    pthread_mutex_lock(&g_lock);
    wclass *c = class_of(cls);
    if (!c && cp >= 0xC000 && cp < 0xC000 + MAX_CLASSES && g_cls[cp - 0xC000].used) c = &g_cls[cp - 0xC000];
    wwin *p = 0;
    for (int i = 0; i < MAX_WINDOWS && !p; i++) if (!g_win[i].used) p = &g_win[i];
    if (!p) { pthread_mutex_unlock(&g_lock); fprintf(stderr, "winrun: user32: out of windows\n"); RET(0); return; }

    int32_t ax = (int32_t)(uint32_t)ARG(4), ay = (int32_t)(uint32_t)ARG(5);
    int32_t aw = (int32_t)(uint32_t)ARG(6), ah = (int32_t)(uint32_t)ARG(7);
    const int32_t CW_USEDEFAULT = (int32_t)0x80000000;
    if (ax == CW_USEDEFAULT) ax = 0;
    if (ay == CW_USEDEFAULT) ay = 0;
    if (aw == CW_USEDEFAULT || aw <= 0) aw = SCREEN_W;
    if (ah == CW_USEDEFAULT || ah <= 0) ah = SCREEN_H;

    memset(p, 0, sizeof *p);
    p->used = 1;
    snprintf(p->cls, sizeof p->cls, "%s", cls);
    p->wndproc = c ? c->wndproc : 0;
    p->exstyle = (uint32_t)ARG(0);
    p->style   = (uint32_t)ARG(3);
    p->x = ax; p->y = ay; p->w = aw; p->h = ah;
    /* No decorations here, so the client area is the window. A game that asked
     * for a 640x480 client through AdjustWindowRect gets 640x480 back. */
    p->cw = aw; p->ch = ah;
    p->parent = ARG(8); p->menu = ARG(9); p->instance = ARG(10); p->param = ARG(11);
    uint64_t hwnd = hwnd_of(p);
    g_focus = hwnd;
    pthread_mutex_unlock(&g_lock);

    if (w->verbose) fprintf(stderr, "winrun: user32: window %#llx, class \"%s\", %dx%d at %d,%d\n",
                            (unsigned long long)hwnd, cls, aw, ah, ax, ay);

    /* WM_CREATE goes straight to the WndProc, not through the queue -- a
     * program is entitled to have run it before CreateWindowEx returns, and
     * plenty of them set things up there. A non-zero return aborts creation. */
    if (p->wndproc) {
        uint64_t args[4] = { hwnd, WM_CREATE, 0, 0 };
        int64_t r = (int64_t)w32_call_guest(w, p->wndproc, 4, args);
        if (w->exited) return;
        if (r == -1) {
            pthread_mutex_lock(&g_lock);
            p->used = 0; g_focus = 0;
            pthread_mutex_unlock(&g_lock);
            RET(0); return;
        }
    }
    pthread_mutex_lock(&g_lock);
    push(hwnd, WM_SIZE, 0, xy_lp(p->cw, p->ch), 0, 0);
    push(hwnd, WM_ACTIVATEAPP, 1, 0, 0, 0);
    push(hwnd, WM_ACTIVATE, 1, 0, 0, 0);
    push(hwnd, WM_SETFOCUS, 0, 0, 0, 0);
    pthread_mutex_unlock(&g_lock);
    (void)ps;
    RET(hwnd);
}
static void u_CreateWindowExA(w32 *w) { create_window(w, 0); }
static void u_CreateWindowExW(w32 *w) { create_window(w, 1); }

static void u_DestroyWindow(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    uint64_t proc = p ? p->wndproc : 0, hwnd = ARG(0);
    if (p) { p->used = 0; if (g_focus == hwnd) g_focus = 0; }
    pthread_mutex_unlock(&g_lock);
    if (proc) { uint64_t args[4] = { hwnd, WM_DESTROY, 0, 0 }; w32_call_guest(w, proc, 4, args); }
    RET(p ? 1 : 0);
}

static void u_ShowWindow(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    int was = p ? p->visible : 0;
    if (p) p->visible = (uint32_t)ARG(1) != SW_HIDE;
    if (p && p->visible) g_focus = ARG(0);
    pthread_mutex_unlock(&g_lock);
    RET(was);
}
static void u_IsWindowVisible(w32 *w) {
    pthread_mutex_lock(&g_lock); wwin *p = win_of(ARG(0)); int v = p && p->visible; pthread_mutex_unlock(&g_lock);
    RET(v);
}
static void u_IsWindow(w32 *w) {
    pthread_mutex_lock(&g_lock); int v = win_of(ARG(0)) != 0; pthread_mutex_unlock(&g_lock);
    RET(v);
}
static void u_IsIconic(w32 *w) { (void)w; RET(0); }
static void u_IsZoomed(w32 *w) { (void)w; RET(1); }

static void put_rect(w32 *w, uint64_t r, int l, int t, int rr, int b) {
    if (!r) return;
    w32_write(w, r + 0, 4, (uint32_t)l); w32_write(w, r + 4, 4, (uint32_t)t);
    w32_write(w, r + 8, 4, (uint32_t)rr); w32_write(w, r + 12, 4, (uint32_t)b);
}
static void u_GetClientRect(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    int cw = p ? p->cw : SCREEN_W, ch = p ? p->ch : SCREEN_H;
    pthread_mutex_unlock(&g_lock);
    put_rect(w, ARG(1), 0, 0, cw, ch);
    RET(1);
}
static void u_GetWindowRect(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    int x = p ? p->x : 0, y = p ? p->y : 0, cw = p ? p->w : SCREEN_W, ch = p ? p->h : SCREEN_H;
    pthread_mutex_unlock(&g_lock);
    put_rect(w, ARG(1), x, y, x + cw, y + ch);
    RET(1);
}
/* No decorations, so the window rect a client rect needs is itself. */
static void u_AdjustWindowRect(w32 *w) { (void)w; RET(1); }
static void u_AdjustWindowRectEx(w32 *w) { (void)w; RET(1); }

static void u_ClientToScreen(w32 *w) {
    uint64_t pt = ARG(1);
    if (!pt) { RET(0); return; }
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    int ox = p ? p->x : 0, oy = p ? p->y : 0;
    pthread_mutex_unlock(&g_lock);
    w32_write(w, pt + 0, 4, (uint32_t)((int32_t)w32_read(w, pt + 0, 4) + ox));
    w32_write(w, pt + 4, 4, (uint32_t)((int32_t)w32_read(w, pt + 4, 4) + oy));
    RET(1);
}
static void u_ScreenToClient(w32 *w) {
    uint64_t pt = ARG(1);
    if (!pt) { RET(0); return; }
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    int ox = p ? p->x : 0, oy = p ? p->y : 0;
    pthread_mutex_unlock(&g_lock);
    w32_write(w, pt + 0, 4, (uint32_t)((int32_t)w32_read(w, pt + 0, 4) - ox));
    w32_write(w, pt + 4, 4, (uint32_t)((int32_t)w32_read(w, pt + 4, 4) - oy));
    RET(1);
}

static void u_MoveWindow(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    if (p) { p->x = (int32_t)(uint32_t)ARG(1); p->y = (int32_t)(uint32_t)ARG(2);
             p->w = (int32_t)(uint32_t)ARG(3); p->h = (int32_t)(uint32_t)ARG(4);
             p->cw = p->w; p->ch = p->h;
             push(hwnd_of(p), WM_SIZE, 0, xy_lp(p->cw, p->ch), 0, 0); }
    pthread_mutex_unlock(&g_lock);
    RET(p ? 1 : 0);
}
/* SetWindowPos(hwnd, after, x, y, cx, cy, flags): SWP_NOSIZE/NOMOVE are 1/2 */
static void u_SetWindowPos(w32 *w) {
    uint32_t f = (uint32_t)ARG(6);
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    if (p) {
        if (!(f & 2)) { p->x = (int32_t)(uint32_t)ARG(2); p->y = (int32_t)(uint32_t)ARG(3); }
        if (!(f & 1)) {
            int32_t cx = (int32_t)(uint32_t)ARG(4), cy = (int32_t)(uint32_t)ARG(5);
            if (cx > 0 && cy > 0) { p->w = cx; p->h = cy; p->cw = cx; p->ch = cy;
                                    push(hwnd_of(p), WM_SIZE, 0, xy_lp(cx, cy), 0, 0); }
        }
    }
    pthread_mutex_unlock(&g_lock);
    RET(p ? 1 : 0);
}
static void u_SetWindowTextA(w32 *w) {
    if (w->verbose) fprintf(stderr, "winrun: user32: title \"%s\"\n", w32_str(w, ARG(1)));
    RET(1);
}
static void u_SetWindowTextW(w32 *w) { (void)w; RET(1); }
static void u_GetWindowTextA(w32 *w) {
    if (ARG(1) && ARG(2)) w32_write(w, ARG(1), 1, 0);
    RET(0);
}

/* GetWindowLong / SetWindowLong. GWL_WNDPROC (-4) is the one that matters:
 * subclassing is how a launcher hooks a game's window. */
enum { GWL_WNDPROC = -4, GWL_HINSTANCE = -6, GWL_HWNDPARENT = -8,
       GWL_STYLE = -16, GWL_EXSTYLE = -20, GWL_USERDATA = -21 };
static uint64_t win_long_get(w32 *w, uint64_t hwnd, int32_t idx) {
    (void)w;
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    uint64_t v = 0;
    if (p) switch (idx) {
        case GWL_WNDPROC:    v = p->wndproc; break;
        case GWL_HINSTANCE:  v = p->instance; break;
        case GWL_HWNDPARENT: v = p->parent; break;
        case GWL_STYLE:      v = p->style; break;
        case GWL_EXSTYLE:    v = p->exstyle; break;
        case GWL_USERDATA:   v = p->longs[0]; break;
        default: if (idx >= 0 && idx / 4 < GWL_SLOTS - 1) v = p->longs[1 + idx / 4]; break;
    }
    pthread_mutex_unlock(&g_lock);
    return v;
}
static uint64_t win_long_set(w32 *w, uint64_t hwnd, int32_t idx, uint64_t v) {
    uint64_t old = win_long_get(w, hwnd, idx);
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p) switch (idx) {
        case GWL_WNDPROC:  p->wndproc = v; break;
        case GWL_STYLE:    p->style = (uint32_t)v; break;
        case GWL_EXSTYLE:  p->exstyle = (uint32_t)v; break;
        case GWL_USERDATA: p->longs[0] = v; break;
        default: if (idx >= 0 && idx / 4 < GWL_SLOTS - 1) p->longs[1 + idx / 4] = v; break;
    }
    pthread_mutex_unlock(&g_lock);
    return old;
}
static void u_GetWindowLongA(w32 *w) { RET(win_long_get(w, ARG(0), (int32_t)(uint32_t)ARG(1))); }
static void u_SetWindowLongA(w32 *w) { RET(win_long_set(w, ARG(0), (int32_t)(uint32_t)ARG(1), ARG(2))); }
static void u_GetWindowLongPtrA(w32 *w) { w32_ret64(w, win_long_get(w, ARG(0), (int32_t)(uint32_t)ARG(1))); }
static void u_SetWindowLongPtrA(w32 *w) { w32_ret64(w, win_long_set(w, ARG(0), (int32_t)(uint32_t)ARG(1), ARG(2))); }

static void u_GetDesktopWindow(w32 *w) { (void)w; RET(HW_BASE - HW_STEP); }
static void u_GetForegroundWindow(w32 *w) { pthread_mutex_lock(&g_lock); uint64_t f = g_focus; pthread_mutex_unlock(&g_lock); RET(f); }
static void u_SetForegroundWindow(w32 *w) { pthread_mutex_lock(&g_lock); if (win_of(ARG(0))) g_focus = ARG(0); pthread_mutex_unlock(&g_lock); RET(1); }
static void u_GetActiveWindow(w32 *w) { u_GetForegroundWindow(w); }
static void u_SetActiveWindow(w32 *w) { u_SetForegroundWindow(w); }
static void u_GetFocus(w32 *w) { u_GetForegroundWindow(w); }
static void u_SetFocus(w32 *w) { pthread_mutex_lock(&g_lock); uint64_t old = g_focus; if (win_of(ARG(0))) g_focus = ARG(0); pthread_mutex_unlock(&g_lock); RET(old); }
static void u_FindWindowA(w32 *w) { (void)w; RET(0); }
static void u_GetWindowThreadProcessId(w32 *w) {
    if (ARG(1)) w32_write(w, ARG(1), 4, 4242);
    RET(4243);
}
static void u_GetParent(w32 *w) { pthread_mutex_lock(&g_lock); wwin *p = win_of(ARG(0)); uint64_t v = p ? p->parent : 0; pthread_mutex_unlock(&g_lock); RET(v); }

/* --- the message pump ---------------------------------------------------- */

/* MSG field offsets */
static void put_msg(w32 *w, uint64_t m, const qmsg *q, uint64_t hwnd) {
    int ps = (int)w32_ptrsize(w);
    if (w->is32) {
        w32_write(w, m + 0, 4, hwnd);
        w32_write(w, m + 4, 4, q->msg);
        w32_write(w, m + 8, 4, q->wparam);
        w32_write(w, m + 12, 4, q->lparam);
        w32_write(w, m + 16, 4, q->time);
        w32_write(w, m + 20, 4, (uint32_t)q->x);
        w32_write(w, m + 24, 4, (uint32_t)q->y);
    } else {
        w32_write(w, m + 0, 8, hwnd);
        w32_write(w, m + 8, 4, q->msg);
        w32_write(w, m + 16, 8, q->wparam);
        w32_write(w, m + 24, 8, q->lparam);
        w32_write(w, m + 32, 4, q->time);
        w32_write(w, m + 36, 4, (uint32_t)q->x);
        w32_write(w, m + 40, 4, (uint32_t)q->y);
    }
    (void)ps;
}

/* Take the next message, or say there is none. `remove` distinguishes
 * PeekMessage(PM_REMOVE) from a look-ahead, and the filters are the ones
 * games use: a window handle, and a message range. */
static int take(w32 *w, uint64_t m, uint64_t filter_hwnd, uint32_t lo, uint32_t hi, int remove) {
    pthread_mutex_lock(&g_lock);
    int i = g_qhead;
    while (i != g_qtail) {
        qmsg *q = &g_q[i];
        uint64_t target = q->hwnd ? q->hwnd : g_focus;
        int ok = (!filter_hwnd || filter_hwnd == target) &&
                 (!(lo || hi) || (q->msg >= lo && q->msg <= hi));
        if (ok) {
            qmsg copy = *q;
            if (remove) {
                /* shift the ring down over the hole -- messages are few and
                 * order matters more than the cost of moving them */
                int j = i;
                while (j != g_qtail) {
                    int nx = (j + 1) % MAX_MSGS;
                    if (nx == g_qtail) break;
                    g_q[j] = g_q[nx];
                    j = nx;
                }
                g_qtail = (g_qtail - 1 + MAX_MSGS) % MAX_MSGS;
            }
            pthread_mutex_unlock(&g_lock);
            put_msg(w, m, &copy, target);
            return 1;
        }
        i = (i + 1) % MAX_MSGS;
    }
    /* WM_QUIT is not queued: it is a flag, and it stays true until read */
    if (g_quit && !filter_hwnd && !(lo || hi)) {
        qmsg q; memset(&q, 0, sizeof q);
        q.msg = WM_QUIT; q.wparam = (uint64_t)(uint32_t)g_quit_code; q.time = tick_ms();
        if (remove) g_quit = 0;
        pthread_mutex_unlock(&g_lock);
        put_msg(w, m, &q, 0);
        return 1;
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* PeekMessage(msg, hwnd, min, max, flags) */
static void u_PeekMessageA(w32 *w) {
    RET(take(w, ARG(0), ARG(1), (uint32_t)ARG(2), (uint32_t)ARG(3), ((uint32_t)ARG(4) & PM_REMOVE) != 0));
}
static void u_PeekMessageW(w32 *w) { u_PeekMessageA(w); }

/* GetMessage(msg, hwnd, min, max): blocks until there is one. With no other
 * thread to produce input, "blocks" would be a hang -- so an empty queue
 * means the program is waiting for something that is not coming, and it gets
 * WM_QUIT rather than a deadlock. A game's loop uses PeekMessage anyway. */
static void u_GetMessageA(w32 *w) {
    if (take(w, ARG(0), ARG(1), (uint32_t)ARG(2), (uint32_t)ARG(3), 1)) {
        uint64_t m = ARG(0);
        uint32_t msg = (uint32_t)w32_read(w, m + (w->is32 ? 4 : 8), 4);
        RET(msg == WM_QUIT ? 0 : 1);
        return;
    }
    qmsg q; memset(&q, 0, sizeof q);
    q.msg = WM_QUIT; q.time = tick_ms();
    put_msg(w, ARG(0), &q, 0);
    if (w->verbose) fprintf(stderr, "winrun: user32: GetMessage with an empty queue; returning WM_QUIT\n");
    RET(0);
}
static void u_GetMessageW(w32 *w) { u_GetMessageA(w); }

/* TranslateMessage: a WM_KEYDOWN that names a printable key also produces a
 * WM_CHAR. The app sends real characters through w32_input_char when it has
 * them (a hardware keyboard knows its layout better than we do), so this only
 * covers what can be derived: US-layout letters, digits and space. */
static const char SHIFTED[] = ")!@#$%^&*(";
static void u_TranslateMessage(w32 *w) {
    uint64_t m = ARG(0);
    if (!m) { RET(0); return; }
    uint32_t msg = (uint32_t)w32_read(w, m + (w->is32 ? 4 : 8), 4);
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) { RET(0); return; }
    uint32_t vk = (uint32_t)w32_read(w, m + (w->is32 ? 8 : 16), w->is32 ? 4 : 8);
    pthread_mutex_lock(&g_lock);
    int shift = (g_keys[VK_SHIFT] & 0x80) != 0;
    int caps = (g_keys[VK_CAPITAL] & 0x01) != 0;
    uint32_t hostch = vk < 256 ? g_keychar[vk] : 0;
    pthread_mutex_unlock(&g_lock);
    int ch = 0;
    /* the host's own layout resolved it: better than anything derived here */
    if (hostch) ch = (int)hostch;
    else if (vk >= 'A' && vk <= 'Z') ch = (shift ^ caps) ? (int)vk : (int)vk + 32;
    else if (vk >= '0' && vk <= '9') ch = shift ? SHIFTED[vk - '0'] : (int)vk;
    else if (vk == VK_SPACE) ch = ' ';
    else if (vk == VK_RETURN) ch = '\r';
    else if (vk == VK_TAB) ch = '\t';
    else if (vk == VK_BACK) ch = '\b';
    else if (vk == VK_ESCAPE) ch = 27;
    if (ch) { pthread_mutex_lock(&g_lock); push(0, WM_CHAR, (uint64_t)ch, 1, g_mx, g_my); pthread_mutex_unlock(&g_lock); }
    RET(ch != 0);
}

/* DispatchMessage: call the window's WndProc, which is guest code. */
static void u_DispatchMessageA(w32 *w) {
    uint64_t m = ARG(0);
    if (!m) { RET(0); return; }
    int ps = (int)w32_ptrsize(w);
    uint64_t hwnd = w32_read(w, m + 0, ps);
    uint32_t msg = (uint32_t)w32_read(w, m + (w->is32 ? 4 : 8), 4);
    uint64_t wp  = w32_read(w, m + (w->is32 ? 8 : 16), ps);
    uint64_t lp  = w32_read(w, m + (w->is32 ? 12 : 24), ps);
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    uint64_t proc = p ? p->wndproc : 0;
    pthread_mutex_unlock(&g_lock);
    if (!proc) { RET(0); return; }
    uint64_t args[4] = { hwnd, msg, wp, lp };
    RET(w32_call_guest(w, proc, 4, args));
}
static void u_DispatchMessageW(w32 *w) { u_DispatchMessageA(w); }

/* SendMessage runs the WndProc now; PostMessage queues it. */
static void u_SendMessageA(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    uint64_t proc = p ? p->wndproc : 0;
    pthread_mutex_unlock(&g_lock);
    if (!proc) { RET(0); return; }
    uint64_t args[4] = { ARG(0), ARG(1), ARG(2), ARG(3) };
    RET(w32_call_guest(w, proc, 4, args));
}
static void u_SendMessageW(w32 *w) { u_SendMessageA(w); }
static void u_PostMessageA(w32 *w) {
    pthread_mutex_lock(&g_lock);
    push(ARG(0), (uint32_t)ARG(1), ARG(2), ARG(3), g_mx, g_my);
    pthread_mutex_unlock(&g_lock);
    RET(1);
}
static void u_PostMessageW(w32 *w) { u_PostMessageA(w); }
static void u_PostThreadMessageA(w32 *w) {
    pthread_mutex_lock(&g_lock);
    push(0, (uint32_t)ARG(1), ARG(2), ARG(3), g_mx, g_my);
    pthread_mutex_unlock(&g_lock);
    RET(1);
}
static void u_PostQuitMessage(w32 *w) {
    pthread_mutex_lock(&g_lock);
    g_quit = 1; g_quit_code = (int)(int32_t)(uint32_t)ARG(0);
    pthread_mutex_unlock(&g_lock);
}

/* DefWindowProc: the default handling a program relies on for the messages it
 * does not care about. WM_CLOSE destroying the window and WM_DESTROY posting
 * the quit message is the part that makes a normal message loop terminate. */
static void u_DefWindowProcA(w32 *w) {
    uint64_t hwnd = ARG(0);
    uint32_t msg = (uint32_t)ARG(1);
    switch (msg) {
    case WM_CLOSE:
        pthread_mutex_lock(&g_lock);
        { wwin *p = win_of(hwnd); if (p) p->used = 0; if (g_focus == hwnd) g_focus = 0; }
        push(hwnd, WM_DESTROY, 0, 0, 0, 0);
        pthread_mutex_unlock(&g_lock);
        RET(0); return;
    case WM_DESTROY:
        pthread_mutex_lock(&g_lock);
        g_quit = 1;
        pthread_mutex_unlock(&g_lock);
        RET(0); return;
    case WM_SETCURSOR: RET(1); return;
    case WM_ERASEBKGND: RET(1); return;
    default: RET(0); return;
    }
}
static void u_DefWindowProcW(w32 *w) { u_DefWindowProcA(w); }
static void u_CallWindowProcA(w32 *w) {
    uint64_t proc = ARG(0);
    if (!proc) { RET(0); return; }
    uint64_t args[4] = { ARG(1), ARG(2), ARG(3), ARG(4) };
    RET(w32_call_guest(w, proc, 4, args));
}
static void u_WaitMessage(w32 *w) { (void)w; RET(1); }
static void u_GetMessageTime(w32 *w) { (void)w; RET(tick_ms()); }
static void u_GetMessagePos(w32 *w) {
    pthread_mutex_lock(&g_lock); uint64_t v = xy_lp(g_mx, g_my); pthread_mutex_unlock(&g_lock);
    RET(v);
}
static void u_GetQueueStatus(w32 *w) {
    pthread_mutex_lock(&g_lock); int any = g_qhead != g_qtail || g_quit; pthread_mutex_unlock(&g_lock);
    RET(any ? 0x00FF00FFu : 0);
}

/* --- polled input, which is what a frame loop uses ---------------------- */

/* GetAsyncKeyState: 0x8000 = down now, 0x0001 = pressed since the last call.
 * The "since the last call" bit really is per-call in Windows, and a game
 * that uses it for single-shot actions breaks if it is left set. */
static void u_GetAsyncKeyState(w32 *w) {
    int vk = (int)(uint32_t)ARG(0) & 0xFF;
    pthread_mutex_lock(&g_lock);
    uint32_t v = (g_keys[vk] & 0x80) ? 0x8000u : 0;
    if (g_keys_hit[vk]) { v |= 1; g_keys_hit[vk] = 0; }
    pthread_mutex_unlock(&g_lock);
    RET((uint64_t)(int64_t)(int16_t)(uint16_t)v);
}
static void u_GetKeyState(w32 *w) {
    int vk = (int)(uint32_t)ARG(0) & 0xFF;
    pthread_mutex_lock(&g_lock);
    uint32_t v = ((g_keys[vk] & 0x80) ? 0x8000u : 0) | (g_keys[vk] & 0x01);
    pthread_mutex_unlock(&g_lock);
    RET((uint64_t)(int64_t)(int16_t)(uint16_t)v);
}
static void u_GetKeyboardState(w32 *w) {
    uint64_t p = ARG(0);
    if (!p) { RET(0); return; }
    pthread_mutex_lock(&g_lock);
    memcpy(W32P(w, p), g_keys, 256);
    pthread_mutex_unlock(&g_lock);
    RET(1);
}
static void u_SetKeyboardState(w32 *w) {
    uint64_t p = ARG(0);
    if (p) { pthread_mutex_lock(&g_lock); memcpy(g_keys, W32P(w, p), 256); pthread_mutex_unlock(&g_lock); }
    RET(1);
}

static void u_GetCursorPos(w32 *w) {
    uint64_t p = ARG(0);
    if (!p) { RET(0); return; }
    pthread_mutex_lock(&g_lock);
    int32_t x = g_mx, y = g_my;
    pthread_mutex_unlock(&g_lock);
    w32_write(w, p + 0, 4, (uint32_t)x);
    w32_write(w, p + 4, 4, (uint32_t)y);
    RET(1);
}
/* A game recentres the pointer every frame to keep mouselook going. It must
 * not look like motion, or the view spins: the position moves, the relative
 * accumulator does not. */
static void u_SetCursorPos(w32 *w) {
    pthread_mutex_lock(&g_lock);
    g_mx = (int32_t)(uint32_t)ARG(0); g_my = (int32_t)(uint32_t)ARG(1);
    pthread_mutex_unlock(&g_lock);
    RET(1);
}
static void u_ShowCursor(w32 *w) {
    pthread_mutex_lock(&g_lock);
    g_cursor_count += ARG(0) ? 1 : -1;
    g_cursor_shown = g_cursor_count >= 0;
    int c = g_cursor_count;
    pthread_mutex_unlock(&g_lock);
    RET((uint64_t)(int64_t)c);
}
static void u_SetCursor(w32 *w) { (void)w; RET(0); }
static void u_LoadCursorA(w32 *w) { (void)w; RET(0x9001); }
static void u_LoadCursorW(w32 *w) { (void)w; RET(0x9001); }
static void u_LoadIconA(w32 *w) { (void)w; RET(0x9002); }
static void u_LoadIconW(w32 *w) { (void)w; RET(0x9002); }
static void u_LoadImageA(w32 *w) { (void)w; RET(0x9003); }
static void u_SetCapture(w32 *w) {
    pthread_mutex_lock(&g_lock); uint64_t old = g_capture; g_capture = ARG(0); pthread_mutex_unlock(&g_lock);
    RET(old);
}
static void u_ReleaseCapture(w32 *w) { pthread_mutex_lock(&g_lock); g_capture = 0; pthread_mutex_unlock(&g_lock); RET(1); }
static void u_GetCapture(w32 *w) { pthread_mutex_lock(&g_lock); uint64_t c = g_capture; pthread_mutex_unlock(&g_lock); RET(c); }
/* Confining the pointer is what the host does or does not do; either way the
 * guest's own idea of where it is stays inside the window. */
static void u_ClipCursor(w32 *w) { (void)w; RET(1); }
static void u_GetClipCursor(w32 *w) { put_rect(w, ARG(0), 0, 0, SCREEN_W, SCREEN_H); RET(1); }

/* Whether the pointer the app draws should be visible, and where it is. The
 * app asks so a virtual cursor can disappear exactly when the game hides the
 * real one -- which is how a game says "I am doing mouselook now". */
int w32_cursor_visible(void) {
    pthread_mutex_lock(&g_lock); int v = g_cursor_shown; pthread_mutex_unlock(&g_lock);
    return v;
}
void w32_cursor_pos(int *x, int *y) {
    pthread_mutex_lock(&g_lock);
    if (x) *x = g_mx;
    if (y) *y = g_my;
    pthread_mutex_unlock(&g_lock);
}

/* MapVirtualKey: only the mappings a program is likely to ask for. The scan
 * codes here are the ones w32_input_key puts in lParam, so a round trip
 * agrees with itself. */
static void u_MapVirtualKeyA(w32 *w) {
    uint32_t code = (uint32_t)ARG(0), type = (uint32_t)ARG(1);
    if (type == 0) RET(code & 0xFF);              /* VK -> scan */
    else if (type == 1 || type == 3) RET(code & 0xFF);  /* scan -> VK */
    else if (type == 2) {                          /* VK -> char */
        if (code >= 'A' && code <= 'Z') RET(code);
        else if (code >= '0' && code <= '9') RET(code);
        else RET(0);
    } else RET(0);
}
static void u_MapVirtualKeyW(w32 *w) { u_MapVirtualKeyA(w); }
static void u_VkKeyScanA(w32 *w) {
    int c = (int)(uint32_t)ARG(0) & 0xFF;
    if (c >= 'a' && c <= 'z') RET(c - 32);
    else if (c >= 'A' && c <= 'Z') RET(c | 0x100);   /* needs shift */
    else RET(c);
}
static void u_GetKeyNameTextA(w32 *w) {
    uint64_t p = ARG(1);
    uint32_t cap = (uint32_t)ARG(2);
    int vk = (int)(((uint32_t)ARG(0) >> 16) & 0xFF);
    char buf[16];
    if (vk >= 'A' && vk <= 'Z') snprintf(buf, sizeof buf, "%c", vk);
    else snprintf(buf, sizeof buf, "0x%02X", vk);
    uint32_t n = (uint32_t)strlen(buf);
    if (p && cap > n) memcpy(W32P(w, p), buf, n + 1);
    RET(p && cap > n ? n : 0);
}

/* --- the display -------------------------------------------------------- */

/* GetSystemMetrics: SM_CXSCREEN 0, SM_CYSCREEN 1, SM_CXFULLSCREEN 16,
 * SM_CYFULLSCREEN 17, SM_CMOUSEBUTTONS 43, SM_MOUSEPRESENT 19. A game reads
 * these to pick a resolution, so they have to agree with what d3d9 reports. */
static void u_GetSystemMetrics(w32 *w) {
    switch ((uint32_t)ARG(0)) {
    case 0: case 16: case 61: RET(SCREEN_W); return;
    case 1: case 17: case 62: RET(SCREEN_H); return;
    case 19: RET(1); return;                       /* a mouse is present */
    case 43: RET(3); return;                       /* three buttons */
    case 75: RET(1); return;                       /* SM_MOUSEWHEELPRESENT */
    case 4: RET(0); return;                        /* SM_CYCAPTION: no decorations */
    case 5: case 6: case 7: case 8: RET(0); return; /* borders */
    case 80: RET(1); return;                       /* SM_CMONITORS */
    default: RET(0); return;
    }
}

/* EnumDisplaySettings(device, mode, devmode): one mode, the one we present.
 * dmFields says which fields are meaningful: BITSPERPEL|PELSWIDTH|PELSHEIGHT|
 * DISPLAYFREQUENCY = 0x0004|0x0008|0x0010|0x0400. */
static void u_EnumDisplaySettingsA(w32 *w) {
    uint32_t mode = (uint32_t)ARG(1), dm = 0;
    uint64_t p = ARG(2);
    if (!p) { RET(0); return; }
    if (mode != 0 && mode != 0xFFFFFFFFu) { RET(0); return; }   /* one mode, and ENUM_CURRENT */
    memset(W32P(w, p), 0, 156);
    memcpy(W32P(w, p), "winios", 7);
    w32_write(w, p + 32, 2, 0x0401);               /* dmSpecVersion */
    dm = 0x0004 | 0x0008 | 0x0010 | 0x0400;
    w32_write(w, p + 40, 4, dm);
    w32_write(w, p + 104, 4, 32);                  /* dmBitsPerPel */
    w32_write(w, p + 108, 4, SCREEN_W);
    w32_write(w, p + 112, 4, SCREEN_H);
    w32_write(w, p + 120, 4, 60);                  /* dmDisplayFrequency */
    RET(1);
}
static void u_EnumDisplaySettingsW(w32 *w) { u_EnumDisplaySettingsA(w); }
static void u_EnumDisplayDevicesA(w32 *w) { (void)w; RET(0); }
/* A mode change is a request we always grant, because there is one mode and
 * the app scales whatever the guest draws. DISP_CHANGE_SUCCESSFUL = 0. */
static void u_ChangeDisplaySettingsA(w32 *w) { (void)w; RET(0); }
static void u_ChangeDisplaySettingsExA(w32 *w) { (void)w; RET(0); }
static void u_MonitorFromWindow(w32 *w) { (void)w; RET(0x9101); }
static void u_MonitorFromPoint(w32 *w) { (void)w; RET(0x9101); }
static void u_GetMonitorInfoA(w32 *w) {
    uint64_t p = ARG(1);
    if (!p) { RET(0); return; }
    put_rect(w, p + 4, 0, 0, SCREEN_W, SCREEN_H);
    put_rect(w, p + 20, 0, 0, SCREEN_W, SCREEN_H);
    w32_write(w, p + 36, 4, 1);                    /* MONITORINFOF_PRIMARY */
    RET(1);
}

/* Painting. There is no GDI here: a game draws through Direct3D and only
 * calls these to satisfy the message loop. BeginPaint has to clear the paint
 * region or WM_PAINT arrives forever, which it does here by not queueing one. */
static void u_BeginPaint(w32 *w) {
    uint64_t ps = ARG(1);
    if (ps) {
        memset(W32P(w, ps), 0, w->is32 ? 64 : 72);
        w32_write(w, ps + 0, (int)w32_ptrsize(w), 0x9201);         /* an HDC */
        put_rect(w, ps + (w->is32 ? 8 : 12), 0, 0, SCREEN_W, SCREEN_H);
    }
    RET(0x9201);
}
static void u_EndPaint(w32 *w) { (void)w; RET(1); }
static void u_GetDC(w32 *w) { (void)w; RET(0x9201); }
static void u_GetWindowDC(w32 *w) { (void)w; RET(0x9201); }
static void u_ReleaseDC(w32 *w) { (void)w; RET(1); }
static void u_InvalidateRect(w32 *w) { (void)w; RET(1); }
static void u_UpdateWindow(w32 *w) { (void)w; RET(1); }
static void u_ValidateRect(w32 *w) { (void)w; RET(1); }
static void u_FillRect(w32 *w) { (void)w; RET(1); }
static void u_SetTimer(w32 *w) { RET(ARG(1) ? ARG(1) : 1); }
static void u_KillTimer(w32 *w) { (void)w; RET(1); }
static void u_SystemParametersInfoA(w32 *w) { (void)w; RET(1); }
static void u_SetProcessDPIAware(w32 *w) { (void)w; RET(1); }
static void u_GetSysColorBrush(w32 *w) { (void)w; RET(0x9301); }
static void u_GetSysColor(w32 *w) { (void)w; RET(0); }
static void u_MessageBoxA(w32 *w) {
    fprintf(stderr, "[MessageBox] %s: %s\n", w32_str(w, ARG(2)), w32_str(w, ARG(1)));
    RET(1);
}
static void u_MessageBoxW(w32 *w) {
    char t[256], b[512];
    w32_wtoa(w, ARG(2), t, sizeof t);
    w32_wtoa(w, ARG(1), b, sizeof b);
    fprintf(stderr, "[MessageBox] %s: %s\n", t, b);
    RET(1);
}
static void u_CharUpperA(w32 *w) { uint64_t p = ARG(0); if (p < 0x10000) { int c = (int)p; RET(c >= 'a' && c <= 'z' ? c - 32 : c); return; } RET(p); }
static void u_CharNextA(w32 *w) { uint64_t p = ARG(0); RET(p && *w32_str(w, p) ? p + 1 : p); }
#define F(n, a)  { #n, a, 0, u_##n, 0 }
const w32_api w32_user32[] = {
    /* classes and windows */
    F(RegisterClassA, 1), F(RegisterClassW, 1), F(RegisterClassExA, 1), F(RegisterClassExW, 1),
    F(UnregisterClassA, 2), F(UnregisterClassW, 2),
    F(CreateWindowExA, 12), F(CreateWindowExW, 12),
    F(DestroyWindow, 1), F(ShowWindow, 2), F(UpdateWindow, 1),
    F(IsWindow, 1), F(IsWindowVisible, 1), F(IsIconic, 1), F(IsZoomed, 1),
    F(GetClientRect, 2), F(GetWindowRect, 2), F(AdjustWindowRect, 3), F(AdjustWindowRectEx, 4),
    F(ClientToScreen, 2), F(ScreenToClient, 2),
    F(MoveWindow, 6), F(SetWindowPos, 7),
    F(SetWindowTextA, 2), F(SetWindowTextW, 2), F(GetWindowTextA, 3),
    F(GetWindowLongA, 2), F(SetWindowLongA, 3), F(GetWindowLongPtrA, 2), F(SetWindowLongPtrA, 3),
    F(GetDesktopWindow, 0), F(GetForegroundWindow, 0), F(SetForegroundWindow, 1),
    F(GetActiveWindow, 0), F(SetActiveWindow, 1), F(GetFocus, 0), F(SetFocus, 1),
    F(FindWindowA, 2), F(GetWindowThreadProcessId, 2), F(GetParent, 1),
    /* the message pump */
    F(PeekMessageA, 5), F(PeekMessageW, 5), F(GetMessageA, 4), F(GetMessageW, 4),
    F(TranslateMessage, 1), F(DispatchMessageA, 1), F(DispatchMessageW, 1),
    F(SendMessageA, 4), F(SendMessageW, 4), F(PostMessageA, 4), F(PostMessageW, 4),
    F(PostThreadMessageA, 4), F(PostQuitMessage, 1),
    F(DefWindowProcA, 4), F(DefWindowProcW, 4), F(CallWindowProcA, 5),
    F(WaitMessage, 0), F(GetMessageTime, 0), F(GetMessagePos, 0), F(GetQueueStatus, 1),
    /* input */
    F(GetAsyncKeyState, 1), F(GetKeyState, 1), F(GetKeyboardState, 1), F(SetKeyboardState, 1),
    F(GetCursorPos, 1), F(SetCursorPos, 2), F(ShowCursor, 1), F(SetCursor, 1),
    F(LoadCursorA, 2), F(LoadCursorW, 2), F(LoadIconA, 2), F(LoadIconW, 2), F(LoadImageA, 6),
    F(SetCapture, 1), F(ReleaseCapture, 0), F(GetCapture, 0),
    F(ClipCursor, 1), F(GetClipCursor, 1),
    F(MapVirtualKeyA, 2), F(MapVirtualKeyW, 2), F(VkKeyScanA, 1), F(GetKeyNameTextA, 3),
    /* the display, and the calls a message loop needs to get through */
    F(GetSystemMetrics, 1),
    F(EnumDisplaySettingsA, 3), F(EnumDisplaySettingsW, 3), F(EnumDisplayDevicesA, 4),
    F(ChangeDisplaySettingsA, 2), F(ChangeDisplaySettingsExA, 5),
    F(MonitorFromWindow, 2), F(MonitorFromPoint, 3), F(GetMonitorInfoA, 2),
    F(BeginPaint, 2), F(EndPaint, 2), F(GetDC, 1), F(GetWindowDC, 1), F(ReleaseDC, 2),
    F(InvalidateRect, 3), F(ValidateRect, 2), F(FillRect, 3),
    F(SetTimer, 4), F(KillTimer, 2), F(SystemParametersInfoA, 4), F(SetProcessDPIAware, 0),
    F(GetSysColorBrush, 1), F(GetSysColor, 1),
    F(MessageBoxA, 4), F(MessageBoxW, 4), F(CharUpperA, 1), F(CharNextA, 1),
    { 0, 0, 0, 0, 0 },
};
