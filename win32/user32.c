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
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* The display we claim to be. d3d9.c reads the same pair through
 * w32_screen_size(), rather than the two files holding the same constant and
 * a comment asking them to stay in step.
 *
 * 1280x720 is the default because it is a mode every game recognises and it
 * is half the pixels of 1080p. Lowering it is the biggest thing a person can
 * do for frame rate while DrawPrimitive still goes through the software
 * rasterizer, which is why the app can set it. */
static int g_screen_w = 1280, g_screen_h = 720;

void w32_set_screen_size(int cx, int cy) {
    /* Bounded rather than trusted: a game that is told the screen is 8 pixels
     * wide does something unhelpful, and one told it is 32768 wide tries to
     * allocate a backbuffer that cannot exist. */
    if (cx >= 320 && cy >= 200 && cx <= 7680 && cy <= 4320) {
        g_screen_w = cx;
        g_screen_h = cy;
    }
}
void w32_screen_size(int *cx, int *cy) {
    if (cx) *cx = g_screen_w;
    if (cy) *cy = g_screen_h;
}
#define SCREEN_W g_screen_w
#define SCREEN_H g_screen_h

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
/* Window styles that change how a control draws. */
enum {
    WS_CHILD = 0x40000000u, WS_VISIBLE = 0x10000000u, WS_DISABLED = 0x08000000u,
    WS_BORDER = 0x00800000u, WS_GROUP = 0x00020000u, WS_TABSTOP = 0x00010000u,
    WS_CAPTION = 0x00C00000u, WS_POPUP = 0x80000000u, WS_VSCROLL = 0x00200000u,
    BS_DEFPUSHBUTTON = 1, BS_CHECKBOX = 2, BS_AUTOCHECKBOX = 3,
    BS_RADIOBUTTON = 4, BS_3STATE = 5, BS_AUTO3STATE = 6, BS_GROUPBOX = 7,
    BS_AUTORADIOBUTTON = 9, BS_OWNERDRAW = 11, BS_TYPEMASK = 0x0F,
    SS_LEFT = 0, SS_CENTER = 1, SS_RIGHT = 2, SS_ICON = 3,
    SS_BLACKRECT = 4, SS_GRAYRECT = 5, SS_WHITERECT = 6,
    SS_ETCHEDHORZ = 0x10, SS_ETCHEDVERT = 0x11, SS_ETCHEDFRAME = 0x12,
    SS_TYPEMASK = 0x1F,
    ES_CENTER = 1, ES_RIGHT = 2, ES_MULTILINE = 4, ES_PASSWORD = 0x20,
    ES_READONLY = 0x800,
};

/* Control messages. */
enum {
    WM_SETTEXT = 0x000C, WM_GETTEXT = 0x000D, WM_GETTEXTLENGTH = 0x000E,
    WM_SETFONT = 0x0030, WM_GETFONT = 0x0031, WM_SETREDRAW = 0x000B,
    WM_COMMAND = 0x0111, WM_NOTIFY = 0x004E, WM_TIMER = 0x0113,
    WM_INITDIALOG = 0x0110, WM_CTLCOLORSTATIC = 0x0138, WM_CTLCOLORBTN = 0x0135,
    WM_CTLCOLOREDIT = 0x0133, WM_CTLCOLORDLG = 0x0136, WM_CTLCOLORLISTBOX = 0x0134,
    BM_GETCHECK = 0x00F0, BM_SETCHECK = 0x00F1, BM_GETSTATE = 0x00F2,
    BM_SETSTATE = 0x00F3, BM_CLICK = 0x00F5,
    EM_GETSEL = 0x00B0, EM_SETSEL = 0x00B1, EM_SETREADONLY = 0x00CF,
    EM_LIMITTEXT = 0x00C5, EM_SETMODIFY = 0x00B9, EM_GETMODIFY = 0x00B8,
    LB_ADDSTRING = 0x0180, LB_INSERTSTRING = 0x0181, LB_DELETESTRING = 0x0182,
    LB_RESETCONTENT = 0x0184, LB_GETCOUNT = 0x018B, LB_GETCURSEL = 0x0188,
    LB_SETCURSEL = 0x0186, LB_GETTEXT = 0x0189, LB_GETTEXTLEN = 0x018A,
    CB_ADDSTRING = 0x0143, CB_RESETCONTENT = 0x014B, CB_GETCURSEL = 0x0147,
    CB_SETCURSEL = 0x014E, CB_GETCOUNT = 0x0146, CB_GETLBTEXT = 0x0148,
    PBM_SETRANGE = 0x0401, PBM_SETPOS = 0x0402, PBM_DELTAPOS = 0x0403,
    PBM_SETSTEP = 0x0404, PBM_STEPIT = 0x0405, PBM_SETRANGE32 = 0x0406,
    PBM_GETPOS = 0x0408, PBM_SETMARQUEE = 0x040A,
    BN_CLICKED = 0, EN_CHANGE = 0x0300, LBN_SELCHANGE = 1,
    DM_GETDEFID = 0x0400, DM_SETDEFID = 0x0401,
};

/* System colours, by the GetSysColor index. These are the Windows Classic
 * values rather than an invented palette: a program that draws part of a
 * control itself picks its colours from here, and its half has to match
 * ours. */
enum {
    COLOR_SCROLLBAR = 0, COLOR_BACKGROUND = 1, COLOR_ACTIVECAPTION = 2,
    COLOR_MENU = 4, COLOR_WINDOW = 5, COLOR_WINDOWFRAME = 6, COLOR_MENUTEXT = 7,
    COLOR_WINDOWTEXT = 8, COLOR_CAPTIONTEXT = 9, COLOR_BTNFACE = 15,
    COLOR_BTNSHADOW = 16, COLOR_GRAYTEXT = 17, COLOR_BTNTEXT = 18,
    COLOR_BTNHIGHLIGHT = 20, COLOR_3DDKSHADOW = 21, COLOR_3DLIGHT = 22,
    COLOR_HIGHLIGHT = 13, COLOR_HIGHLIGHTTEXT = 14, COLOR_NCOLORS = 31,
};
static uint32_t sys_color(int i);

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

/* A game has one window. A dialog has one per control, and an installer's
 * wizard page runs to a couple of dozen, so the old limit of 16 was a limit
 * on whether an installer could be drawn at all. */
enum { MAX_CLASSES = 64, MAX_WINDOWS = 192, MAX_MSGS = 512, GWL_SLOTS = 8 };

/* The controls we draw ourselves. A window of one of these classes has no
 * guest window procedure: ours is behind it, at a pseudo-address in the
 * HPROC range, so SetWindowLong(GWL_WNDPROC) and CallWindowProc keep working
 * on it -- a program that subclasses a button gets our procedure as the one
 * to chain to, exactly as it would on Windows. */
typedef enum {
    CTL_NONE = 0, CTL_DIALOG, CTL_STATIC, CTL_BUTTON, CTL_EDIT,
    CTL_LISTBOX, CTL_COMBOBOX, CTL_PROGRESS, CTL_PANE, CTL_N
} ctlkind;
enum { HPROC_BASE = 0x00030000u, HPROC_STEP = 4 };

/* There is no window manager here, so a window with WS_CAPTION draws its own
 * title bar -- and a title bar takes room. Keeping it *outside* the client
 * area is not cosmetic: a dialog template positions every control from the
 * client origin, and a caption drawn over the client area puts the first row
 * of controls underneath it. */
enum { CAPTION_H = 22 };

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
    int      x, y, w, h;        /* window rect, in screen coordinates */
    int      cw, ch;            /* client size */
    int      cxo, cyo;          /* client origin within the window: the caption bar */
    uint32_t style, exstyle;
    int      visible;
    uint64_t instance, param, menu, parent;
    uint64_t longs[GWL_SLOTS];  /* GWL_USERDATA and the window's extra bytes */

    /* what it takes to draw one */
    char     text[256];         /* the window text: a caption, a label, a field */
    uint64_t font;              /* WM_SETFONT, or 0 for the default */
    ctlkind  ctl;               /* non-zero: we draw it, we handle its clicks */
    uint64_t id;                /* child identifier -- the menu argument */
    int      enabled;
    int      checked, pressed;  /* a button's two visible states */
    int      pos, lo, hi, step; /* a progress bar's range */
    int      list;              /* index into the list-box store, -1 for none */
    int      sel;               /* selected item */
    int      caret;             /* insertion point in an edit field */
    /* a dialog */
    uint64_t dlgproc;
    int      is_dialog, ending, result, default_id;
    uint64_t userdata;          /* DWL_USER */
    uint64_t subclass_ref;
} wwin;

/* List boxes are rare and their contents are not: one store per list box
 * that exists, rather than a fixed array on every window. */
enum { MAX_LISTS = 8, LIST_ITEMS = 64, LIST_TEXT = 96 };
static struct { int used, n; char item[LIST_ITEMS][LIST_TEXT]; } g_list[MAX_LISTS];

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

static void ui_reset_tables(void);   /* the drawing side, defined with its tables */

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
    memset(g_list, 0, sizeof g_list);
    ui_reset_tables();
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

/* Making a window, with the arguments as values rather than as guest stack
 * slots -- because a dialog template creates windows too, and it has no
 * stack frame to read them out of. CreateWindowEx is a thin wrapper over
 * this; so is every control a dialog puts on screen.
 *
 * CW_USEDEFAULT and a zero size become the display, which is what a
 * fullscreen game asks for and what the app is going to show anyway. */
static ctlkind ctl_of_class(const char *cls);
static int is_extra_class(const char *cls);
static uint64_t hproc_addr(ctlkind k);
static int is_hproc(uint64_t a);
static void invalidate(uint64_t hwnd);
static uint64_t send_to(w32 *w, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int wide);
static uint64_t deliver(w32 *w, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int wide);
static uint64_t call_proc(w32 *w, uint64_t proc, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int wide);
static uint64_t ctl_proc(w32 *w, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int wide);
static uint64_t dlg_default_proc(w32 *w, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int wide);
static void paint_control(w32 *w, uint64_t hwnd, wwin *snap);
static uint64_t window_at(int x, int y);
static void surface_present(void);

uint64_t w32_new_window(w32 *w, const char *cls, const char *text,
                        uint32_t style, uint32_t exstyle,
                        int ax, int ay, int aw, int ah,
                        uint64_t parent, uint64_t id, uint64_t inst, uint64_t param,
                        int run_wm_create) {
    pthread_mutex_lock(&g_lock);
    wclass *c = class_of(cls);
    wwin *p = 0;
    for (int i = 0; i < MAX_WINDOWS && !p; i++) if (!g_win[i].used) p = &g_win[i];
    if (!p) { pthread_mutex_unlock(&g_lock); fprintf(stderr, "winrun: user32: out of windows\n"); return 0; }

    const int32_t CW_USEDEFAULT = (int32_t)0x80000000;
    if (ax == CW_USEDEFAULT) ax = 0;
    if (ay == CW_USEDEFAULT) ay = 0;
    if (aw == CW_USEDEFAULT || aw <= 0) aw = SCREEN_W;
    if (ah == CW_USEDEFAULT || ah <= 0) ah = SCREEN_H;

    /* A child's position is given relative to its parent's client area, and
     * everything on screen is in screen coordinates, so the conversion has to
     * happen here -- once -- or every control lands at the wrong place. */
    if (parent) {
        wwin *pp = win_of(parent);
        if (pp) { ax += pp->x + pp->cxo; ay += pp->y + pp->cyo; }
    }

    memset(p, 0, sizeof *p);
    p->used = 1;
    p->list = -1;
    p->sel = -1;
    p->enabled = (style & WS_DISABLED) ? 0 : 1;
    snprintf(p->cls, sizeof p->cls, "%s", cls);
    snprintf(p->text, sizeof p->text, "%s", text ? text : "");
    p->exstyle = exstyle;
    p->style   = style;
    p->x = ax; p->y = ay; p->w = aw; p->h = ah;
    /* No decorations here, so the client area is the window. A game that asked
     * for a 640x480 client through AdjustWindowRect gets 640x480 back. */
    p->cw = aw; p->ch = ah;
    p->parent = parent; p->menu = id; p->id = id;
    p->instance = inst; p->param = param;
    p->visible = (style & WS_VISIBLE) ? 1 : 0;
    p->hi = 100;

    p->ctl = ctl_of_class(cls);
    if (!p->ctl && parent && is_extra_class(cls)) p->ctl = CTL_PANE;
    /* A title bar takes room out of the client area, and that is only right
     * for a window whose inside we are also drawing: a dialog's template
     * positions its controls from the client origin, so a caption painted
     * over the client area buries the first row of them.
     *
     * A window the program draws itself gets no caption at all. Its content
     * is its own -- a game's back buffer, most of the time -- and taking
     * twenty-two pixels off it, or painting a bar over the top of it, would
     * be this layer inventing chrome nobody asked for and then charging the
     * program for it. */
    if (p->ctl && !(style & WS_CHILD) && (style & WS_CAPTION) == WS_CAPTION) {
        p->cyo = CAPTION_H;
        p->ch = ah - CAPTION_H > 1 ? ah - CAPTION_H : 1;
    }
    if (p->ctl == CTL_LISTBOX || p->ctl == CTL_COMBOBOX) {
        for (int i = 0; i < MAX_LISTS; i++) if (!g_list[i].used) {
            g_list[i].used = 1; g_list[i].n = 0; p->list = i; break;
        }
    }
    /* A class the program registered wins over a built-in of the same name:
     * a program that registers its own "BUTTON" means its own. */
    if (c && c->wndproc) { p->wndproc = c->wndproc; p->ctl = CTL_NONE; }
    else if (p->ctl) { p->wndproc = hproc_addr(p->ctl); }
    else p->wndproc = c ? c->wndproc : 0;

    uint64_t hwnd = hwnd_of(p);
    if (!parent) g_focus = hwnd;
    uint64_t proc = p->wndproc;
    int cw2 = p->cw, ch2 = p->ch;
    pthread_mutex_unlock(&g_lock);

    if (w->verbose) fprintf(stderr, "winrun: user32: window %#llx, class \"%s\", %dx%d at %d,%d\n",
                            (unsigned long long)hwnd, cls, aw, ah, ax, ay);

    /* WM_CREATE goes straight to the WndProc, not through the queue -- a
     * program is entitled to have run it before CreateWindowEx returns, and
     * plenty of them set things up there. A non-zero return aborts creation. */
    if (run_wm_create && proc && !is_hproc(proc)) {
        uint64_t args[4] = { hwnd, WM_CREATE, 0, 0 };
        int64_t r = (int64_t)w32_call_guest(w, proc, 4, args);
        if (w->exited) return 0;
        if (r == -1) {
            pthread_mutex_lock(&g_lock);
            p->used = 0;
            if (g_focus == hwnd) g_focus = 0;
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
    }
    pthread_mutex_lock(&g_lock);
    if (!parent) {
        push(hwnd, WM_SIZE, 0, xy_lp(cw2, ch2), 0, 0);
        push(hwnd, WM_ACTIVATEAPP, 1, 0, 0, 0);
        push(hwnd, WM_ACTIVATE, 1, 0, 0, 0);
        push(hwnd, WM_SETFOCUS, 0, 0, 0, 0);
    }
    pthread_mutex_unlock(&g_lock);
    if (p->visible) invalidate(hwnd);
    return hwnd;
}

void w32_destroy_window(w32 *w, uint64_t hwnd) {
    int ex = 0, ey = 0, ew = 0, eh = 0;
    int had = w32_window_area(hwnd, 1, &ex, &ey, &ew, &eh);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_win[i].used && (HW_BASE + (uint64_t)i * HW_STEP == hwnd || g_win[i].parent == hwnd)) {
            if (g_win[i].list >= 0) g_list[g_win[i].list].used = 0;
            g_win[i].used = 0;
        }
    if (g_focus == hwnd) g_focus = 0;
    pthread_mutex_unlock(&g_lock);
    /* The surface keeps whatever was last drawn on it, so a window that goes
     * away leaves its pixels behind unless they are painted over. */
    if (had) {
        uint64_t dc = w32_dc_for_window(w, 0, 1);
        if (dc) {
            w32_gdi_fill_rect(dc, ex, ey, ex + ew, ey + eh, sys_color(COLOR_BACKGROUND));
            w32_dc_release(dc);
        }
    }
    /* What was underneath has to be drawn again, and the only thing that
     * knows what that is now is the windows that are left. */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        pthread_mutex_lock(&g_lock);
        uint64_t h = (g_win[i].used && !g_win[i].parent) ? HW_BASE + (uint64_t)i * HW_STEP : 0;
        pthread_mutex_unlock(&g_lock);
        if (h) invalidate(h);
    }
    w32_desktop_damaged();
}

void w32_set_window_text(w32 *w, uint64_t hwnd, const char *s) {
    (void)w;
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p) snprintf(p->text, sizeof p->text, "%s", s ? s : "");
    pthread_mutex_unlock(&g_lock);
}
void w32_get_window_text(w32 *w, uint64_t hwnd, char *out, size_t n) {
    (void)w;
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    snprintf(out, n, "%s", p ? p->text : "");
    pthread_mutex_unlock(&g_lock);
}

static void create_window(w32 *w, int wide) {
    char cls[96], text[256];
    uint64_t cp = ARG(1), tp = ARG(2);
    if (!cp) { RET(0); return; }
    /* a class can be named or given as an ATOM in the low word */
    if (cp < 0x10000) snprintf(cls, sizeof cls, "#%llu", (unsigned long long)cp);
    else if (wide) w32_wtoa(w, cp, cls, sizeof cls);
    else snprintf(cls, sizeof cls, "%.95s", w32_str(w, cp));
    text[0] = 0;
    if (tp >= 0x10000) {
        if (wide) w32_wtoa(w, tp, text, sizeof text);
        else snprintf(text, sizeof text, "%.255s", w32_str(w, tp));
    }
    pthread_mutex_lock(&g_lock);
    if (!class_of(cls) && cp >= 0xC000 && cp < 0xC000 + MAX_CLASSES && g_cls[cp - 0xC000].used)
        snprintf(cls, sizeof cls, "%s", g_cls[cp - 0xC000].name);
    pthread_mutex_unlock(&g_lock);

    RET(w32_new_window(w, cls, text, (uint32_t)ARG(3), (uint32_t)ARG(0),
                       (int)(int32_t)(uint32_t)ARG(4), (int)(int32_t)(uint32_t)ARG(5),
                       (int)(int32_t)(uint32_t)ARG(6), (int)(int32_t)(uint32_t)ARG(7),
                       ARG(8), ARG(9), ARG(10), ARG(11), 1));
}
static void u_CreateWindowExA(w32 *w) { create_window(w, 0); }
static void u_CreateWindowExW(w32 *w) { create_window(w, 1); }

static void u_DestroyWindow(w32 *w) {
    uint64_t h = ARG(0);
    send_to(w, h, WM_DESTROY, 0, 0, 0);
    w32_destroy_window(w, h);
    RET(1);
}

static void u_ShowWindow(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    int was = p ? p->visible : 0;
    if (p) p->visible = ARG(1) != SW_HIDE;
    int now = p ? p->visible : 0;
    pthread_mutex_unlock(&g_lock);
    if (now) invalidate(ARG(0));
    else w32_desktop_damaged();
    RET((uint64_t)(uint32_t)was);
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
/* A client point in screen space and back. The offset is the caption bar
 * when there is one, and zero when there is not. */
/* Nothing here draws chrome around a program's own window, so the rectangle
 * a program needs for a given client area is that client area. */
static void adjust_rect(w32 *w, uint32_t style) { (void)w; (void)style; RET(1); }
static void u_AdjustWindowRect(w32 *w)   { adjust_rect(w, (uint32_t)ARG(1)); }
static void u_AdjustWindowRectEx(w32 *w) { adjust_rect(w, (uint32_t)ARG(1)); }

static void u_ClientToScreen(w32 *w) {
    uint64_t pt = ARG(1);
    if (!pt) { RET(0); return; }
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    int ox = p ? p->x + p->cxo : 0, oy = p ? p->y + p->cyo : 0;
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
    int ox = p ? p->x + p->cxo : 0, oy = p ? p->y + p->cyo : 0;
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
    w32_set_window_text(w, ARG(0), ARG(1) ? w32_str(w, ARG(1)) : "");
    invalidate(ARG(0));
    RET(1);
}
static void u_GetWindowTextA(w32 *w) {
    char buf[256];
    w32_get_window_text(w, ARG(0), buf, sizeof buf);
    uint64_t out = ARG(1);
    int cap = (int)(int32_t)(uint32_t)ARG(2);
    if (!out || cap <= 0) { RET(0); return; }
    int n = (int)strlen(buf);
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) w32_write(w, out + (unsigned)i, 1, (uint8_t)buf[i]);
    w32_write(w, out + (unsigned)n, 1, 0);
    RET((uint64_t)(uint32_t)n);
}

/* GetWindowLong / SetWindowLong. GWL_WNDPROC (-4) is the one that matters:
 * subclassing is how a launcher hooks a game's window. DWL_* are the dialog
 * equivalents, which sit in the same negative space. */
enum { GWL_WNDPROC = -4, GWL_HINSTANCE = -6, GWL_HWNDPARENT = -8,
       GWL_STYLE = -16, GWL_EXSTYLE = -20, GWL_USERDATA = -21,
       DWL_MSGRESULT = 0, DWL_DLGPROC = 4, DWL_USER = 8 };
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

/* DispatchMessage: call the window's procedure, whether it is the guest's
 * or one of ours. */
static void dispatch_message(w32 *w, int wide) {
    uint64_t m = ARG(0);
    if (!m) { RET(0); return; }
    int ps = (int)w32_ptrsize(w);
    uint64_t hwnd = w32_read(w, m + 0, ps);
    uint32_t msg = (uint32_t)w32_read(w, m + (w->is32 ? 4 : 8), 4);
    uint64_t wp  = w32_read(w, m + (w->is32 ? 8 : 16), ps);
    uint64_t lp  = w32_read(w, m + (w->is32 ? 12 : 24), ps);
    /* A message with no window goes to whatever is under the pointer for
     * mouse input and to the focus window otherwise, which is what makes a
     * click on a button reach the button. */
    if (!hwnd) {
        pthread_mutex_lock(&g_lock);
        hwnd = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)
               ? window_at((int)(int16_t)(lp & 0xFFFF), (int)(int16_t)((lp >> 16) & 0xFFFF))
               : g_focus;
        pthread_mutex_unlock(&g_lock);
    }
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
        int wx = 0, wy = 0, ww = 0, wh = 0;
        if (w32_window_area(hwnd, 0, &wx, &wy, &ww, &wh))
            lp = xy_lp((int)(int16_t)(lp & 0xFFFF) - wx, (int)(int16_t)((lp >> 16) & 0xFFFF) - wy);
    }
    uint64_t r = deliver(w, hwnd, msg, wp, lp, wide);
    surface_present();
    RET(r);
}
static void u_DispatchMessageA(w32 *w) { dispatch_message(w, 0); }
static void u_DispatchMessageW(w32 *w) { dispatch_message(w, 1); }

/* SendMessage runs the WndProc now; PostMessage queues it. */
static void u_SendMessageA(w32 *w) { RET(send_to(w, ARG(0), (uint32_t)ARG(1), ARG(2), ARG(3), 0)); }
static void u_SendMessageW(w32 *w) { RET(send_to(w, ARG(0), (uint32_t)ARG(1), ARG(2), ARG(3), 1)); }
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
    case WM_PAINT: {
        /* Only for a window we draw. DefWindowProc on Windows paints nothing
         * -- it validates the region and returns -- and painting a grey
         * rectangle here instead would put one over the frame a game just
         * presented. */
        pthread_mutex_lock(&g_lock);
        wwin *p = win_of(hwnd);
        wwin snap;
        int have = p && p->ctl;
        if (have) snap = *p;
        pthread_mutex_unlock(&g_lock);
        if (have) paint_control(w, hwnd, &snap);
        RET(0); return;
    }
    case WM_SETTEXT: case WM_GETTEXT: case WM_GETTEXTLENGTH:
    case WM_SETFONT: case WM_GETFONT: case WM_ENABLE:
        RET(ctl_proc(w, hwnd, msg, ARG(2), ARG(3), 0)); return;
    default: RET(0); return;
    }
}
static void u_DefWindowProcW(w32 *w) { u_DefWindowProcA(w); }
static void u_CallWindowProcA(w32 *w) { RET(call_proc(w, ARG(0), ARG(1), (uint32_t)ARG(2), ARG(3), ARG(4), 0)); }
static void u_CallWindowProcW(w32 *w) { RET(call_proc(w, ARG(0), ARG(1), (uint32_t)ARG(2), ARG(3), ARG(4), 1)); }
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
/* For xinput.c, so a keyboard can stand in for a gamepad without a second
 * copy of this array. */
int w32_key_down(int vk) {
    if (vk < 0 || vk > 255) return 0;
    pthread_mutex_lock(&g_lock);
    int d = (g_keys[vk] & 0x80) != 0;
    pthread_mutex_unlock(&g_lock);
    return d;
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

static void u_wsprintfA(w32 *w) { w32_do_wsprintf(w, 0); }
static void u_wsprintfW(w32 *w) { w32_do_wsprintf(w, 1); }

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
    memcpy(W32P(w, p), "Winios", 7);   /* dmDeviceName: the display we claim to be */
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

/* --- the screen, and what is drawn on it ---------------------------------
 *
 * One surface, the size of the display, and a window's client area is a
 * rectangle in it. That is how Windows worked before the compositor and it
 * is the right model here: the app already has a path for a frame of pixels
 * (the one Direct3D presents through), so a painted dialog reaches the
 * screen with no new plumbing on the iOS side at all -- it is a frame like
 * any other.
 *
 * The surface persists between paints, so a window that has painted once
 * stays painted; only what is invalidated is drawn again. `g_surf_dirty`
 * says a frame is worth presenting, and the message pump flushes it, so a
 * pump iteration presents at most one frame however many controls painted.
 */
static uint32_t *g_surface;
static int g_surf_w, g_surf_h;
static int g_surf_dirty;

/* Caller must hold the lock, or be the guest thread before any window
 * exists. Returns NULL only if the allocation fails, which is fatal to
 * drawing but not to running. */
static uint32_t *surface_get(int *cx, int *cy) {
    if (!g_surface || g_surf_w != SCREEN_W || g_surf_h != SCREEN_H) {
        uint32_t *n = calloc((size_t)SCREEN_W * SCREEN_H, sizeof *n);
        if (!n) { if (cx) *cx = 0; if (cy) *cy = 0; return 0; }
        /* The desktop colour, opaque. Alpha 0 would upload as an invisible
         * frame, which looks exactly like "nothing drew" and is not; and
         * black would make an unpainted area indistinguishable from a hole
         * where a window failed to draw. */
        uint32_t d = sys_color(COLOR_BACKGROUND);
        uint32_t bg = 0xFF000000u | ((d & 0xFFu) << 16) | (d & 0xFF00u) | ((d >> 16) & 0xFFu);
        for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) n[i] = bg;
        free(g_surface);
        g_surface = n; g_surf_w = SCREEN_W; g_surf_h = SCREEN_H;
    }
    if (cx) *cx = g_surf_w;
    if (cy) *cy = g_surf_h;
    return g_surface;
}
uint32_t *w32_desktop_bits(int *cx, int *cy) {
    pthread_mutex_lock(&g_lock);
    uint32_t *b = surface_get(cx, cy);
    pthread_mutex_unlock(&g_lock);
    return b;
}
/* Called by gdi32 after it has drawn, and by anything that changes what the
 * screen should look like. */
void w32_desktop_damaged(void) { g_surf_dirty = 1; }

/* Hand the surface to whoever is showing frames. Nothing happens if a 3D
 * guest owns the display: it presents its own frames and this one is not
 * being drawn into. */
static void surface_present(void) {
    if (!g_surf_dirty) return;
    void *ctx = 0;
    w32_present_fn fn = w32_get_present(&ctx);
    pthread_mutex_lock(&g_lock);
    int cx = 0, cy = 0;
    uint32_t *b = surface_get(&cx, &cy);
    g_surf_dirty = 0;
    pthread_mutex_unlock(&g_lock);
    if (fn && b) fn(ctx, b, cx, cy, cx * 4);
}
/* For a test, and for the probe: the frame as it stands. */
const uint32_t *w32_desktop_peek(int *cx, int *cy) { return w32_desktop_bits(cx, cy); }

/* Where a window's client area sits on the screen. `whole` asks for the
 * window rectangle instead, which is the same thing here -- there are no
 * decorations to subtract. */
int w32_window_area(uint64_t hwnd, int whole, int *x, int *y, int *cx, int *cy) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    int ok = p != 0;
    if (ok) {
        *x = p->x + (whole ? 0 : p->cxo);
        *y = p->y + (whole ? 0 : p->cyo);
        *cx = whole ? p->w : p->cw;
        *cy = whole ? p->h : p->ch;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

/* --- the built-in controls ---------------------------------------------- */

static uint32_t sys_color(int i) {
    switch (i) {
    case COLOR_WINDOW: case COLOR_HIGHLIGHTTEXT: case COLOR_BTNHIGHLIGHT: return 0xFFFFFF;
    case COLOR_WINDOWTEXT: case COLOR_BTNTEXT: case COLOR_MENUTEXT: return 0x000000;
    case COLOR_BTNFACE: case COLOR_MENU: case COLOR_SCROLLBAR: return 0xF0F0F0;
    case COLOR_BTNSHADOW: case COLOR_GRAYTEXT: return 0x808080;
    case COLOR_3DDKSHADOW: case COLOR_WINDOWFRAME: return 0x404040;
    case COLOR_3DLIGHT: return 0xE3E3E3;
    case COLOR_HIGHLIGHT: return 0xD77800;          /* COLORREF: 0x00BBGGRR */
    case COLOR_ACTIVECAPTION: return 0x9B4D00;
    case COLOR_CAPTIONTEXT: return 0xFFFFFF;
    case COLOR_BACKGROUND: return 0x542B00;
    default: return 0xF0F0F0;
    }
}
uint32_t w32_sys_color(int i) { return sys_color(i); }

/* Which built-in a class name asks for. The names are the real ones, because
 * that is what a dialog template contains: a template that says "Button" has
 * to make a button. */
static ctlkind ctl_of_class(const char *cls) {
    if (!strcasecmp(cls, "button")) return CTL_BUTTON;
    if (!strcasecmp(cls, "static")) return CTL_STATIC;
    if (!strcasecmp(cls, "edit") || !strcasecmp(cls, "richedit") ||
        !strcasecmp(cls, "richedit20a") || !strcasecmp(cls, "richedit20w")) return CTL_EDIT;
    if (!strcasecmp(cls, "listbox")) return CTL_LISTBOX;
    if (!strcasecmp(cls, "combobox")) return CTL_COMBOBOX;
    if (!strcasecmp(cls, "msctls_progress32")) return CTL_PROGRESS;
    if (!strcasecmp(cls, "#32770") || !strcasecmp(cls, "dialog")) return CTL_DIALOG;
    /* The atoms a dialog template uses for the standard classes, in place of
     * a name: 0x0080 button, 0x0081 edit, 0x0082 static, 0x0083 list box,
     * 0x0084 scroll bar, 0x0085 combo box. */
    if (cls[0] == '#') {
        int a = atoi(cls + 1);
        switch (a) {
        case 0x0080: return CTL_BUTTON;
        case 0x0081: return CTL_EDIT;
        case 0x0082: return CTL_STATIC;
        case 0x0083: return CTL_LISTBOX;
        case 0x0085: return CTL_COMBOBOX;
        case 32770:  return CTL_DIALOG;
        default: break;
        }
    }
    return CTL_NONE;
}

/* Classes registered by InitCommonControls that we do not draw specially.
 * They still have to exist, or CreateWindowEx fails and the program's layout
 * collapses; a pane draws as a sunken rectangle, which is honest -- there is
 * a control there and it has no content. */
enum { MAX_EXTRA = 32 };
static char g_extra[MAX_EXTRA][32];
void w32_register_control_class(const char *name) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_EXTRA; i++) {
        if (!g_extra[i][0]) { snprintf(g_extra[i], sizeof g_extra[i], "%s", name); break; }
        if (!strcasecmp(g_extra[i], name)) break;
    }
    pthread_mutex_unlock(&g_lock);
}
static int is_extra_class(const char *cls) {
    for (int i = 0; i < MAX_EXTRA && g_extra[i][0]; i++) if (!strcasecmp(g_extra[i], cls)) return 1;
    return 0;
}

/* Draw a raised or sunken bevel: two light edges and two dark ones, which is
 * the whole of the Windows Classic look and reads correctly at any size. */
static void bevel(uint64_t hdc, int l, int t, int r, int b, int sunken) {
    uint32_t tl = sunken ? sys_color(COLOR_BTNSHADOW) : 0xFFFFFF;
    uint32_t br = sunken ? 0xFFFFFF : sys_color(COLOR_3DDKSHADOW);
    w32_gdi_fill_rect(hdc, l, t, r, t + 1, tl);
    w32_gdi_fill_rect(hdc, l, t, l + 1, b, tl);
    w32_gdi_fill_rect(hdc, l, b - 1, r, b, br);
    w32_gdi_fill_rect(hdc, r - 1, t, r, b, br);
}

/* Text inside a rectangle, with the three alignments a control uses and
 * wrapping on word boundaries when it does not fit. Returns the height used,
 * which is what DrawText with DT_CALCRECT reports. */
enum { DT_LEFT = 0, DT_CENTER = 1, DT_RIGHT = 2, DT_VCENTER = 4, DT_BOTTOM = 8,
       DT_WORDBREAK = 0x10, DT_SINGLELINE = 0x20, DT_NOCLIP = 0x100,
       DT_CALCRECT = 0x400, DT_NOPREFIX = 0x800, DT_END_ELLIPSIS = 0x8000 };

static int draw_text_rect(uint64_t hdc, const char *s, int len,
                          int l, int t, int r, int b, uint32_t fmt) {
    int lh = w32_gdi_line_height(hdc);
    int cw = 0, dummy = 0;
    w32_gdi_text_extent(hdc, 1, &cw, &dummy);
    if (cw < 1) cw = 1;
    int width = r - l;
    int per = width / cw;
    if (per < 1) per = 1;
    /* Measure first, so DT_VCENTER and DT_CALCRECT know the height before
     * anything is drawn. Two passes over a label is nothing, and getting the
     * height from a single pass means drawing in the wrong place first. */
    int lines = 0, i = 0;
    int starts[64], lens[64];
    while (i < len && lines < 64) {
        int take_n = len - i, hard = 0;
        for (int k = 0; k < take_n; k++) if (s[i + k] == '\n') { take_n = k; hard = 1; break; }
        if (!(fmt & DT_SINGLELINE) && (fmt & DT_WORDBREAK) && take_n > per) {
            int cut = per;
            while (cut > 0 && s[i + cut] != ' ') cut--;
            take_n = cut > 0 ? cut : per;
        } else if (take_n > per && !(fmt & DT_SINGLELINE) && !(fmt & DT_NOCLIP)) {
            take_n = per;
        }
        starts[lines] = i; lens[lines] = take_n;
        lines++;
        i += take_n + hard;
        if (fmt & DT_SINGLELINE) break;
        while (i < len && s[i] == ' ') i++;
    }
    if (!lines) { lines = 1; starts[0] = 0; lens[0] = 0; }
    int total = lines * lh;
    if (fmt & DT_CALCRECT) return total;

    int y = t;
    if (fmt & DT_VCENTER) y = t + ((b - t) - total) / 2;
    else if (fmt & DT_BOTTOM) y = b - total;
    if (y < t && !(fmt & DT_NOCLIP)) y = t;
    for (int k = 0; k < lines; k++) {
        int wpx = lens[k] * cw;
        int x = l;
        if (fmt & DT_CENTER) x = l + (width - wpx) / 2;
        else if (fmt & DT_RIGHT) x = r - wpx;
        w32_gdi_text_at(hdc, x, y + k * lh, s + starts[k], lens[k]);
    }
    return total;
}

/* The ampersand in a caption marks the access key and is not drawn. Windows
 * underlines the letter after it; we drop it, which is the same string a
 * program sees back from GetWindowText. */
static int strip_amp(const char *in, char *out, int cap) {
    int n = 0;
    for (int i = 0; in[i] && n + 1 < cap; i++) {
        if (in[i] == '&' && in[i + 1] == '&') { out[n++] = '&'; i++; continue; }
        if (in[i] == '&') continue;
        out[n++] = in[i];
    }
    out[n] = 0;
    return n;
}

/* ---- painting one control ---------------------------------------------- */

static void paint_control(w32 *w, uint64_t hwnd, wwin *snap) {
    int cw = snap->cw, ch = snap->ch;
    if (cw <= 0 || ch <= 0) return;
    uint64_t hdc = w32_dc_for_window(w, hwnd, 0);
    if (!hdc) return;
    if (snap->font) w32_gdi_set_font(hdc, snap->font);
    char label[256];
    int ln = strip_amp(snap->text, label, sizeof label);
    uint32_t fg = snap->enabled ? sys_color(COLOR_BTNTEXT) : sys_color(COLOR_GRAYTEXT);
    w32_gdi_set_bk_mode(hdc, 1);                 /* TRANSPARENT */
    w32_gdi_set_text_color(hdc, fg);

    switch (snap->ctl) {
    case CTL_DIALOG:
        w32_gdi_fill_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_BTNFACE));
        if (snap->cyo) {
            /* The title bar, on a device context over the whole window --
             * the client one cannot reach above its own origin, which is
             * where the caption is. */
            uint64_t wdc = w32_dc_for_window(w, hwnd, 1);
            if (wdc) {
                if (snap->font) w32_gdi_set_font(wdc, snap->font);
                w32_gdi_fill_rect(wdc, 0, 0, snap->w, snap->cyo, sys_color(COLOR_ACTIVECAPTION));
                w32_gdi_set_bk_mode(wdc, 1);
                w32_gdi_set_text_color(wdc, sys_color(COLOR_CAPTIONTEXT));
                w32_gdi_text_at(wdc, 6, 4, snap->text, (int)strlen(snap->text));
                w32_gdi_frame_rect(wdc, 0, 0, snap->w, snap->h, sys_color(COLOR_3DDKSHADOW));
                w32_dc_release(wdc);
            }
        }
        break;

    case CTL_STATIC: {
        int kind = (int)(snap->style & SS_TYPEMASK);
        if (kind == SS_BLACKRECT) { w32_gdi_fill_rect(hdc, 0, 0, cw, ch, 0x000000); break; }
        if (kind == SS_GRAYRECT)  { w32_gdi_fill_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_BTNSHADOW)); break; }
        if (kind == SS_WHITERECT) { w32_gdi_fill_rect(hdc, 0, 0, cw, ch, 0xFFFFFF); break; }
        if (kind == SS_ETCHEDHORZ || (kind == SS_ETCHEDVERT && cw > ch)) {
            w32_gdi_fill_rect(hdc, 0, 0, cw, 1, sys_color(COLOR_BTNSHADOW));
            w32_gdi_fill_rect(hdc, 0, 1, cw, 2, 0xFFFFFF);
            break;
        }
        if (kind == SS_ETCHEDFRAME) { bevel(hdc, 0, 0, cw, ch, 1); break; }
        if (kind == SS_ICON) break;
        uint32_t fmt = (uint32_t)(kind == SS_CENTER ? DT_CENTER : kind == SS_RIGHT ? DT_RIGHT : DT_LEFT);
        draw_text_rect(hdc, label, ln, 0, 0, cw, ch, fmt | DT_WORDBREAK);
        break;
    }

    case CTL_BUTTON: {
        int kind = (int)(snap->style & BS_TYPEMASK);
        if (kind == BS_GROUPBOX) {
            /* A group box is a frame with a gap where its label goes. */
            int top = 6;
            w32_gdi_frame_rect(hdc, 0, top, cw, ch, sys_color(COLOR_BTNSHADOW));
            if (ln) {
                int tw = 0, th = 0;
                w32_gdi_text_extent(hdc, ln, &tw, &th);
                w32_gdi_fill_rect(hdc, 6, top - 1, 6 + tw + 6, top + 1, sys_color(COLOR_BTNFACE));
                w32_gdi_text_at(hdc, 9, 0, label, ln);
            }
            break;
        }
        if (kind == BS_CHECKBOX || kind == BS_AUTOCHECKBOX ||
            kind == BS_3STATE || kind == BS_AUTO3STATE ||
            kind == BS_RADIOBUTTON || kind == BS_AUTORADIOBUTTON) {
            int box = ch < 16 ? ch : 13;
            int by = (ch - box) / 2;
            w32_gdi_fill_rect(hdc, 0, by, box, by + box, 0xFFFFFF);
            bevel(hdc, 0, by, box, by + box, 1);
            if (snap->checked) {
                /* A tick for a check box, a dot for a radio button: the two
                 * are different controls and have to look different, because
                 * that is how a user knows one choice from many. */
                uint32_t mark = sys_color(COLOR_BTNTEXT);
                if (kind == BS_RADIOBUTTON || kind == BS_AUTORADIOBUTTON)
                    w32_gdi_fill_rect(hdc, 4, by + 4, box - 4, by + box - 4, mark);
                else {
                    w32_gdi_line(hdc, 3, by + box / 2, box / 2 - 1, by + box - 4, mark);
                    w32_gdi_line(hdc, box / 2 - 1, by + box - 4, box - 3, by + 3, mark);
                    w32_gdi_line(hdc, 3, by + box / 2 + 1, box / 2 - 1, by + box - 3, mark);
                    w32_gdi_line(hdc, box / 2 - 1, by + box - 3, box - 3, by + 4, mark);
                }
            }
            draw_text_rect(hdc, label, ln, box + 5, 0, cw, ch, DT_LEFT | DT_VCENTER | DT_WORDBREAK);
            break;
        }
        /* A push button. */
        w32_gdi_fill_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_BTNFACE));
        bevel(hdc, 0, 0, cw, ch, snap->pressed);
        if (kind == BS_DEFPUSHBUTTON) w32_gdi_frame_rect(hdc, 0, 0, cw, ch, 0x000000);
        int off = snap->pressed ? 1 : 0;
        draw_text_rect(hdc, label, ln, off, off, cw + off, ch + off,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }

    case CTL_EDIT:
        w32_gdi_fill_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_WINDOW));
        bevel(hdc, 0, 0, cw, ch, 1);
        w32_gdi_set_text_color(hdc, snap->enabled ? sys_color(COLOR_WINDOWTEXT) : sys_color(COLOR_GRAYTEXT));
        if (snap->style & ES_PASSWORD) {
            char stars[64];
            int n = (int)strlen(snap->text); if (n > 63) n = 63;
            memset(stars, '*', (size_t)n); stars[n] = 0;
            draw_text_rect(hdc, stars, n, 3, 2, cw - 3, ch - 2,
                           (snap->style & ES_MULTILINE) ? DT_WORDBREAK : DT_SINGLELINE | DT_VCENTER);
        } else {
            draw_text_rect(hdc, snap->text, (int)strlen(snap->text), 3, 2, cw - 3, ch - 2,
                           (snap->style & ES_MULTILINE) ? DT_WORDBREAK : DT_SINGLELINE | DT_VCENTER);
        }
        break;

    case CTL_LISTBOX: case CTL_COMBOBOX: {
        w32_gdi_fill_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_WINDOW));
        bevel(hdc, 0, 0, cw, ch, 1);
        int lh = w32_gdi_line_height(hdc);
        if (lh < 1) lh = 13;
        if (snap->list >= 0 && snap->list < MAX_LISTS) {
            int n = g_list[snap->list].n;
            int rows = (ch - 4) / lh;
            int first = 0;
            if (snap->sel >= rows) first = snap->sel - rows + 1;
            for (int k = 0; k < rows && first + k < n; k++) {
                int idx = first + k, y = 2 + k * lh;
                if (idx == snap->sel) {
                    w32_gdi_fill_rect(hdc, 2, y, cw - 2, y + lh, sys_color(COLOR_HIGHLIGHT));
                    w32_gdi_set_text_color(hdc, sys_color(COLOR_HIGHLIGHTTEXT));
                } else {
                    w32_gdi_set_text_color(hdc, sys_color(COLOR_WINDOWTEXT));
                }
                const char *it = g_list[snap->list].item[idx];
                w32_gdi_text_at(hdc, 4, y, it, (int)strlen(it));
            }
        }
        if (snap->ctl == CTL_COMBOBOX) {
            /* The drop-down button, so it does not look like an edit field. */
            int bx = cw - 17;
            w32_gdi_fill_rect(hdc, bx, 2, cw - 2, ch - 2, sys_color(COLOR_BTNFACE));
            bevel(hdc, bx, 2, cw - 2, ch - 2, 0);
            int mx = bx + 8, my = ch / 2 - 1;
            for (int k = 0; k < 4; k++)
                w32_gdi_fill_rect(hdc, mx - 4 + k, my + k, mx + 4 - k, my + k + 1, 0x000000);
        }
        break;
    }

    case CTL_PROGRESS: {
        w32_gdi_fill_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_WINDOW));
        bevel(hdc, 0, 0, cw, ch, 1);
        int span = snap->hi - snap->lo;
        int done = span > 0 ? (snap->pos - snap->lo) : 0;
        if (done < 0) done = 0;
        if (span > 0 && done > span) done = span;
        int fillw = span > 0 ? (cw - 4) * done / span : 0;
        if (fillw > 0) w32_gdi_fill_rect(hdc, 2, 2, 2 + fillw, ch - 2, 0x00A000);
        break;
    }

    case CTL_PANE:
        w32_gdi_fill_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_WINDOW));
        bevel(hdc, 0, 0, cw, ch, 1);
        break;

    default:
        break;
    }
    if ((snap->style & WS_BORDER) && snap->ctl != CTL_EDIT && snap->ctl != CTL_LISTBOX)
        w32_gdi_frame_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_WINDOWFRAME));
    w32_dc_release(hdc);
    w32_desktop_damaged();
}

/* ---- the built-in window procedure -------------------------------------- */

static void post_command(uint64_t parent, uint64_t id, int code, uint64_t child);

/* Read a string argument that may be A or W. Control messages carry text as
 * whichever the window was created as, and a dialog template is Unicode, so
 * both arrive. */
static void get_msg_text(w32 *w, uint64_t p, int wide, char *out, size_t n) {
    if (!p) { out[0] = 0; return; }
    if (wide) w32_wtoa(w, p, out, n);
    else snprintf(out, n, "%.*s", (int)n - 1, w32_str(w, p));
}

static uint64_t ctl_proc(w32 *w, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int wide) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (!p) { pthread_mutex_unlock(&g_lock); return 0; }
    wwin snap = *p;
    pthread_mutex_unlock(&g_lock);

    switch (msg) {
    case WM_PAINT:
        paint_control(w, hwnd, &snap);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SETTEXT: {
        char buf[256];
        get_msg_text(w, lp, wide, buf, sizeof buf);
        pthread_mutex_lock(&g_lock);
        if ((p = win_of(hwnd))) snprintf(p->text, sizeof p->text, "%s", buf);
        pthread_mutex_unlock(&g_lock);
        invalidate(hwnd);
        return 1;
    }
    case WM_GETTEXTLENGTH:
        return strlen(snap.text);
    case WM_GETTEXT: {
        uint64_t cap = wp, out = lp;
        if (!out || !cap) return 0;
        size_t n = strlen(snap.text);
        if (n > cap - 1) n = (size_t)cap - 1;
        if (wide) {
            for (size_t i = 0; i < n; i++) w32_write(w, out + i * 2, 2, (uint8_t)snap.text[i]);
            w32_write(w, out + n * 2, 2, 0);
        } else {
            for (size_t i = 0; i < n; i++) w32_write(w, out + i, 1, (uint8_t)snap.text[i]);
            w32_write(w, out + n, 1, 0);
        }
        return n;
    }
    case WM_SETFONT:
        pthread_mutex_lock(&g_lock);
        if ((p = win_of(hwnd))) p->font = wp;
        pthread_mutex_unlock(&g_lock);
        if (lp) invalidate(hwnd);
        return 0;
    case WM_GETFONT:
        return snap.font;
    case WM_ENABLE:
        pthread_mutex_lock(&g_lock);
        if ((p = win_of(hwnd))) p->enabled = wp ? 1 : 0;
        pthread_mutex_unlock(&g_lock);
        invalidate(hwnd);
        return 0;

    case WM_LBUTTONDOWN:
        if (snap.ctl == CTL_BUTTON && snap.enabled) {
            pthread_mutex_lock(&g_lock);
            if ((p = win_of(hwnd))) p->pressed = 1;
            g_focus = hwnd;
            pthread_mutex_unlock(&g_lock);
            invalidate(hwnd);
        } else if ((snap.ctl == CTL_LISTBOX || snap.ctl == CTL_COMBOBOX) && snap.enabled) {
            int lh = 13, y = (int)(int16_t)((lp >> 16) & 0xFFFF);
            pthread_mutex_lock(&g_lock);
            if ((p = win_of(hwnd)) && p->list >= 0) {
                int idx = (y - 2) / (lh > 0 ? lh : 13);
                if (idx >= 0 && idx < g_list[p->list].n) p->sel = idx;
            }
            g_focus = hwnd;
            pthread_mutex_unlock(&g_lock);
            invalidate(hwnd);
            post_command(snap.parent, snap.id, LBN_SELCHANGE, hwnd);
        } else if (snap.ctl == CTL_EDIT && snap.enabled) {
            pthread_mutex_lock(&g_lock); g_focus = hwnd; pthread_mutex_unlock(&g_lock);
        }
        return 0;

    case WM_LBUTTONUP:
        if (snap.ctl == CTL_BUTTON && snap.enabled) {
            int was = snap.pressed;
            int kind = (int)(snap.style & BS_TYPEMASK);
            pthread_mutex_lock(&g_lock);
            if ((p = win_of(hwnd))) {
                p->pressed = 0;
                if (was) {
                    if (kind == BS_AUTOCHECKBOX || kind == BS_CHECKBOX ||
                        kind == BS_AUTO3STATE || kind == BS_3STATE)
                        p->checked = !p->checked;
                    else if (kind == BS_AUTORADIOBUTTON || kind == BS_RADIOBUTTON) {
                        /* One of a group: clearing the siblings is what makes
                         * a radio button a radio button, and the program does
                         * not do it -- the control does. */
                        for (int i = 0; i < MAX_WINDOWS; i++) {
                            wwin *q = &g_win[i];
                            if (q->used && q != p && q->parent == p->parent && q->ctl == CTL_BUTTON) {
                                int k = (int)(q->style & BS_TYPEMASK);
                                if (k == BS_AUTORADIOBUTTON || k == BS_RADIOBUTTON) q->checked = 0;
                            }
                        }
                        p->checked = 1;
                    }
                }
            }
            pthread_mutex_unlock(&g_lock);
            invalidate(snap.parent ? snap.parent : hwnd);
            if (was) post_command(snap.parent, snap.id, BN_CLICKED, hwnd);
        }
        return 0;

    case BM_GETCHECK: return (uint64_t)snap.checked;
    case BM_SETCHECK:
        pthread_mutex_lock(&g_lock);
        if ((p = win_of(hwnd))) p->checked = wp ? 1 : 0;
        pthread_mutex_unlock(&g_lock);
        invalidate(hwnd);
        return 0;
    case BM_GETSTATE: return (uint64_t)(snap.pressed ? 4 : 0) | (uint64_t)(snap.checked ? 1 : 0);
    case BM_SETSTATE:
        pthread_mutex_lock(&g_lock);
        if ((p = win_of(hwnd))) p->pressed = wp ? 1 : 0;
        pthread_mutex_unlock(&g_lock);
        invalidate(hwnd);
        return 0;
    case BM_CLICK:
        ctl_proc(w, hwnd, WM_LBUTTONDOWN, 1, 0, wide);
        ctl_proc(w, hwnd, WM_LBUTTONUP, 0, 0, wide);
        return 0;

    case WM_CHAR:
        if (snap.ctl == CTL_EDIT && snap.enabled && !(snap.style & ES_READONLY)) {
            pthread_mutex_lock(&g_lock);
            if ((p = win_of(hwnd))) {
                size_t n = strlen(p->text);
                if (wp == 8) { if (n) p->text[n - 1] = 0; }
                else if (wp >= 32 && wp < 127 && n + 1 < sizeof p->text) {
                    p->text[n] = (char)wp; p->text[n + 1] = 0;
                }
            }
            pthread_mutex_unlock(&g_lock);
            invalidate(hwnd);
            post_command(snap.parent, snap.id, EN_CHANGE, hwnd);
        }
        return 0;

    case EM_SETSEL: case EM_LIMITTEXT: case EM_SETMODIFY: return 0;
    case EM_GETMODIFY: return 0;
    case EM_SETREADONLY:
        pthread_mutex_lock(&g_lock);
        if ((p = win_of(hwnd))) p->style = wp ? (p->style | ES_READONLY) : (p->style & ~(uint32_t)ES_READONLY);
        pthread_mutex_unlock(&g_lock);
        return 1;

    case LB_ADDSTRING: case CB_ADDSTRING: {
        char buf[LIST_TEXT];
        get_msg_text(w, lp, wide, buf, sizeof buf);
        int idx = -1;
        pthread_mutex_lock(&g_lock);
        if ((p = win_of(hwnd)) && p->list >= 0 && g_list[p->list].n < LIST_ITEMS) {
            idx = g_list[p->list].n++;
            snprintf(g_list[p->list].item[idx], LIST_TEXT, "%s", buf);
        }
        pthread_mutex_unlock(&g_lock);
        invalidate(hwnd);
        return (uint64_t)(uint32_t)(idx < 0 ? -2 : idx);      /* LB_ERRSPACE */
    }
    case LB_RESETCONTENT: case CB_RESETCONTENT:
        pthread_mutex_lock(&g_lock);
        if ((p = win_of(hwnd)) && p->list >= 0) { g_list[p->list].n = 0; p->sel = -1; }
        pthread_mutex_unlock(&g_lock);
        invalidate(hwnd);
        return 0;
    case LB_GETCOUNT: case CB_GETCOUNT:
        return (uint64_t)(snap.list >= 0 ? g_list[snap.list].n : 0);
    case LB_GETCURSEL: case CB_GETCURSEL:
        return (uint64_t)(uint32_t)(snap.sel >= 0 ? snap.sel : -1);
    case LB_SETCURSEL: case CB_SETCURSEL:
        pthread_mutex_lock(&g_lock);
        if ((p = win_of(hwnd))) p->sel = (int)(int32_t)(uint32_t)wp;
        pthread_mutex_unlock(&g_lock);
        invalidate(hwnd);
        return wp;
    case LB_GETTEXT: case CB_GETLBTEXT: {
        int i = (int)(int32_t)(uint32_t)wp;
        if (snap.list < 0 || i < 0 || i >= g_list[snap.list].n || !lp) return (uint64_t)(uint32_t)-1;
        const char *it = g_list[snap.list].item[i];
        size_t n = strlen(it);
        if (wide) { for (size_t k = 0; k <= n; k++) w32_write(w, lp + k * 2, 2, (uint8_t)it[k]); }
        else      { for (size_t k = 0; k <= n; k++) w32_write(w, lp + k, 1, (uint8_t)it[k]); }
        return n;
    }
    case LB_GETTEXTLEN: {
        int i = (int)(int32_t)(uint32_t)wp;
        if (snap.list < 0 || i < 0 || i >= g_list[snap.list].n) return (uint64_t)(uint32_t)-1;
        return strlen(g_list[snap.list].item[i]);
    }

    case PBM_SETRANGE:
        pthread_mutex_lock(&g_lock);
        if ((p = win_of(hwnd))) { p->lo = (int)(lp & 0xFFFF); p->hi = (int)((lp >> 16) & 0xFFFF); }
        pthread_mutex_unlock(&g_lock);
        invalidate(hwnd);
        return 0;
    case PBM_SETRANGE32:
        pthread_mutex_lock(&g_lock);
        if ((p = win_of(hwnd))) { p->lo = (int)(int32_t)(uint32_t)wp; p->hi = (int)(int32_t)(uint32_t)lp; }
        pthread_mutex_unlock(&g_lock);
        invalidate(hwnd);
        return 0;
    case PBM_SETPOS: {
        int old = snap.pos;
        pthread_mutex_lock(&g_lock);
        if ((p = win_of(hwnd))) p->pos = (int)(int32_t)(uint32_t)wp;
        pthread_mutex_unlock(&g_lock);
        invalidate(hwnd);
        return (uint64_t)(uint32_t)old;
    }
    case PBM_DELTAPOS: {
        int old = snap.pos;
        pthread_mutex_lock(&g_lock);
        if ((p = win_of(hwnd))) p->pos += (int)(int32_t)(uint32_t)wp;
        pthread_mutex_unlock(&g_lock);
        invalidate(hwnd);
        return (uint64_t)(uint32_t)old;
    }
    case PBM_SETSTEP: {
        int old = snap.step;
        pthread_mutex_lock(&g_lock);
        if ((p = win_of(hwnd))) p->step = (int)(int32_t)(uint32_t)wp;
        pthread_mutex_unlock(&g_lock);
        return (uint64_t)(uint32_t)old;
    }
    case PBM_STEPIT: {
        int old = snap.pos;
        pthread_mutex_lock(&g_lock);
        if ((p = win_of(hwnd))) {
            p->pos += p->step ? p->step : 10;
            if (p->hi > p->lo && p->pos > p->hi) p->pos = p->lo;   /* it wraps, as it does */
        }
        pthread_mutex_unlock(&g_lock);
        invalidate(hwnd);
        return (uint64_t)(uint32_t)old;
    }
    case PBM_GETPOS: return (uint64_t)(uint32_t)snap.pos;
    case PBM_SETMARQUEE: return 1;

    default:
        break;
    }
    return 0;
}

/* ---- calling a window procedure ---------------------------------------- */

/* A window's procedure is either guest code or one of ours. Ours live at
 * pseudo-addresses so that everything which treats a WNDPROC as a value --
 * SetWindowLong(GWL_WNDPROC), CallWindowProc, a subclass chain -- keeps
 * working across the boundary in both directions. */
static uint64_t hproc_addr(ctlkind k) { return HPROC_BASE + (uint64_t)k * HPROC_STEP; }
static int is_hproc(uint64_t a) { return a >= HPROC_BASE && a < HPROC_BASE + (uint64_t)CTL_N * HPROC_STEP; }

static uint64_t dlg_default_proc(w32 *w, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int wide);

/* `wide` says whether a string in this message is UTF-16 or bytes. It comes
 * from which entry point the guest called -- SetDlgItemTextA hands us bytes
 * and SetDlgItemTextW hands us characters, and a control that guesses gets
 * either an empty string or mojibake. It is threaded through rather than
 * stored on the window because it is a property of the call, not of the
 * window: a program may use both on the same control, and plenty do. */
static uint64_t call_proc(w32 *w, uint64_t proc, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int wide) {
    if (!proc) return 0;
    if (is_hproc(proc)) {
        pthread_mutex_lock(&g_lock);
        wwin *p = win_of(hwnd);
        int dlg = p && p->is_dialog;
        pthread_mutex_unlock(&g_lock);
        if (dlg) return dlg_default_proc(w, hwnd, msg, wp, lp, wide);
        return ctl_proc(w, hwnd, msg, wp, lp, wide);
    }
    uint64_t args[4] = { hwnd, msg, wp, lp };
    return w32_call_guest(w, proc, 4, args);
}

/* Send a message to a window now, whoever owns its procedure. */
static uint64_t send_to(w32 *w, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int wide) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    uint64_t proc = p ? p->wndproc : 0;
    pthread_mutex_unlock(&g_lock);
    return call_proc(w, proc, hwnd, msg, wp, lp, wide);
}

/* The same, but with the default handling a dialog procedure relies on. A
 * DLGPROC returns FALSE to mean "I did not handle this", and the caller is
 * then obliged to do the default thing -- paint the background, close on OK.
 * Every path that delivers a message to a dialog has to honour that, not
 * only the modal loop, or a dialog whose procedure ignores WM_PAINT never
 * gets a background and appears as a hole. */
static uint64_t deliver(w32 *w, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int wide) {
    uint64_t r = send_to(w, hwnd, msg, wp, lp, wide);
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    int is_dlg = p && p->is_dialog;
    pthread_mutex_unlock(&g_lock);
    if (is_dlg && !r) return dlg_default_proc(w, hwnd, msg, wp, lp, wide);
    return r;
}
uint64_t w32_window_defproc(w32 *w, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    uint64_t old = p ? p->longs[GWL_SLOTS - 1] : 0;
    ctlkind k = p ? p->ctl : CTL_NONE;
    pthread_mutex_unlock(&g_lock);
    if (old) return call_proc(w, old, hwnd, msg, wp, lp, 1);
    if (k) return ctl_proc(w, hwnd, msg, wp, lp, 1);
    return 0;
}
/* comctl32's SetWindowSubclass: the new procedure runs first and the one it
 * displaced becomes what DefSubclassProc chains to. */
void w32_window_subclass(w32 *w, uint64_t hwnd, uint64_t proc, uint64_t ref) {
    (void)w;
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p) { p->longs[GWL_SLOTS - 1] = p->wndproc; p->wndproc = proc; p->subclass_ref = ref; }
    pthread_mutex_unlock(&g_lock);
}
void w32_window_unsubclass(w32 *w, uint64_t hwnd, uint64_t proc) {
    (void)w;
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p && p->wndproc == proc) { p->wndproc = p->longs[GWL_SLOTS - 1]; p->longs[GWL_SLOTS - 1] = 0; }
    pthread_mutex_unlock(&g_lock);
}

/* WM_COMMAND to the parent: wParam is (code << 16) | id, lParam the control.
 * Posted rather than sent, so a dialog procedure that closes the dialog does
 * it from its own message loop rather than from inside a click. */
static void post_command(uint64_t parent, uint64_t id, int code, uint64_t child) {
    if (!parent) return;
    pthread_mutex_lock(&g_lock);
    push(parent, WM_COMMAND, ((uint64_t)(uint32_t)code << 16) | (id & 0xFFFF), child, g_mx, g_my);
    pthread_mutex_unlock(&g_lock);
}

/* Ask for a repaint. A window and then its children, because a control sits
 * on its parent's background and repainting the parent alone would erase it. */
static void invalidate_tree(uint64_t hwnd, int depth) {
    if (depth > 4) return;
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    int vis = p && p->visible;
    if (vis) push(hwnd, WM_PAINT, 0, 0, g_mx, g_my);
    pthread_mutex_unlock(&g_lock);
    if (!vis) return;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        pthread_mutex_lock(&g_lock);
        uint64_t child = (g_win[i].used && g_win[i].parent == hwnd && g_win[i].visible)
                         ? HW_BASE + (uint64_t)i * HW_STEP : 0;
        pthread_mutex_unlock(&g_lock);
        if (child) invalidate_tree(child, depth + 1);
    }
}
static void invalidate(uint64_t hwnd) { invalidate_tree(hwnd, 0); }

/* The window under a screen point: the last one created wins, which puts a
 * child above its parent and a later dialog above an earlier one. Caller
 * holds the lock. */
static uint64_t window_at(int x, int y) {
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        wwin *p = &g_win[i];
        if (!p->used || !p->visible || p->cw <= 0 || p->ch <= 0) continue;
        if (x >= p->x && x < p->x + p->w && y >= p->y && y < p->y + p->h)
            return HW_BASE + (uint64_t)i * HW_STEP;
    }
    return 0;
}

/* ---- painting, for real ------------------------------------------------- */

/* PAINTSTRUCT: hdc, fErase, rcPaint, fRestore, fIncUpdate, rgbReserved.
 * The rectangle is in client coordinates, and a control that honours it
 * draws only what changed -- which is why it has to be the client area and
 * not the screen. */
static void u_BeginPaint(w32 *w) {
    uint64_t hwnd = ARG(0), ps = ARG(1);
    int cw = 0, ch = 0, x = 0, y = 0;
    if (!w32_window_area(hwnd, 0, &x, &y, &cw, &ch)) { cw = SCREEN_W; ch = SCREEN_H; }
    uint64_t hdc = w32_dc_for_window(w, hwnd, 0);
    if (ps) {
        int ptr = (int)w32_ptrsize(w);
        memset(W32P(w, ps), 0, w->is32 ? 64 : 72);
        w32_write(w, ps, ptr, hdc);
        w32_write(w, ps + (unsigned)ptr, 4, 1);                 /* fErase */
        put_rect(w, ps + (unsigned)ptr + 4, 0, 0, cw, ch);
    }
    RET(hdc);
}
static void u_EndPaint(w32 *w) {
    if (ARG(1)) w32_dc_release(w32_read(w, ARG(1), (int)w32_ptrsize(w)));
    w32_desktop_damaged();
    RET(1);
}
static void u_GetDC(w32 *w)       { RET(w32_dc_for_window(w, ARG(0), 0)); }
static void u_GetWindowDC(w32 *w) { RET(w32_dc_for_window(w, ARG(0), 1)); }
static void u_GetDCEx(w32 *w)     { RET(w32_dc_for_window(w, ARG(0), 0)); }
static void u_ReleaseDC(w32 *w)   { w32_dc_release(ARG(1)); w32_desktop_damaged(); RET(1); }

static void u_InvalidateRect(w32 *w) {
    uint64_t hwnd = ARG(0);
    if (!hwnd) {
        /* A null window invalidates every top-level one, which is what a
         * program does when it has changed something global. */
        for (int i = 0; i < MAX_WINDOWS; i++) {
            pthread_mutex_lock(&g_lock);
            uint64_t h = (g_win[i].used && !g_win[i].parent) ? HW_BASE + (uint64_t)i * HW_STEP : 0;
            pthread_mutex_unlock(&g_lock);
            if (h) invalidate(h);
        }
    } else invalidate(hwnd);
    RET(1);
}
static void u_ValidateRect(w32 *w) { (void)w; RET(1); }
/* UpdateWindow paints now rather than queueing: a program calls it when it
 * wants the window on screen before it goes on to do something slow, which
 * for an installer is the whole point. */
static void u_UpdateWindow(w32 *w) {
    uint64_t hwnd = ARG(0);
    deliver(w, hwnd, WM_PAINT, 0, 0, 0);
    for (int i = 0; i < MAX_WINDOWS; i++) {
        pthread_mutex_lock(&g_lock);
        uint64_t child = (g_win[i].used && g_win[i].parent == hwnd && g_win[i].visible)
                         ? HW_BASE + (uint64_t)i * HW_STEP : 0;
        pthread_mutex_unlock(&g_lock);
        if (child) deliver(w, child, WM_PAINT, 0, 0, 0);
    }
    surface_present();
    RET(1);
}
static void u_RedrawWindow(w32 *w) { invalidate(ARG(0)); RET(1); }

/* FillRect(hdc, rect, brush): the brush may be a real one or a system colour
 * plus one, which is how a dialog procedure answers WM_CTLCOLOR. */
static void u_FillRect(w32 *w) {
    uint64_t hdc = ARG(0), rp = ARG(1), br = ARG(2);
    if (!hdc || !rp) { RET(0); return; }
    int l = (int)(int32_t)w32_read(w, rp, 4),     t = (int)(int32_t)w32_read(w, rp + 4, 4);
    int r = (int)(int32_t)w32_read(w, rp + 8, 4), b = (int)(int32_t)w32_read(w, rp + 12, 4);
    uint32_t color;
    if (br > 0 && br <= COLOR_NCOLORS + 1) color = sys_color((int)br - 1);
    else {
        int null_brush = 0;
        color = w32_gdi_brush_color(br, &null_brush);
        if (null_brush) { RET(1); return; }
    }
    w32_gdi_fill_rect(hdc, l, t, r, b, color);
    w32_desktop_damaged();
    RET(1);
}
static void u_FrameRect(w32 *w) {
    uint64_t hdc = ARG(0), rp = ARG(1);
    if (!hdc || !rp) { RET(0); return; }
    int l = (int)(int32_t)w32_read(w, rp, 4),     t = (int)(int32_t)w32_read(w, rp + 4, 4);
    int r = (int)(int32_t)w32_read(w, rp + 8, 4), b = (int)(int32_t)w32_read(w, rp + 12, 4);
    int null_brush = 0;
    uint32_t color = w32_gdi_brush_color(ARG(2), &null_brush);
    w32_gdi_frame_rect(hdc, l, t, r, b, color);
    w32_desktop_damaged();
    RET(1);
}
static void u_InvertRect(w32 *w) { (void)w; RET(1); }
static void u_DrawFocusRect(w32 *w) { (void)w; RET(1); }
/* DrawEdge covers the bevels a program draws itself: the two edge flags that
 * matter are RAISED (0x05) and SUNKEN (0x0A). */
static void u_DrawEdge(w32 *w) {
    uint64_t hdc = ARG(0), rp = ARG(1);
    if (!hdc || !rp) { RET(0); return; }
    int l = (int)(int32_t)w32_read(w, rp, 4),     t = (int)(int32_t)w32_read(w, rp + 4, 4);
    int r = (int)(int32_t)w32_read(w, rp + 8, 4), b = (int)(int32_t)w32_read(w, rp + 12, 4);
    bevel(hdc, l, t, r, b, ((uint32_t)ARG(2) & 0x0A) != 0);
    w32_desktop_damaged();
    RET(1);
}

static void draw_text_api(w32 *w, int wide) {
    uint64_t hdc = ARG(0), sp = ARG(1), rp = ARG(3);
    int len = (int)(int32_t)(uint32_t)ARG(2);
    uint32_t fmt = (uint32_t)ARG(4);
    if (!hdc || !rp) { RET(0); return; }
    char buf[1024];
    if (wide) w32_wtoa_n(w, sp, len, buf, sizeof buf);
    else {
        const char *s = sp ? w32_str(w, sp) : "";
        if (len < 0) snprintf(buf, sizeof buf, "%s", s);
        else snprintf(buf, sizeof buf, "%.*s", len < (int)sizeof buf - 1 ? len : (int)sizeof buf - 1, s);
    }
    char clean[1024];
    int n = (fmt & DT_NOPREFIX) ? (int)strlen(strcpy(clean, buf)) : strip_amp(buf, clean, sizeof clean);
    int l = (int)(int32_t)w32_read(w, rp, 4),     t = (int)(int32_t)w32_read(w, rp + 4, 4);
    int r = (int)(int32_t)w32_read(w, rp + 8, 4), b = (int)(int32_t)w32_read(w, rp + 12, 4);
    int h = draw_text_rect(hdc, clean, n, l, t, r, b, fmt);
    if (fmt & DT_CALCRECT) w32_write(w, rp + 12, 4, (uint64_t)(uint32_t)(t + h));
    w32_desktop_damaged();
    RET(h);
}
static void u_DrawTextA(w32 *w)   { draw_text_api(w, 0); }
static void u_DrawTextW(w32 *w)   { draw_text_api(w, 1); }
static void u_DrawTextExA(w32 *w) { draw_text_api(w, 0); }
static void u_DrawTextExW(w32 *w) { draw_text_api(w, 1); }

static void u_GetSysColor(w32 *w) { RET(sys_color((int)(int32_t)(uint32_t)ARG(0))); }
/* A system-colour brush has to be a real brush, because a program selects it
 * into a DC and fills with it. */
static void u_GetSysColorBrush(w32 *w) {
    static uint64_t cache[COLOR_NCOLORS + 1];
    int i = (int)(int32_t)(uint32_t)ARG(0);
    if (i < 0 || i > COLOR_NCOLORS) { RET(0); return; }
    if (!cache[i]) cache[i] = w32_make_solid_brush(sys_color(i));
    RET(cache[i]);
}

/* ---- dialogs ------------------------------------------------------------ */

/* A dialog is a window whose children come from a template rather than from
 * the program's own CreateWindowEx calls, and whose procedure returns TRUE
 * or FALSE rather than a result. Everything else about it is an ordinary
 * window, which is why almost nothing here is special-cased below this point.
 *
 * DefDlgProc is the part a program relies on without ever naming: IDOK ends
 * the dialog with IDOK, IDCANCEL with IDCANCEL, Escape is IDCANCEL, and the
 * background is the face colour. A dialog procedure that returns FALSE is
 * asking for exactly that. */
enum { IDOK = 1, IDCANCEL = 2, IDABORT = 3, IDYES = 6, IDNO = 7 };

static void end_dialog(uint64_t hwnd, int result) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p) { p->ending = 1; p->result = result; }
    pthread_mutex_unlock(&g_lock);
}

static uint64_t dlg_default_proc(w32 *w, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int wide) {
    switch (msg) {
    case WM_COMMAND: {
        int id = (int)(wp & 0xFFFF);
        if (id == IDOK || id == IDCANCEL) { end_dialog(hwnd, id); return 1; }
        return 0;
    }
    case WM_CLOSE:
        end_dialog(hwnd, IDCANCEL);
        return 1;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { end_dialog(hwnd, IDCANCEL); return 1; }
        return 0;
    default:
        return ctl_proc(w, hwnd, msg, wp, lp, wide);
    }
}

/* Dialog units. A template's coordinates are in units of a quarter of the
 * average character width and an eighth of the character height, so the
 * conversion depends on the font the dialog is actually drawn in -- get it
 * wrong and every control is the right shape in the wrong place. */
static void dlg_units(w32 *w, uint64_t hwnd, int *bx, int *by) {
    uint64_t hdc = w32_dc_for_window(w, hwnd, 0);
    int cx = 8, cy = 13;
    if (hdc) {
        /* In the dialog's *own* font. A device context starts with the
         * system font, and measuring in that while the controls draw in the
         * template's font puts every control in the wrong place by the ratio
         * between the two. */
        pthread_mutex_lock(&g_lock);
        wwin *p = win_of(hwnd);
        uint64_t f = p ? p->font : 0;
        pthread_mutex_unlock(&g_lock);
        if (f) w32_gdi_set_font(hdc, f);
        w32_gdi_text_extent(hdc, 1, &cx, &cy);
        w32_dc_release(hdc);
    }
    *bx = cx < 1 ? 8 : cx;
    *by = cy < 1 ? 13 : cy;
}
static int du_x(int v, int bx) { return v * bx / 4; }
static int du_y(int v, int by) { return v * by / 8; }

/* A dialog template is a packed structure with variable-length fields, so it
 * is walked with a cursor rather than cast. Two formats exist and both are in
 * use: the original DLGTEMPLATE, and DLGTEMPLATEEX, which a resource compiler
 * emits whenever the dialog uses anything added after Windows 95 -- so an
 * installer built this century is usually the second. They are told apart by
 * the first two words: 0x0001 followed by 0xFFFF can only be the extended
 * one, because in the old format those are the low and high halves of the
 * style and 0xFFFF0001 is not a style anything sets. */
typedef struct { w32 *w; uint64_t at, end; } tcur;

static uint32_t t32(tcur *c) {
    if (c->at + 4 > c->end) return 0;
    uint32_t v = (uint32_t)w32_read(c->w, c->at, 4); c->at += 4; return v;
}
static uint16_t t16(tcur *c) {
    if (c->at + 2 > c->end) return 0;
    uint16_t v = (uint16_t)w32_read(c->w, c->at, 2); c->at += 2; return v;
}
static void talign(tcur *c, uint64_t base) { c->at = base + ((c->at - base + 3) & ~(uint64_t)3); }

/* A name that may be absent (0x0000), an ordinal (0xFFFF then a word), or an
 * inline UTF-16 string. Returns 1 if it produced a name. */
static int tname(tcur *c, char *out, size_t n) {
    out[0] = 0;
    if (c->at + 2 > c->end) return 0;
    uint16_t first = (uint16_t)w32_read(c->w, c->at, 2);
    if (first == 0) { c->at += 2; return 0; }
    if (first == 0xFFFF) {
        c->at += 2;
        uint16_t ord = t16(c);
        snprintf(out, n, "#%u", ord);
        return 1;
    }
    size_t i = 0;
    while (c->at + 2 <= c->end) {
        uint16_t ch = (uint16_t)w32_read(c->w, c->at, 2);
        c->at += 2;
        if (!ch) break;
        if (i + 1 < n) out[i++] = ch < 128 ? (char)ch : '?';
    }
    out[i] = 0;
    return 1;
}

enum { DS_SETFONT = 0x40, DS_MODALFRAME = 0x80, DS_CENTER = 0x0800 };

/* Build the dialog and its controls. Returns the dialog window, or 0. */
static uint64_t build_dialog(w32 *w, uint64_t tmpl, uint32_t size, uint64_t parent,
                             uint64_t dlgproc, uint64_t param, uint64_t inst) {
    if (!tmpl) return 0;
    tcur c = { w, tmpl, tmpl + (size ? size : 0x10000) };
    int ext = (w32_read(w, tmpl, 2) == 1 && w32_read(w, tmpl + 2, 2) == 0xFFFF);

    uint32_t style, exstyle;
    int nitems, x, y, cx, cy;
    if (ext) {
        c.at += 4;                       /* dlgVer, signature */
        (void)t32(&c);                   /* helpID */
        exstyle = t32(&c);
        style   = t32(&c);
        nitems  = (int)t16(&c);
        x = (int16_t)t16(&c); y = (int16_t)t16(&c);
        cx = (int16_t)t16(&c); cy = (int16_t)t16(&c);
    } else {
        style   = t32(&c);
        exstyle = t32(&c);
        nitems  = (int)t16(&c);
        x = (int16_t)t16(&c); y = (int16_t)t16(&c);
        cx = (int16_t)t16(&c); cy = (int16_t)t16(&c);
    }
    char menu[64], cls[64], title[256];
    tname(&c, menu, sizeof menu);
    tname(&c, cls, sizeof cls);
    tname(&c, title, sizeof title);

    int pointsize = 8;
    char face[64] = "";
    if (style & DS_SETFONT) {
        pointsize = (int)t16(&c);
        if (ext) { (void)t16(&c); c.at += 2; }        /* weight, italic + charset */
        tname(&c, face, sizeof face);
    }
    if (nitems < 0 || nitems > 128) return 0;

    /* The template's coordinates are dialog units and the conversion needs a
     * font, so the dialog window is made first at a provisional size, its
     * font is set, and only then is the geometry worked out. */
    int guess_bx = 8, guess_by = 13;
    if (pointsize > 0 && pointsize < 72) { guess_bx = pointsize * 8 / 8; guess_by = pointsize * 13 / 8; }
    if (guess_bx < 4) guess_bx = 4;
    if (guess_by < 8) guess_by = 8;

    int pw = du_x(cx, guess_bx), ph = du_y(cy, guess_by);
    int px = du_x(x, guess_bx), py = du_y(y, guess_by);
    /* A template's position is relative to its owner, and a top-level dialog
     * with no owner is centred -- which is also the only sensible place for
     * it on a display we own entirely. */
    if (!parent || (style & DS_CENTER) || (px == 0 && py == 0)) {
        px = (SCREEN_W - pw) / 2;
        py = (SCREEN_H - ph) / 2;
        if (px < 0) px = 0;
        if (py < 0) py = 0;
    }
    int cap = ((style & WS_CAPTION) == WS_CAPTION) ? CAPTION_H : 0;
    uint64_t dlg = w32_new_window(w, "#32770", title, (style | WS_VISIBLE) & ~(uint32_t)WS_CHILD,
                                  exstyle, px, py, pw, ph + cap, parent, 0, inst, param, 0);
    if (!dlg) return 0;
    pthread_mutex_lock(&g_lock);
    wwin *dp = win_of(dlg);
    if (dp) {
        dp->is_dialog = 1;
        dp->dlgproc = dlgproc;
        dp->ctl = CTL_DIALOG;
        dp->wndproc = dlgproc ? dlgproc : hproc_addr(CTL_DIALOG);
        dp->longs[GWL_SLOTS - 1] = hproc_addr(CTL_DIALOG);
    }
    pthread_mutex_unlock(&g_lock);

    uint64_t font = pointsize > 0 ? w32_make_font(-pointsize * 96 / 72, face) : 0;
    if (font) send_to(w, dlg, WM_SETFONT, font, 0, 1);

    int bx, by;
    dlg_units(w, dlg, &bx, &by);
    /* Re-measure with the real font and resize, so the controls that follow
     * land inside the dialog rather than off its edge. */
    pw = du_x(cx, bx); ph = du_y(cy, by);
    pthread_mutex_lock(&g_lock);
    if ((dp = win_of(dlg))) {
        dp->cw = pw; dp->ch = ph;
        dp->w = pw; dp->h = ph + dp->cyo;
        dp->x = (SCREEN_W - dp->w) / 2;
        dp->y = (SCREEN_H - dp->h) / 2;
        if (dp->x < 0) dp->x = 0;
        if (dp->y < 0) dp->y = 0;
    }
    pthread_mutex_unlock(&g_lock);

    for (int i = 0; i < nitems && c.at < c.end; i++) {
        talign(&c, tmpl);
        uint32_t istyle, iex;
        int ix, iy, icx, icy;
        uint64_t id;
        if (ext) {
            (void)t32(&c);                       /* helpID */
            iex = t32(&c);
            istyle = t32(&c);
            ix = (int16_t)t16(&c); iy = (int16_t)t16(&c);
            icx = (int16_t)t16(&c); icy = (int16_t)t16(&c);
            id = t32(&c);
        } else {
            istyle = t32(&c);
            iex = t32(&c);
            ix = (int16_t)t16(&c); iy = (int16_t)t16(&c);
            icx = (int16_t)t16(&c); icy = (int16_t)t16(&c);
            id = t16(&c);
        }
        char icls[64], itext[256];
        tname(&c, icls, sizeof icls);
        tname(&c, itext, sizeof itext);
        uint16_t extra = t16(&c);
        c.at += extra;
        if (!icls[0]) continue;

        uint64_t h = w32_new_window(w, icls, itext, istyle, iex,
                                    du_x(ix, bx), du_y(iy, by), du_x(icx, bx), du_y(icy, by),
                                    dlg, id, inst, 0, 0);
        if (h && font) send_to(w, h, WM_SETFONT, font, 0, 1);
    }
    return dlg;
}

/* The dialog's own message loop. A modal dialog does not return to the
 * program's loop, so it needs one of its own -- and this is the loop the
 * person actually interacts with while an installer is on screen.
 *
 * It draws, it dispatches, and it stops when EndDialog has been called or
 * the guest has exited. `w32_dialog_pump` is what a host that owns the run
 * loop (the app) can call instead. */
static int run_modal(w32 *w, uint64_t dlg) {
    for (;;) {
        if (w->exited) return 0;
        pthread_mutex_lock(&g_lock);
        wwin *p = win_of(dlg);
        int done = !p || p->ending;
        int result = p ? p->result : 0;
        int quit = g_quit;
        pthread_mutex_unlock(&g_lock);
        if (done) return result;
        if (quit) return 0;

        qmsg q;
        int have = 0;
        pthread_mutex_lock(&g_lock);
        if (g_qhead != g_qtail) {
            q = g_q[g_qhead];
            g_qhead = (g_qhead + 1) % MAX_MSGS;
            have = 1;
        }
        pthread_mutex_unlock(&g_lock);
        if (!have) {
            surface_present();
            w32_host_idle();
            continue;
        }
        uint64_t target = q.hwnd;
        if (!target) {
            pthread_mutex_lock(&g_lock);
            target = (q.msg >= WM_MOUSEFIRST && q.msg <= WM_MOUSELAST)
                     ? window_at(q.x, q.y) : g_focus;
            if (!target) target = dlg;
            pthread_mutex_unlock(&g_lock);
        }
        /* Mouse coordinates reach a window in *its* client space. */
        uint64_t lp = q.lparam;
        if (q.msg >= WM_MOUSEFIRST && q.msg <= WM_MOUSELAST) {
            int wx = 0, wy = 0, ww = 0, wh = 0;
            if (w32_window_area(target, 0, &wx, &wy, &ww, &wh))
                lp = xy_lp(q.x - wx, q.y - wy);
        }
        /* A dialog procedure that returns FALSE has not handled the message,
         * and the default handling is what closes the dialog on OK. */
        pthread_mutex_lock(&g_lock);
        wwin *tp = win_of(target);
        uint64_t proc = tp ? tp->wndproc : 0;
        int is_dlg = tp && tp->is_dialog;
        pthread_mutex_unlock(&g_lock);
        uint64_t r = proc ? call_proc(w, proc, target, q.msg, q.wparam, lp, 1) : 0;
        if (w->exited) return 0;
        if (is_dlg && !r) dlg_default_proc(w, target, q.msg, q.wparam, lp, 1);
        (void)r;
    }
}

/* CreateDialogParam / DialogBoxParam, indirect and by resource name. */
static uint64_t dialog_from_template(w32 *w, uint64_t inst, uint64_t tmpl, uint32_t size,
                                     uint64_t parent, uint64_t proc, uint64_t param) {
    uint64_t dlg = build_dialog(w, tmpl, size, parent, proc, param, inst);
    if (!dlg) return 0;
    /* WM_INITDIALOG carries the parameter, and a program does most of its
     * setup there: filling a list, setting the text of a field. */
    deliver(w, dlg, WM_INITDIALOG, 0, param, 1);
    invalidate(dlg);
    return dlg;
}
static uint64_t dialog_by_name(w32 *w, uint64_t inst, uint64_t name, int wide,
                               uint64_t parent, uint64_t proc, uint64_t param) {
    uint32_t size = 0;
    uint64_t hr = w32_find_resource(w, inst, 5 /* RT_DIALOG */, name, wide);
    if (!hr) return 0;
    uint64_t data = w32_resource_data(w, hr, &size);
    if (!data) return 0;
    return dialog_from_template(w, inst, data, size, parent, proc, param);
}

static void u_CreateDialogParamA(w32 *w) { RET(dialog_by_name(w, ARG(0), ARG(1), 0, ARG(2), ARG(3), ARG(4))); }
static void u_CreateDialogParamW(w32 *w) { RET(dialog_by_name(w, ARG(0), ARG(1), 1, ARG(2), ARG(3), ARG(4))); }
static void u_CreateDialogIndirectParamA(w32 *w) { RET(dialog_from_template(w, ARG(0), ARG(1), 0, ARG(2), ARG(3), ARG(4))); }
static void u_CreateDialogIndirectParamW(w32 *w) { u_CreateDialogIndirectParamA(w); }

static void u_DialogBoxParamA(w32 *w) {
    uint64_t dlg = dialog_by_name(w, ARG(0), ARG(1), 0, ARG(2), ARG(3), ARG(4));
    if (!dlg) { RET((uint64_t)(uint32_t)-1); return; }
    int r = run_modal(w, dlg);
    w32_destroy_window(w, dlg);
    RET((uint64_t)(uint32_t)r);
}
static void u_DialogBoxParamW(w32 *w) {
    uint64_t dlg = dialog_by_name(w, ARG(0), ARG(1), 1, ARG(2), ARG(3), ARG(4));
    if (!dlg) { RET((uint64_t)(uint32_t)-1); return; }
    int r = run_modal(w, dlg);
    w32_destroy_window(w, dlg);
    RET((uint64_t)(uint32_t)r);
}
static void u_DialogBoxIndirectParamA(w32 *w) {
    uint64_t dlg = dialog_from_template(w, ARG(0), ARG(1), 0, ARG(2), ARG(3), ARG(4));
    if (!dlg) { RET((uint64_t)(uint32_t)-1); return; }
    int r = run_modal(w, dlg);
    w32_destroy_window(w, dlg);
    RET((uint64_t)(uint32_t)r);
}
static void u_DialogBoxIndirectParamW(w32 *w) { u_DialogBoxIndirectParamA(w); }

static void u_EndDialog(w32 *w) { end_dialog(ARG(0), (int)(int32_t)(uint32_t)ARG(1)); RET(1); }

static void u_GetDlgItem(w32 *w) {
    uint64_t dlg = ARG(0), id = ARG(1) & 0xFFFF;
    pthread_mutex_lock(&g_lock);
    uint64_t h = 0;
    for (int i = 0; i < MAX_WINDOWS && !h; i++)
        if (g_win[i].used && g_win[i].parent == dlg && (g_win[i].id & 0xFFFF) == id)
            h = HW_BASE + (uint64_t)i * HW_STEP;
    pthread_mutex_unlock(&g_lock);
    RET(h);
}
static void u_GetDlgCtrlID(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    uint64_t id = p ? p->id : 0;
    pthread_mutex_unlock(&g_lock);
    RET(id);
}
static uint64_t dlg_item(w32 *w, uint64_t dlg, uint64_t id) {
    pthread_mutex_lock(&g_lock);
    uint64_t h = 0;
    for (int i = 0; i < MAX_WINDOWS && !h; i++)
        if (g_win[i].used && g_win[i].parent == dlg && (g_win[i].id & 0xFFFF) == (id & 0xFFFF))
            h = HW_BASE + (uint64_t)i * HW_STEP;
    pthread_mutex_unlock(&g_lock);
    return h;
}
static void u_SendDlgItemMessageA(w32 *w) {
    uint64_t h = dlg_item(w, ARG(0), ARG(1));
    RET(h ? send_to(w, h, (uint32_t)ARG(2), ARG(3), ARG(4), 0) : 0);
}
static void u_SendDlgItemMessageW(w32 *w) {
    uint64_t h = dlg_item(w, ARG(0), ARG(1));
    RET(h ? send_to(w, h, (uint32_t)ARG(2), ARG(3), ARG(4), 1) : 0);
}
static void set_dlg_text(w32 *w, int wide) {
    uint64_t h = dlg_item(w, ARG(0), ARG(1));
    if (h) { send_to(w, h, WM_SETTEXT, 0, ARG(2), wide); invalidate(h); }
    RET(h != 0);
}
static void u_SetDlgItemTextA(w32 *w) { set_dlg_text(w, 0); }
static void u_SetDlgItemTextW(w32 *w) { set_dlg_text(w, 1); }
static void u_GetDlgItemTextA(w32 *w) {
    uint64_t h = dlg_item(w, ARG(0), ARG(1));
    RET(h ? send_to(w, h, WM_GETTEXT, ARG(3), ARG(2), 0) : 0);
}
static void u_GetDlgItemTextW(w32 *w) {
    uint64_t h = dlg_item(w, ARG(0), ARG(1));
    RET(h ? send_to(w, h, WM_GETTEXT, ARG(3), ARG(2), 1) : 0);
}
static void u_SetDlgItemInt(w32 *w) {
    uint64_t h = dlg_item(w, ARG(0), ARG(1));
    if (h) {
        char buf[32];
        if (ARG(3)) snprintf(buf, sizeof buf, "%d", (int)(int32_t)(uint32_t)ARG(2));
        else snprintf(buf, sizeof buf, "%u", (unsigned)ARG(2));
        w32_set_window_text(w, h, buf);
        invalidate(h);
    }
    RET(h != 0);
}
static void u_GetDlgItemInt(w32 *w) {
    uint64_t h = dlg_item(w, ARG(0), ARG(1));
    char buf[64] = "";
    if (h) w32_get_window_text(w, h, buf, sizeof buf);
    if (ARG(2)) w32_write(w, ARG(2), 4, h ? 1 : 0);
    RET((uint64_t)(uint32_t)atoi(buf));
}
static void u_CheckDlgButton(w32 *w) {
    uint64_t h = dlg_item(w, ARG(0), ARG(1));
    if (h) send_to(w, h, BM_SETCHECK, ARG(2), 0, 0);
    RET(h != 0);
}
static void u_IsDlgButtonChecked(w32 *w) {
    uint64_t h = dlg_item(w, ARG(0), ARG(1));
    RET(h ? send_to(w, h, BM_GETCHECK, 0, 0, 0) : 0);
}
static void u_CheckRadioButton(w32 *w) {
    uint64_t dlg = ARG(0);
    uint64_t first = ARG(1), last = ARG(2), pick = ARG(3);
    for (uint64_t id = first; id <= last && id - first < 64; id++) {
        uint64_t h = dlg_item(w, dlg, id);
        if (h) send_to(w, h, BM_SETCHECK, id == pick ? 1 : 0, 0, 0);
    }
    RET(1);
}
static void u_MapDialogRect(w32 *w) {
    uint64_t rp = ARG(1);
    if (!rp) { RET(0); return; }
    int bx, by;
    dlg_units(w, ARG(0), &bx, &by);
    int l = (int)(int32_t)w32_read(w, rp, 4),     t = (int)(int32_t)w32_read(w, rp + 4, 4);
    int r = (int)(int32_t)w32_read(w, rp + 8, 4), b = (int)(int32_t)w32_read(w, rp + 12, 4);
    w32_write(w, rp,      4, (uint64_t)(uint32_t)du_x(l, bx));
    w32_write(w, rp + 4,  4, (uint64_t)(uint32_t)du_y(t, by));
    w32_write(w, rp + 8,  4, (uint64_t)(uint32_t)du_x(r, bx));
    w32_write(w, rp + 12, 4, (uint64_t)(uint32_t)du_y(b, by));
    RET(1);
}
static void u_GetDialogBaseUnits(w32 *w) {
    int bx, by;
    dlg_units(w, 0, &bx, &by);
    RET(((uint64_t)(uint32_t)by << 16) | (uint32_t)bx);
}
static void u_EnableWindow(w32 *w) {
    uint64_t h = ARG(0);
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(h);
    int was = p ? !p->enabled : 0;
    if (p) p->enabled = ARG(1) ? 1 : 0;
    pthread_mutex_unlock(&g_lock);
    if (p) { send_to(w, h, WM_ENABLE, ARG(1), 0, 0); invalidate(h); }
    RET((uint64_t)(uint32_t)was);
}
static void u_IsWindowEnabled(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    int e = p ? p->enabled : 0;
    pthread_mutex_unlock(&g_lock);
    RET((uint64_t)(uint32_t)e);
}
static void u_GetNextDlgTabItem(w32 *w) { (void)w; RET(0); }
static void u_IsDialogMessageA(w32 *w) { (void)w; RET(0); }
static void u_IsDialogMessageW(w32 *w) { (void)w; RET(0); }
static void u_SetWindowTextW_real(w32 *w) {
    char buf[256];
    w32_wtoa(w, ARG(1), buf, sizeof buf);
    w32_set_window_text(w, ARG(0), buf);
    send_to(w, ARG(0), WM_SETTEXT, 0, ARG(1), 1);
    invalidate(ARG(0));
    RET(1);
}
static void u_GetWindowTextW(w32 *w) {
    char buf[256];
    w32_get_window_text(w, ARG(0), buf, sizeof buf);
    uint64_t out = ARG(1);
    int cap = (int)(int32_t)(uint32_t)ARG(2);
    if (!out || cap <= 0) { RET(0); return; }
    int n = (int)strlen(buf);
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) w32_write(w, out + (unsigned)i * 2, 2, (uint8_t)buf[i]);
    w32_write(w, out + (unsigned)n * 2, 2, 0);
    RET((uint64_t)(uint32_t)n);
}
static void u_GetWindowTextLengthA(w32 *w) {
    char buf[256];
    w32_get_window_text(w, ARG(0), buf, sizeof buf);
    RET(strlen(buf));
}
static void u_GetWindowTextLengthW(w32 *w) { u_GetWindowTextLengthA(w); }

/* LoadString: a program's own message table. An installer's every caption
 * comes out of here, so a stub returning nothing would draw an empty dialog
 * that looked like a bug in the drawing. */
static void u_LoadStringA(w32 *w) {
    char buf[512];
    int n = w32_load_string(w, ARG(0), (uint32_t)ARG(1), buf, sizeof buf);
    uint64_t out = ARG(2);
    int cap = (int)(int32_t)(uint32_t)ARG(3);
    if (!out || cap <= 0) { RET(0); return; }
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) w32_write(w, out + (unsigned)i, 1, (uint8_t)buf[i]);
    w32_write(w, out + (unsigned)n, 1, 0);
    RET((uint64_t)(uint32_t)n);
}
/* ---- the rest of what a wizard calls -------------------------------------
 *
 * These are the calls that turned up in an installer's import table and did
 * nothing until now. None is deep; the point of each is that the program
 * gets a defined answer instead of a stub, because a stub that returns zero
 * is indistinguishable from a real failure and sends the installer down its
 * error path.
 */

/* SendMessageTimeout is SendMessage with a deadline that cannot expire here:
 * there is no other thread to wait for, so the call has already returned by
 * the time a timeout could matter. */
static void send_timeout(w32 *w, int wide) {
    uint64_t r = send_to(w, ARG(0), (uint32_t)ARG(1), ARG(2), ARG(3), wide);
    if (ARG(6)) w32_write(w, ARG(6), (int)w32_ptrsize(w), r);
    RET(1);
}
static void u_SendMessageTimeoutA(w32 *w) { send_timeout(w, 0); }
static void u_SendMessageTimeoutW(w32 *w) { send_timeout(w, 1); }
static void u_SendNotifyMessageA(w32 *w) { RET(send_to(w, ARG(0), (uint32_t)ARG(1), ARG(2), ARG(3), 0)); }
static void u_SendNotifyMessageW(w32 *w) { RET(send_to(w, ARG(0), (uint32_t)ARG(1), ARG(2), ARG(3), 1)); }

/* Window properties: a named value hung off a window. A program uses them to
 * find its own state from inside a window procedure it did not write, which
 * is exactly what a subclassed control needs. */
enum { MAX_PROPS = 64 };
static struct { int used; uint64_t hwnd; char name[64]; uint64_t value; } g_prop[MAX_PROPS];

static void prop_name(w32 *w, uint64_t p, int wide, char *out, size_t n) {
    /* An atom rather than a string: below 0x10000 it is a number, and the
     * number is the name. */
    if (p < 0x10000) { snprintf(out, n, "#%llu", (unsigned long long)p); return; }
    if (wide) w32_wtoa(w, p, out, n);
    else snprintf(out, n, "%.*s", (int)n - 1, w32_str(w, p));
}
static void set_prop(w32 *w, int wide) {
    char name[64];
    prop_name(w, ARG(1), wide, name, sizeof name);
    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for (int i = 0; i < MAX_PROPS; i++) {
        if (g_prop[i].used && g_prop[i].hwnd == ARG(0) && !strcmp(g_prop[i].name, name)) { slot = i; break; }
        if (!g_prop[i].used && slot < 0) slot = i;
    }
    if (slot >= 0) {
        g_prop[slot].used = 1; g_prop[slot].hwnd = ARG(0);
        snprintf(g_prop[slot].name, sizeof g_prop[slot].name, "%s", name);
        g_prop[slot].value = ARG(2);
    }
    pthread_mutex_unlock(&g_lock);
    RET(slot >= 0);
}
static void get_prop(w32 *w, int wide, int remove) {
    char name[64];
    prop_name(w, ARG(1), wide, name, sizeof name);
    pthread_mutex_lock(&g_lock);
    uint64_t v = 0;
    for (int i = 0; i < MAX_PROPS; i++)
        if (g_prop[i].used && g_prop[i].hwnd == ARG(0) && !strcmp(g_prop[i].name, name)) {
            v = g_prop[i].value;
            if (remove) g_prop[i].used = 0;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    RET(v);
}
/* Everything the drawing side keeps between calls, cleared for a new run.
 * Called with the lock held. Defined here rather than up with the reset
 * because these tables are declared where they are used, and a reset that
 * forward-declares each of them would be a list to forget to add to. */
static void ui_reset_tables(void) {
    memset(g_prop, 0, sizeof g_prop);
    memset(g_extra, 0, sizeof g_extra);
    /* The screen too. Inside the app one process runs many programs, and a
     * new one starting on the last one's pixels would show a frame of the
     * previous game before it had drawn anything of its own. */
    free(g_surface);
    g_surface = 0; g_surf_w = g_surf_h = 0; g_surf_dirty = 0;
    w32_comctl32_reset();
}

static void u_SetPropA(w32 *w)    { set_prop(w, 0); }
static void u_SetPropW(w32 *w)    { set_prop(w, 1); }
static void u_GetPropA(w32 *w)    { get_prop(w, 0, 0); }
static void u_GetPropW(w32 *w)    { get_prop(w, 1, 0); }
static void u_RemovePropA(w32 *w) { get_prop(w, 0, 1); }
static void u_RemovePropW(w32 *w) { get_prop(w, 1, 1); }

/* Menus. Nothing draws one, and a dialog-based installer has none -- but it
 * asks for its system menu so it can grey out Close, and a call that fails
 * there can send it down an error path over a cosmetic detail. Handles are
 * distinct and the counts are honest: zero items, because there are none. */
enum { MENU_BASE = 0x000C0000u };
static uint64_t g_next_menu = MENU_BASE;
static void u_CreateMenu(w32 *w)      { (void)w; RET(g_next_menu += 4); }
static void u_CreatePopupMenu(w32 *w) { (void)w; RET(g_next_menu += 4); }
static void u_DestroyMenu(w32 *w)     { (void)w; RET(1); }
static void u_GetSystemMenu(w32 *w)   { (void)w; RET(ARG(1) ? 0 : MENU_BASE); }
static void u_GetMenu(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    uint64_t m = p ? p->menu : 0;
    pthread_mutex_unlock(&g_lock);
    RET(m);
}
static void u_SetMenu(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    if (p) p->menu = ARG(1);
    pthread_mutex_unlock(&g_lock);
    RET(1);
}
static void u_GetMenuItemCount(w32 *w) { (void)w; RET(0); }
static void u_EnableMenuItem(w32 *w)   { (void)w; RET(0); }
static void u_DeleteMenu(w32 *w)       { (void)w; RET(1); }
static void u_AppendMenuA(w32 *w)      { (void)w; RET(1); }
static void u_AppendMenuW(w32 *w)      { (void)w; RET(1); }
static void u_DrawMenuBar(w32 *w)      { (void)w; RET(1); }
/* TrackPopupMenu returns which item was chosen, and nothing was: there is no
 * menu on screen to choose from. Zero is "dismissed", which is what a person
 * pressing Escape produces and every caller handles. */
static void u_TrackPopupMenu(w32 *w)   { (void)w; RET(0); }
static void u_TrackPopupMenuEx(w32 *w) { (void)w; RET(0); }

/* Hooks. A hook that is installed but never called is not a lie as long as
 * nothing depends on it firing; what a program depends on is that the
 * install succeeds so it can uninstall it later without leaking. */
enum { HOOK_BASE = 0x000D0000u };
static uint64_t g_next_hook = HOOK_BASE;
static void u_SetWindowsHookExA(w32 *w) { (void)w; RET(g_next_hook += 4); }
static void u_SetWindowsHookExW(w32 *w) { (void)w; RET(g_next_hook += 4); }
static void u_UnhookWindowsHookEx(w32 *w) { (void)w; RET(1); }
static void u_CallNextHookEx(w32 *w) { (void)w; RET(0); }

static void u_GetClassNameA(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    char cls[96];
    snprintf(cls, sizeof cls, "%s", p ? p->cls : "");
    pthread_mutex_unlock(&g_lock);
    uint64_t out = ARG(1);
    int cap = (int)(int32_t)(uint32_t)ARG(2);
    if (!out || cap <= 0) { RET(0); return; }
    int n = (int)strlen(cls);
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) w32_write(w, out + (unsigned)i, 1, (uint8_t)cls[i]);
    w32_write(w, out + (unsigned)n, 1, 0);
    RET((uint64_t)(uint32_t)n);
}
static void u_GetClassNameW(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    char cls[96];
    snprintf(cls, sizeof cls, "%s", p ? p->cls : "");
    pthread_mutex_unlock(&g_lock);
    uint64_t out = ARG(1);
    int cap = (int)(int32_t)(uint32_t)ARG(2);
    if (!out || cap <= 0) { RET(0); return; }
    int n = (int)strlen(cls);
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) w32_write(w, out + (unsigned)i * 2, 2, (uint8_t)cls[i]);
    w32_write(w, out + (unsigned)n * 2, 2, 0);
    RET((uint64_t)(uint32_t)n);
}

/* Walking the window tree: how a program finds a control it did not create,
 * and how a wizard finds the page it just put up. */
enum { GW_HWNDFIRST = 0, GW_HWNDLAST = 1, GW_HWNDNEXT = 2, GW_HWNDPREV = 3,
       GW_OWNER = 4, GW_CHILD = 5 };
static void u_GetWindow(w32 *w) {
    uint64_t h = ARG(0);
    uint32_t cmd = (uint32_t)ARG(1);
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(h);
    uint64_t r = 0;
    if (p) {
        int self = (int)(p - g_win);
        if (cmd == GW_OWNER) r = p->parent;
        else if (cmd == GW_CHILD) {
            for (int i = 0; i < MAX_WINDOWS && !r; i++)
                if (g_win[i].used && g_win[i].parent == h) r = HW_BASE + (uint64_t)i * HW_STEP;
        } else if (cmd == GW_HWNDNEXT) {
            for (int i = self + 1; i < MAX_WINDOWS && !r; i++)
                if (g_win[i].used && g_win[i].parent == p->parent) r = HW_BASE + (uint64_t)i * HW_STEP;
        } else if (cmd == GW_HWNDPREV) {
            for (int i = self - 1; i >= 0 && !r; i--)
                if (g_win[i].used && g_win[i].parent == p->parent) r = HW_BASE + (uint64_t)i * HW_STEP;
        }
    }
    pthread_mutex_unlock(&g_lock);
    RET(r);
}
static void u_SetParent(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    uint64_t old = p ? p->parent : 0;
    if (p) p->parent = ARG(1);
    pthread_mutex_unlock(&g_lock);
    if (p) invalidate(ARG(1) ? ARG(1) : ARG(0));
    RET(old);
}
static void u_WindowFromPoint(w32 *w) {
    pthread_mutex_lock(&g_lock);
    uint64_t h = window_at((int)(int32_t)(uint32_t)ARG(0), (int)(int32_t)(uint32_t)ARG(1));
    pthread_mutex_unlock(&g_lock);
    RET(h);
}
static void u_ChildWindowFromPoint(w32 *w) { u_WindowFromPoint(w); }
/* EnumChildWindows(parent, callback, param): the callback is guest code and
 * returning FALSE from it stops the walk, which a program relies on to stop
 * early once it has found what it wanted. */
static void u_EnumChildWindows(w32 *w) {
    uint64_t parent = ARG(0), fn = ARG(1), param = ARG(2);
    if (!fn) { RET(0); return; }
    for (int i = 0; i < MAX_WINDOWS; i++) {
        pthread_mutex_lock(&g_lock);
        uint64_t h = (g_win[i].used && g_win[i].parent == parent) ? HW_BASE + (uint64_t)i * HW_STEP : 0;
        pthread_mutex_unlock(&g_lock);
        if (!h) continue;
        uint64_t args[2] = { h, param };
        if (!w32_call_guest(w, fn, 2, args) || w->exited) break;
    }
    RET(1);
}

static void u_LoadStringW(w32 *w) {
    char buf[512];
    int n = w32_load_string(w, ARG(0), (uint32_t)ARG(1), buf, sizeof buf);
    uint64_t out = ARG(2);
    int cap = (int)(int32_t)(uint32_t)ARG(3);
    if (!out || cap <= 0) { RET(0); return; }
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) w32_write(w, out + (unsigned)i * 2, 2, (uint8_t)buf[i]);
    w32_write(w, out + (unsigned)n * 2, 2, 0);
    RET((uint64_t)(uint32_t)n);
}

/* Everything the app's own loop needs to keep a dialog alive: one pass over
 * the queue and one presented frame. Returns 0 when there is no dialog left
 * to pump, which is how the caller knows the installer has finished. */
int w32_dialog_pump(w32 *w) {
    int any = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_WINDOWS; i++) if (g_win[i].used && g_win[i].visible) any = 1;
    pthread_mutex_unlock(&g_lock);
    if (!any) return 0;
    surface_present();
    return 1;
}

static void u_SetTimer(w32 *w) { RET(ARG(1) ? ARG(1) : 1); }
static void u_KillTimer(w32 *w) { (void)w; RET(1); }
static void u_SystemParametersInfoA(w32 *w) { (void)w; RET(1); }
static void u_SetProcessDPIAware(w32 *w) { (void)w; RET(1); }
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
/* ---- walking a string, one character at a time ---------------------------
 *
 * These look like helpers nobody would bother importing, and they are what a
 * Unicode installer stops on. NSIS built for Unicode -- which is every NSIS
 * installer made this decade -- walks every path it handles with CharNextW,
 * so an installer gets as far as its first path and no further. There was a
 * CharNextA here and no CharNextW, which is why one function was the whole
 * distance between "nothing was installed" and a working install.
 *
 * The wide ones are not the narrow ones with the step doubled. A UTF-16
 * character can be a surrogate pair, and CharNextW is defined to step over
 * the pair rather than into the middle of it. Getting that wrong does not
 * crash: it silently corrupts a path that happens to contain a character
 * outside the BMP, which is a far worse failure than the one being fixed.
 *
 * Case mapping is ASCII only. Anything above 0x7F is left alone, which is
 * wrong for accented Latin and for Greek and Cyrillic -- doing it properly
 * needs Unicode case tables, and quietly getting é wrong is better than
 * pretending a locale exists. Paths compare case-insensitively on the ASCII
 * range, which is what the drive does.
 */
static int up_ascii(int c)   { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int down_ascii(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int is_high_surrogate(unsigned c) { return c >= 0xD800 && c <= 0xDBFF; }
static int is_low_surrogate(unsigned c)  { return c >= 0xDC00 && c <= 0xDFFF; }

/* CharNextA: the next byte, unless we are already at the terminator -- in
 * which case the terminator is returned, so a loop that forgot to check
 * stops instead of running off the end. */
static void u_CharNextA(w32 *w) {
    uint64_t p = ARG(0);
    if (!p) { RET(0); return; }
    RET(w32_read(w, p, 1) ? p + 1 : p);
}
static void u_CharNextExA(w32 *w) {
    /* (codepage, string, flags) -- one code page here, so the first argument
     * makes no difference. */
    uint64_t p = ARG(1);
    if (!p) { RET(0); return; }
    RET(w32_read(w, p, 1) ? p + 1 : p);
}
static void u_CharNextW(w32 *w) {
    uint64_t p = ARG(0);
    if (!p) { RET(0); return; }
    unsigned c = (unsigned)w32_read(w, p, 2);
    if (!c) { RET(p); return; }
    if (is_high_surrogate(c) && is_low_surrogate((unsigned)w32_read(w, p + 2, 2))) { RET(p + 4); return; }
    RET(p + 2);
}
/* CharPrev takes the start of the string as well, so it can refuse to go
 * back past it -- which is the only thing making it safe to call in a loop. */
static void u_CharPrevA(w32 *w) {
    uint64_t start = ARG(0), p = ARG(1);
    if (!p || !start || p <= start) { RET(start ? start : p); return; }
    RET(p - 1);
}
static void u_CharPrevExA(w32 *w) {
    uint64_t start = ARG(1), p = ARG(2);
    if (!p || !start || p <= start) { RET(start ? start : p); return; }
    RET(p - 1);
}
static void u_CharPrevW(w32 *w) {
    uint64_t start = ARG(0), p = ARG(1);
    if (!p || !start || p <= start) { RET(start ? start : p); return; }
    uint64_t q = p - 2;
    /* Back over a whole surrogate pair, not into the middle of one. */
    if (q >= start + 2 && is_low_surrogate((unsigned)w32_read(w, q, 2))
                       && is_high_surrogate((unsigned)w32_read(w, q - 2, 2))) q -= 2;
    RET(q < start ? start : q);
}

/* CharUpper and CharLower take either a pointer or a single character packed
 * into the low word -- the same trick MAKEINTRESOURCE uses, and a caller
 * genuinely uses both forms. A pointer is uppercased in place and returned;
 * a character is returned uppercased. */
static void char_case(w32 *w, int wide, int up) {
    uint64_t p = ARG(0);
    if (p < 0x10000) {
        int c = (int)p;
        RET((uint64_t)(uint32_t)(up ? up_ascii(c) : down_ascii(c)));
        return;
    }
    int step = wide ? 2 : 1;
    for (uint64_t at = p;; at += (unsigned)step) {
        uint64_t c = w32_read(w, at, step);
        if (!c) break;
        if (c < 0x80) w32_write(w, at, step, (uint64_t)(uint32_t)(up ? up_ascii((int)c) : down_ascii((int)c)));
    }
    RET(p);
}
static void u_CharUpperA(w32 *w) { char_case(w, 0, 1); }
static void u_CharUpperW(w32 *w) { char_case(w, 1, 1); }
static void u_CharLowerA(w32 *w) { char_case(w, 0, 0); }
static void u_CharLowerW(w32 *w) { char_case(w, 1, 0); }

/* The Buff forms take a count instead of relying on a terminator, and the
 * count is in *characters* -- so the wide one steps two bytes per unit. A
 * caller that passes a byte count to the wide form would corrupt memory past
 * its buffer, which is its bug, not ours; we honour the documented meaning. */
static void char_case_buff(w32 *w, int wide, int up) {
    uint64_t p = ARG(0);
    uint64_t n = ARG(1);
    if (!p) { RET(0); return; }
    if (n > (1u << 24)) n = 1u << 24;              /* a count this large is a bug, not a string */
    int step = wide ? 2 : 1;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t at = p + i * (unsigned)step;
        uint64_t c = w32_read(w, at, step);
        if (c && c < 0x80) w32_write(w, at, step, (uint64_t)(uint32_t)(up ? up_ascii((int)c) : down_ascii((int)c)));
    }
    RET(n);
}
static void u_CharUpperBuffA(w32 *w) { char_case_buff(w, 0, 1); }
static void u_CharUpperBuffW(w32 *w) { char_case_buff(w, 1, 1); }
static void u_CharLowerBuffA(w32 *w) { char_case_buff(w, 0, 0); }
static void u_CharLowerBuffW(w32 *w) { char_case_buff(w, 1, 0); }

/* The Is* family takes a character, never a pointer. */
static int alpha_ascii(unsigned c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static void u_IsCharAlphaA(w32 *w)        { RET(alpha_ascii((unsigned)ARG(0) & 0xFF) ? 1 : 0); }
static void u_IsCharAlphaW(w32 *w)        { RET(alpha_ascii((unsigned)ARG(0) & 0xFFFF) ? 1 : 0); }
static void u_IsCharAlphaNumericA(w32 *w) { unsigned c = (unsigned)ARG(0) & 0xFF;   RET(alpha_ascii(c) || (c >= '0' && c <= '9') ? 1 : 0); }
static void u_IsCharAlphaNumericW(w32 *w) { unsigned c = (unsigned)ARG(0) & 0xFFFF; RET(alpha_ascii(c) || (c >= '0' && c <= '9') ? 1 : 0); }
static void u_IsCharUpperA(w32 *w)        { unsigned c = (unsigned)ARG(0) & 0xFF;   RET(c >= 'A' && c <= 'Z' ? 1 : 0); }
static void u_IsCharUpperW(w32 *w)        { unsigned c = (unsigned)ARG(0) & 0xFFFF; RET(c >= 'A' && c <= 'Z' ? 1 : 0); }
static void u_IsCharLowerA(w32 *w)        { unsigned c = (unsigned)ARG(0) & 0xFF;   RET(c >= 'a' && c <= 'z' ? 1 : 0); }
static void u_IsCharLowerW(w32 *w)        { unsigned c = (unsigned)ARG(0) & 0xFFFF; RET(c >= 'a' && c <= 'z' ? 1 : 0); }

/* wvsprintf: wsprintf given a va_list. Implemented in msvcrt.c beside the
 * formatter, for the same reason wsprintf is. */
static void u_wvsprintfA(w32 *w) { w32_do_wvsprintf(w, 0); }
static void u_wvsprintfW(w32 *w) { w32_do_wvsprintf(w, 1); }

/* ---- the clipboard ------------------------------------------------------
 *
 * One clipboard, no other process to share it with, so it is a buffer. An
 * installer offers to copy its log to the clipboard; a game copies a crash
 * id. Both work, and what is copied can be read back by the same program,
 * which is all either of them does with it. It does not reach the iOS
 * clipboard -- doing that would mean a Windows program could put anything
 * into the pasteboard of the device it is running on, which is not a
 * decision this layer should make on its own. */
enum { CF_TEXT_ = 1, CF_UNICODETEXT_ = 13 };
static int      g_clip_open;
static uint64_t g_clip_mem;
static uint32_t g_clip_fmt;

static void u_OpenClipboard(w32 *w)  { (void)w; g_clip_open = 1; RET(1); }
static void u_CloseClipboard(w32 *w) { (void)w; g_clip_open = 0; RET(1); }
static void u_EmptyClipboard(w32 *w) {
    if (!g_clip_open) { RET(0); return; }
    g_clip_mem = 0; g_clip_fmt = 0;
    RET(1);
}
/* SetClipboardData takes ownership of the handle, which here is the guest
 * pointer GlobalAlloc returned -- so it is kept, not copied, and not freed. */
static void u_SetClipboardData(w32 *w) {
    if (!g_clip_open) { RET(0); return; }
    g_clip_fmt = (uint32_t)ARG(0);
    g_clip_mem = ARG(1);
    RET(ARG(1));
}
static void u_GetClipboardData(w32 *w) {
    if (!g_clip_open) { RET(0); return; }
    RET(g_clip_fmt == (uint32_t)ARG(0) ? g_clip_mem : 0);
}
static void u_IsClipboardFormatAvailable(w32 *w) { RET(g_clip_mem && g_clip_fmt == (uint32_t)ARG(0) ? 1 : 0); }

/* ---- the odds and ends an installer's last page calls ------------------- */

/* ExitWindowsEx: a reboot, which cannot happen and must not be pretended.
 * An installer asks at the end of a run that replaced a file in use; saying
 * no leaves it to report that a restart is needed, which is true. */
static void u_ExitWindowsEx(w32 *w) {
    w32_note_refused(w, "user32!ExitWindowsEx (nothing here can restart the device)");
    w32_set_last_error(w, 1314);                 /* ERROR_PRIVILEGE_NOT_HELD */
    RET(0);
}

/* Class longs. The one that matters is GCL_HICON/HICONSM, which a program
 * sets so its window has an icon; there is no title bar to put one in, and
 * the call has to succeed anyway or the program treats it as a failure. */
static void u_GetClassLongA(w32 *w) { (void)w; RET(0); }
static void u_GetClassLongW(w32 *w) { (void)w; RET(0); }
static void u_SetClassLongA(w32 *w) { (void)w; RET(0); }
static void u_SetClassLongW(w32 *w) { (void)w; RET(0); }
static void u_GetClassLongPtrA(w32 *w) { w32_ret64(w, 0); }
static void u_GetClassLongPtrW(w32 *w) { w32_ret64(w, 0); }
static void u_SetClassLongPtrA(w32 *w) { w32_ret64(w, 0); }
static void u_SetClassLongPtrW(w32 *w) { w32_ret64(w, 0); }

static void u_LoadImageW(w32 *w) { (void)w; RET(0x9003); }
static void u_LoadBitmapA(w32 *w) { (void)w; RET(0); }
static void u_LoadBitmapW(w32 *w) { (void)w; RET(0); }
static void u_DestroyIcon(w32 *w) { (void)w; RET(1); }
static void u_DestroyCursor(w32 *w) { (void)w; RET(1); }
static void u_GetKeyboardLayout(w32 *w) { (void)w; RET(0x04090409u); }   /* en-US */

/* MessageBoxIndirect takes a MSGBOXPARAMS struct instead of arguments. The
 * text is at a fixed offset and the offset differs by bitness, because the
 * three fields before it are a size, an HWND and an HINSTANCE. */
static void u_MessageBoxIndirectW(w32 *w) {
    uint64_t p = ARG(0);
    if (!p) { RET(0); return; }
    int ps = (int)w32_ptrsize(w);
    uint64_t text = w32_read(w, p + 4u + 2u * (unsigned)ps, ps);
    uint64_t cap  = w32_read(w, p + 4u + 3u * (unsigned)ps, ps);
    char t[256] = "", b[512] = "";
    if (cap)  w32_wtoa(w, cap, t, sizeof t);
    if (text) w32_wtoa(w, text, b, sizeof b);
    fprintf(stderr, "[MessageBox] %s: %s\n", t, b);
    RET(1);                                       /* IDOK */
}
static void u_MessageBoxIndirectA(w32 *w) {
    uint64_t p = ARG(0);
    if (!p) { RET(0); return; }
    int ps = (int)w32_ptrsize(w);
    uint64_t text = w32_read(w, p + 4u + 2u * (unsigned)ps, ps);
    uint64_t cap  = w32_read(w, p + 4u + 3u * (unsigned)ps, ps);
    fprintf(stderr, "[MessageBox] %s: %s\n", cap ? w32_str(w, cap) : "", text ? w32_str(w, text) : "");
    RET(1);
}
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
    F(SetWindowTextA, 2), F(GetWindowTextA, 3),
    F(GetWindowLongA, 2), F(SetWindowLongA, 3), F(GetWindowLongPtrA, 2), F(SetWindowLongPtrA, 3),
    F(GetDesktopWindow, 0), F(GetForegroundWindow, 0), F(SetForegroundWindow, 1),
    F(GetActiveWindow, 0), F(SetActiveWindow, 1), F(GetFocus, 0), F(SetFocus, 1),
    F(FindWindowA, 2), F(GetWindowThreadProcessId, 2), F(GetParent, 1),
    /* the message pump */
    F(PeekMessageA, 5), F(PeekMessageW, 5), F(GetMessageA, 4), F(GetMessageW, 4),
    F(TranslateMessage, 1), F(DispatchMessageA, 1), F(DispatchMessageW, 1),
    F(SendMessageA, 4), F(SendMessageW, 4), F(PostMessageA, 4), F(PostMessageW, 4),
    F(PostThreadMessageA, 4), F(PostQuitMessage, 1),
    F(DefWindowProcA, 4), F(DefWindowProcW, 4), F(CallWindowProcA, 5), F(CallWindowProcW, 5),
    F(WaitMessage, 0), F(GetMessageTime, 0), F(GetMessagePos, 0), F(GetQueueStatus, 1),
    /* input */
    F(GetAsyncKeyState, 1), F(GetKeyState, 1), F(GetKeyboardState, 1), F(SetKeyboardState, 1),
    F(GetCursorPos, 1), F(SetCursorPos, 2), F(ShowCursor, 1), F(SetCursor, 1),
    F(LoadCursorA, 2), F(LoadCursorW, 2), F(LoadIconA, 2), F(LoadIconW, 2), F(LoadImageA, 6),
    F(SetCapture, 1), F(ReleaseCapture, 0), F(GetCapture, 0),
    F(ClipCursor, 1), F(GetClipCursor, 1),
    F(MapVirtualKeyA, 2), F(MapVirtualKeyW, 2), F(VkKeyScanA, 1), F(GetKeyNameTextA, 3),
    /* the display, and the calls a message loop needs to get through */
    /* Cdecl and variadic: the caller pops, so the count is not used. NSIS
     * builds every path it reports with this. */
    { "wsprintfA", 2, 1, u_wsprintfA, 0 },
    { "wsprintfW", 2, 1, u_wsprintfW, 0 },
    F(GetSystemMetrics, 1),
    F(EnumDisplaySettingsA, 3), F(EnumDisplaySettingsW, 3), F(EnumDisplayDevicesA, 4),
    F(ChangeDisplaySettingsA, 2), F(ChangeDisplaySettingsExA, 5),
    F(MonitorFromWindow, 2), F(MonitorFromPoint, 3), F(GetMonitorInfoA, 2),
    /* painting */
    F(BeginPaint, 2), F(EndPaint, 2), F(GetDC, 1), F(GetWindowDC, 1), F(GetDCEx, 3), F(ReleaseDC, 2),
    F(InvalidateRect, 3), F(ValidateRect, 2), F(RedrawWindow, 4),
    F(FillRect, 3), F(FrameRect, 3), F(InvertRect, 2), F(DrawFocusRect, 2), F(DrawEdge, 4),
    F(DrawTextA, 5), F(DrawTextW, 5), F(DrawTextExA, 6), F(DrawTextExW, 6),
    F(GetSysColorBrush, 1), F(GetSysColor, 1),
    /* dialogs: the templates in a program's own resources, made real */
    F(CreateDialogParamA, 5), F(CreateDialogParamW, 5),
    F(CreateDialogIndirectParamA, 5), F(CreateDialogIndirectParamW, 5),
    F(DialogBoxParamA, 5), F(DialogBoxParamW, 5),
    F(DialogBoxIndirectParamA, 5), F(DialogBoxIndirectParamW, 5),
    F(EndDialog, 2), F(GetDlgItem, 2), F(GetDlgCtrlID, 1),
    F(SendDlgItemMessageA, 5), F(SendDlgItemMessageW, 5),
    F(SetDlgItemTextA, 3), F(SetDlgItemTextW, 3),
    F(GetDlgItemTextA, 4), F(GetDlgItemTextW, 4),
    F(SetDlgItemInt, 4), F(GetDlgItemInt, 4),
    F(CheckDlgButton, 3), F(IsDlgButtonChecked, 2), F(CheckRadioButton, 4),
    F(MapDialogRect, 2), F(GetDialogBaseUnits, 0),
    F(EnableWindow, 2), F(IsWindowEnabled, 1), F(GetNextDlgTabItem, 3),
    F(IsDialogMessageA, 2), F(IsDialogMessageW, 2),
    F(GetWindowTextW, 3), F(GetWindowTextLengthA, 1), F(GetWindowTextLengthW, 1),
    F(LoadStringA, 4), F(LoadStringW, 4),
    /* the wide half of the window-long family, which a Unicode program uses
     * for subclassing exactly as an ANSI one uses the narrow half */
    { "GetWindowLongW", 2, 0, u_GetWindowLongA, 0 },
    { "SetWindowLongW", 3, 0, u_SetWindowLongA, 0 },
    { "GetWindowLongPtrW", 2, 0, u_GetWindowLongPtrA, 0 },
    { "SetWindowLongPtrW", 3, 0, u_SetWindowLongPtrA, 0 },
    F(SendMessageTimeoutA, 7), F(SendMessageTimeoutW, 7),
    F(SendNotifyMessageA, 4), F(SendNotifyMessageW, 4),
    F(SetPropA, 3), F(SetPropW, 3), F(GetPropA, 2), F(GetPropW, 2),
    F(RemovePropA, 2), F(RemovePropW, 2),
    F(CreateMenu, 0), F(CreatePopupMenu, 0), F(DestroyMenu, 1), F(GetSystemMenu, 2),
    F(GetMenu, 1), F(SetMenu, 2), F(GetMenuItemCount, 1), F(EnableMenuItem, 3),
    F(DeleteMenu, 3), F(AppendMenuA, 4), F(AppendMenuW, 4), F(DrawMenuBar, 1),
    F(TrackPopupMenu, 7), F(TrackPopupMenuEx, 6),
    F(SetWindowsHookExA, 4), F(SetWindowsHookExW, 4),
    F(UnhookWindowsHookEx, 1), F(CallNextHookEx, 4),
    F(GetClassNameA, 3), F(GetClassNameW, 3),
    F(GetWindow, 2), F(SetParent, 2), F(WindowFromPoint, 2),
    F(ChildWindowFromPoint, 3), F(EnumChildWindows, 3),
    { "SetWindowTextW", 2, 0, u_SetWindowTextW_real, 0 },
    F(SetTimer, 4), F(KillTimer, 2), F(SystemParametersInfoA, 4), F(SetProcessDPIAware, 0),
    F(MessageBoxA, 4), F(MessageBoxW, 4),
    F(MessageBoxIndirectA, 1), F(MessageBoxIndirectW, 1),
    /* Walking a string. A Unicode installer walks every path it touches
     * through CharNextW, so these are not optional for it. */
    F(CharNextA, 1), F(CharNextW, 1), F(CharNextExA, 3),
    F(CharPrevA, 2), F(CharPrevW, 2), F(CharPrevExA, 4),
    F(CharUpperA, 1), F(CharUpperW, 1), F(CharLowerA, 1), F(CharLowerW, 1),
    F(CharUpperBuffA, 2), F(CharUpperBuffW, 2), F(CharLowerBuffA, 2), F(CharLowerBuffW, 2),
    F(IsCharAlphaA, 1), F(IsCharAlphaW, 1),
    F(IsCharAlphaNumericA, 1), F(IsCharAlphaNumericW, 1),
    F(IsCharUpperA, 1), F(IsCharUpperW, 1), F(IsCharLowerA, 1), F(IsCharLowerW, 1),
    /* Cdecl like wsprintf, but given a va_list rather than the stack. */
    { "wvsprintfA", 3, 0, u_wvsprintfA, 0 },
    { "wvsprintfW", 3, 0, u_wvsprintfW, 0 },
    F(OpenClipboard, 1), F(CloseClipboard, 0), F(EmptyClipboard, 0),
    F(SetClipboardData, 2), F(GetClipboardData, 1), F(IsClipboardFormatAvailable, 1),
    F(ExitWindowsEx, 2),
    F(GetClassLongA, 2), F(GetClassLongW, 2), F(SetClassLongA, 3), F(SetClassLongW, 3),
    F(GetClassLongPtrA, 2), F(GetClassLongPtrW, 2),
    F(SetClassLongPtrA, 3), F(SetClassLongPtrW, 3),
    F(LoadImageW, 6), F(LoadBitmapA, 2), F(LoadBitmapW, 2),
    F(DestroyIcon, 1), F(DestroyCursor, 1), F(GetKeyboardLayout, 1),
    { 0, 0, 0, 0, 0 },
};
