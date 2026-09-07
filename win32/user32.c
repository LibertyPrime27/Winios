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
#include "image.h"

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

static void screen_size_changed(void);    /* defined with the pointer state it moves */

void w32_set_screen_size(int cx, int cy) {
    /* Bounded rather than trusted: a game that is told the screen is 8 pixels
     * wide does something unhelpful, and one told it is 32768 wide tries to
     * allocate a backbuffer that cannot exist. */
    if (cx >= 320 && cy >= 200 && cx <= 7680 && cy <= 4320) {
        g_screen_w = cx;
        g_screen_h = cy;
        /* The pointer has to end up on the display this call just made. It
         * starts in the middle of one nobody has chosen yet -- winrun resets
         * the input state before it reads `-screen`, and the app sets the
         * size after it has started a guest -- so left alone it sits off the
         * right-hand edge, where GetCursorPos reports a point outside every
         * window and nothing draws it at all. */
        screen_size_changed();
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
    WM_HSCROLL = 0x0114, WM_VSCROLL = 0x0115,
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
    CTL_LISTBOX, CTL_COMBOBOX, CTL_PROGRESS, CTL_PANE,
    /* The common controls. They were registered by InitCommonControls from
     * the beginning -- a program that cannot create one does not get as far
     * as drawing anything -- but nothing painted them, so an installer's
     * licence box, its component list and its tabs came up as empty holes. */
    CTL_LISTVIEW, CTL_TREEVIEW, CTL_TAB, CTL_STATUS, CTL_TRACK,
    CTL_TOOLBAR, CTL_LINK, CTL_HEADER, CTL_UPDOWN, CTL_N
} ctlkind;
enum { HPROC_BASE = 0x00030000u, HPROC_STEP = 4 };
/* LVS_EX_CHECKBOXES. Kept in the window's exstyle because that is where
 * LVM_SETEXTENDEDLISTVIEWSTYLE puts it and where the painter looks. */
enum { LVS_EX_CHECKBOXES_ = 0x00000004u };

/* There is no window manager here, so a window with WS_CAPTION draws its own
 * title bar -- and a title bar takes room. Keeping it *outside* the client
 * area is not cosmetic: a dialog template positions every control from the
 * client origin, and a caption drawn over the client area puts the first row
 * of controls underneath it. */
/* Deep enough for the caption text plus a little air, at whatever DPI we
 * are claiming -- a fixed pixel height clips the title the moment the
 * display is dense enough to be worth scaling for. */
static int caption_height(void) {
    int h = w32_points_to_pixels(9) + 8;
    return h < 18 ? 18 : h;
}
/* A menu bar sits under the caption and takes room out of the client area for
 * exactly the reason the caption does: a window's contents are positioned
 * from the client origin, and a bar drawn over the client area covers the
 * first row of whatever is in it. Shallower than the caption because a bar is
 * text with a little air and nothing else in it. */
static int menu_bar_height(void) {
    int h = w32_points_to_pixels(9) + 6;
    return h < 16 ? 16 : h;
}

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
    /* ...and, when it does not fit in that, the whole of it.
     *
     * 256 bytes is plenty for a caption and a button, and nowhere near enough
     * for the thing that actually needs the most room: a licence agreement in
     * a read-only edit box, which is tens of kilobytes and is the first page
     * of most installers. `text` keeps the leading part so everything that
     * reads a caption stays simple; `big` is what gets drawn. */
    char    *big;
    int      biglen;
    uint64_t font;              /* WM_SETFONT, or 0 for the default */
    ctlkind  ctl;               /* non-zero: we draw it, we handle its clicks */
    uint64_t id;                /* child identifier -- the menu argument */
    int      enabled;
    int      focused;           /* has the keyboard: drawn with a focus rectangle */
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
    uint64_t icon;              /* WM_SETICON, drawn in the caption */
    /* scroll bars, for the controls that have them and the windows that ask
     * for one with WS_VSCROLL. A licence agreement in a read-only edit box is
     * the case that matters: without a bar there is nothing to say the text
     * continues, and nothing to move it with. */
    int      scroll_pos, scroll_max, scroll_page;
    int      hscroll_pos, hscroll_max, hscroll_page;
    int      top;               /* first visible item or line */
} wwin;

/* List boxes are rare and their contents are not: one store per list box
 * that exists, rather than a fixed array on every window. */
/* One store per list-ish control that exists. A list view's component list
 * and a tab control's labels live here too, which is why there are more of
 * them and more room in each than a combo box ever needed. */
enum { MAX_LISTS = 24, LIST_ITEMS = 256, LIST_TEXT = 128 };
static struct {
    int  used, n;
    char item[LIST_ITEMS][LIST_TEXT];
    /* A list view's check boxes (LVS_EX_CHECKBOXES), which is how every
     * installer's component page asks what to install. */
    unsigned char checked[LIST_ITEMS];
    /* Column widths, for a report-mode list view. Zero columns means the
     * control is in list or icon mode and the whole row is one string. */
    int  ncol, colw[8];
    char colname[8][32];
} g_list[MAX_LISTS];

/* Menus, on the same principle as the list store above: one entry per menu
 * that exists, a fixed array of them, and a handle that is an index in
 * disguise. A menu bar and each of its popups are separate menus -- that is
 * how Windows models them and how a resource stores them -- so a program with
 * a File/Edit/View/Help bar owns five of these before any submenu.
 *
 * Until now these were handles with nothing behind them: distinct numbers,
 * an honest item count of zero, and nothing drawn. That is survivable for an
 * installer, which has no menu and only asks for its system menu so it can
 * grey out Close, and not survivable for a game with a menu bar -- the bar
 * simply was not there. */
enum { MAX_MENUS = 48, MENU_ITEMS = 64, MENU_TEXT = 64 };
typedef struct {
    char     text[MENU_TEXT];
    uint32_t id;                /* the WM_COMMAND identifier */
    uint32_t flags;             /* MF_*, as the program or the resource gave them */
    uint64_t sub;               /* a popup menu, for MF_POPUP */
} mitem;
typedef struct { int used, n; mitem it[MENU_ITEMS]; } wmenu;
static wmenu g_menu[MAX_MENUS];
enum { MENU_BASE = 0x000C0000u, MENU_STEP = 4 };

/* Menu item flags. MF_END is the resource format's "last item at this level"
 * and shares its value with MF_HILITE, which is why the parser strips it
 * before the flags are kept. */
enum {
    MF_STRING = 0x0000, MF_ENABLED = 0x0000, MF_UNCHECKED = 0x0000,
    MF_GRAYED = 0x0001, MF_DISABLED = 0x0002, MF_BITMAP = 0x0004,
    MF_CHECKED = 0x0008, MF_POPUP = 0x0010, MF_MENUBARBREAK = 0x0020,
    MF_MENUBREAK = 0x0040, MF_HILITE = 0x0080, MF_OWNERDRAW = 0x0100,
    MF_SEPARATOR = 0x0800, MF_BYCOMMAND = 0x0000, MF_BYPOSITION = 0x0400,
    MF_END = 0x0080,
};
/* TrackPopupMenu's flags. Only the two that change what happens are read:
 * where the menu is placed relative to the point, and whether the chosen
 * command comes back as the return value instead of as a WM_COMMAND. */
enum {
    TPM_CENTERALIGN = 0x0004, TPM_RIGHTALIGN = 0x0008,
    TPM_VCENTERALIGN = 0x0010, TPM_BOTTOMALIGN = 0x0020,
    TPM_RETURNCMD = 0x0100,
};
/* MENUITEMINFO's fMask bits. */
enum {
    MIIM_STATE = 0x01, MIIM_ID = 0x02, MIIM_SUBMENU = 0x04, MIIM_CHECKMARKS = 0x08,
    MIIM_TYPE = 0x10, MIIM_DATA = 0x20, MIIM_STRING = 0x40, MIIM_BITMAP = 0x80,
    MIIM_FTYPE = 0x100,
};

static wclass g_cls[MAX_CLASSES];
static wwin   g_win[MAX_WINDOWS];
enum { HW_BASE = 0x00050000u, HW_STEP = 4 };

/* Everything the UI thread and the guest thread both touch. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static qmsg    g_q[MAX_MSGS];
static int     g_qhead, g_qtail;        /* head == tail: empty */
/* Which cursor the program last set. Drawn by the compositor at the end of a
 * repaint, so a game that hides it or picks the wait cursor is believed. */
static uint64_t g_cursor;
/* Has anything ever been drawn into the screen surface?
 *
 * Moving the pointer changes the finished frame without anything having
 * painted, so it has to be able to ask for one to be presented. But only for
 * a surface that is actually the picture: a Direct3D guest presents its own
 * frames and never touches this one, and presenting an empty desktop between
 * its frames would both flicker and -- because a presented frame is what an
 * input script counts as a frame -- move every scripted event. */
static int g_surf_live;
/* Signalled whenever anything is queued, so GetMessage can wait for input
 * instead of spinning on the queue or -- as it used to -- giving up on it. */
static pthread_cond_t g_qcond = PTHREAD_COND_INITIALIZER;
static uint8_t g_keys[256];             /* 0x80 down, 0x01 toggled */
static uint8_t g_keys_hit[256];         /* pressed since the last GetAsyncKeyState */
static uint32_t g_keychar[256];         /* the character the host resolved for this key, if any */
static int32_t g_mx, g_my;              /* pointer, in client pixels */
static int     g_mouse_moved;           /* has anything put the pointer somewhere? */
static int32_t g_rel_dx, g_rel_dy;      /* relative motion not yet consumed */
static uint32_t g_buttons;              /* bit 0 left, 1 right, 2 middle */
static int32_t g_wheel;
static int     g_quit, g_quit_code;
static uint64_t g_focus;                /* the window input goes to */
static uint64_t g_capture;
static int     g_cursor_shown = 1;
static int     g_cursor_count;          /* ShowCursor's counter */

/* The menu that is open, if any.
 *
 * A menu is not a window here. It is drawn as an overlay over the finished
 * frame -- above every window, because that is where a menu is -- and while
 * it is up it takes the mouse and the keyboard away from the windows
 * underneath. That second half is what makes a click past an open menu close
 * the menu instead of pressing whatever it landed on.
 *
 * Levels, because a submenu does not replace the popup it came from: both
 * stay on screen, which is the entire visual grammar of a cascading menu. */
enum { MENU_DEPTH = 4 };
static struct {
    int      n;                     /* popups on screen; 0 means no menu is open */
    uint64_t owner;                 /* the window a chosen command is posted to */
    uint64_t barwnd;                /* whose menu bar is dropped, 0 for a context menu */
    int      baritem;               /* which bar item, -1 for a context menu */
    struct { uint64_t menu; int x, y, w, h, hot; } lv[MENU_DEPTH];
    int      tracking;              /* TrackPopupMenu is running its own message loop */
    uint32_t tflags;                /* its TPM_* flags */
    int      chosen;                /* what that loop will return; -1 until something is */
} g_pop;

/* --- the menu store ------------------------------------------------------
 *
 * A menu handle is an index into g_menu wearing a disguise, exactly as a
 * window handle is an index into g_win. Callers hold g_lock.
 */
static wmenu *menu_of(uint64_t h) {
    if (h < MENU_BASE) return 0;
    uint64_t i = (h - MENU_BASE) / MENU_STEP;
    if ((h - MENU_BASE) % MENU_STEP || i >= MAX_MENUS || !g_menu[i].used) return 0;
    return &g_menu[i];
}
static uint64_t menu_new(void) {
    for (int i = 0; i < MAX_MENUS; i++) if (!g_menu[i].used) {
        memset(&g_menu[i], 0, sizeof g_menu[i]);
        g_menu[i].used = 1;
        return MENU_BASE + (uint64_t)i * MENU_STEP;
    }
    fprintf(stderr, "winrun: user32: out of menus\n");
    return 0;
}
/* A copy to draw or measure from. Nothing that touches gdi32 may hold g_lock
 * -- gdi32 takes it again to reach the screen surface, and the mutex is not
 * recursive -- so every painter works from one of these instead. */
static int menu_snapshot(uint64_t h, wmenu *out) {
    pthread_mutex_lock(&g_lock);
    wmenu *m = menu_of(h);
    if (m) *out = *m;
    pthread_mutex_unlock(&g_lock);
    return m != 0;
}

/* MF_BYPOSITION or MF_BYCOMMAND: which one a call means is a flag in it, and
 * getting it backwards is the classic way a menu call silently does nothing.
 * By command it descends into submenus, because EnableMenuItem(hMenuBar,
 * ID_FILE_OPEN, MF_GRAYED) is addressed to the bar and means an item three
 * levels down. Caller holds the lock. */
static mitem *menu_find(uint64_t h, uint32_t which, uint32_t flags, int depth) {
    wmenu *m = menu_of(h);
    if (!m || depth > 8) return 0;
    /* Unsigned throughout: a caller that passes a command id where a position
     * was expected hands over a number in the billions, and comparing that as
     * a signed int makes it look like a small position. */
    if (flags & MF_BYPOSITION)
        return which < (uint32_t)m->n ? &m->it[which] : 0;
    for (int i = 0; i < m->n; i++)
        if (!(m->it[i].flags & (MF_POPUP | MF_SEPARATOR)) && m->it[i].id == which)
            return &m->it[i];
    for (int i = 0; i < m->n; i++)
        if (m->it[i].flags & MF_POPUP) {
            mitem *r = menu_find(m->it[i].sub, which, flags, depth + 1);
            if (r) return r;
        }
    return 0;
}

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
    g_mouse_moved = 0;
    g_rel_dx = g_rel_dy = 0;
    g_buttons = 0; g_wheel = 0;
    g_quit = 0; g_quit_code = 0;
    g_focus = 0; g_capture = 0;
    g_cursor_shown = 1; g_cursor_count = 0;
    g_cursor = 0; g_surf_live = 0;
    memset(g_list, 0, sizeof g_list);
    memset(g_menu, 0, sizeof g_menu);
    memset(&g_pop, 0, sizeof g_pop);
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
    pthread_cond_broadcast(&g_qcond);
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

/* Put the pointer back on the display, wherever the display has got to.
 *
 * A pointer nobody has touched is still where the reset put it -- the middle
 * of the screen -- and it belongs in the middle of the new one rather than
 * wherever the old middle happens to fall. Once something has moved it, its
 * position is a fact about the session and is only clamped back inside. */
static void screen_size_changed(void) {
    pthread_mutex_lock(&g_lock);
    if (!g_mouse_moved) { g_mx = SCREEN_W / 2; g_my = SCREEN_H / 2; }
    if (g_mx < 0) g_mx = 0;
    if (g_my < 0) g_my = 0;
    if (g_mx >= SCREEN_W) g_mx = SCREEN_W - 1;
    if (g_my >= SCREEN_H) g_my = SCREEN_H - 1;
    pthread_mutex_unlock(&g_lock);
}

/* The pointer moved, changed shape, or was hidden. Nothing painted, but the
 * frame the pointer is composited into is different -- so ask for it to be
 * presented again, and only if that surface is the one on the display. */
static void cursor_damaged(void) { if (g_surf_live) w32_desktop_damaged(); }

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
    g_mx = x; g_my = y; g_mouse_moved = 1;
    push(0, WM_MOUSEMOVE, mouse_wp(), xy_lp(x, y), x, y);
    pthread_mutex_unlock(&g_lock);
    cursor_damaged();
}

/* Relative motion with no pointer to move: a trackpad or mouse in the
 * "captured" state a game puts one in for mouselook. The pointer still moves,
 * clamped to the screen, so a program that reads GetCursorPos sees something
 * sensible -- but the deltas are kept whole, because clamping them is what
 * makes a view stop turning at the edge of the screen. */
void w32_input_mouse_delta(int dx, int dy) {
    pthread_mutex_lock(&g_lock);
    g_rel_dx += dx; g_rel_dy += dy;
    g_mx += dx; g_my += dy; g_mouse_moved = 1;
    if (g_mx < 0) g_mx = 0;
    if (g_mx >= SCREEN_W) g_mx = SCREEN_W - 1;
    if (g_my < 0) g_my = 0;
    if (g_my >= SCREEN_H) g_my = SCREEN_H - 1;
    push(0, WM_MOUSEMOVE, mouse_wp(), xy_lp(g_mx, g_my), g_mx, g_my);
    pthread_mutex_unlock(&g_lock);
    cursor_damaged();
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

/* CreateWindowEx's ninth argument is an HMENU for a top-level window and a
 * child identifier for a child, and both land in the same field. So the test
 * for "does this window have a menu bar" is not whether the field is set but
 * whether what is in it names a menu with anything in it -- an empty menu
 * gets no bar, which is also what Windows draws. Caller holds the lock. */
static uint64_t menu_of_window(const wwin *p) {
    if (!p || (p->style & WS_CHILD)) return 0;
    wmenu *m = menu_of(p->menu);
    return m && m->n ? p->menu : 0;
}
static uint64_t window_menu(uint64_t hwnd) {
    pthread_mutex_lock(&g_lock);
    uint64_t h = menu_of_window(win_of(hwnd));
    pthread_mutex_unlock(&g_lock);
    return h;
}
/* Work out where the client area starts and how tall it is, given whatever
 * chrome this window is currently showing. SetMenu can arrive long after the
 * window was made, and a bar that appeared without the client area shrinking
 * would be drawn straight over the top row of the contents. Caller holds the
 * lock; `wh` is the window height. */
static void apply_chrome(wwin *p) {
    int cyo = 0;
    if (!(p->style & WS_CHILD) && (p->style & WS_CAPTION) == WS_CAPTION) cyo = caption_height();
    if (menu_of_window(p)) cyo += menu_bar_height();
    p->cyo = cyo;
    p->ch = p->h - cyo > 1 ? p->h - cyo : 1;
}

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
static void move_window(w32 *w, uint64_t hwnd, int nx, int ny, int ncw, int nch, int repaint);
static void repaint_area(w32 *w, int x, int y, int cx, int cy);
static uint64_t deliver(w32 *w, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int wide);
static uint64_t call_proc(w32 *w, uint64_t proc, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int wide);
static uint64_t ctl_proc(w32 *w, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int wide);
static uint64_t dlg_default_proc(w32 *w, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int wide);
static void paint_control(w32 *w, uint64_t hwnd, wwin *snap);
static uint64_t window_at(int x, int y);
static void surface_present(void);
/* The menu bar takes room out of the client area the way the caption does,
 * so how deep it is has to be known where a window is sized as well as where
 * one is drawn. */
static int menu_bar_height(void);
static uint64_t window_menu(uint64_t hwnd);         /* the bar this window shows, or 0 */
/* Whether an open menu wanted this message. Everything that dispatches input
 * asks first: a menu on screen is modal over the windows under it. */
static int menu_input(w32 *w, uint32_t msg, uint64_t wp, int sx, int sy);
static int is_key_msg(uint32_t msg);
/* The open menu and the mouse pointer, put into the frame on its way to the
 * display and taken out again immediately afterwards. Defined with the menu
 * painters, because that is what they draw with. */
static void overlay_compose(void);
static void overlay_restore(void);

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
     * This used to apply only to windows of a class we recognise, on the
     * reasoning that a window the program draws itself should not have chrome
     * invented for it. That was wrong, and an installer showed why: a Delphi
     * or MFC program registers its own class for its main form, sets
     * WS_CAPTION, and expects the system to draw the title bar exactly as it
     * does for anyone else -- so the window came up with a blank blue strip
     * and no title on it. WS_CAPTION is the program asking for a caption, and
     * that is the whole test. A game's full-screen window does not set it.
     *
     * A menu bar is the same argument again, and apply_chrome does both: the
     * HMENU passed to CreateWindowEx is in `menu` by now, so a window created
     * with its menu already built gets the room taken out here rather than
     * having to be resized when the first bar item is drawn. */
    apply_chrome(p);
    if (p->ctl == CTL_LISTBOX || p->ctl == CTL_COMBOBOX ||
        p->ctl == CTL_LISTVIEW || p->ctl == CTL_TREEVIEW ||
        p->ctl == CTL_TAB || p->ctl == CTL_STATUS) {
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
            free(g_win[i].big); g_win[i].big = 0; g_win[i].biglen = 0;
            g_win[i].used = 0;
        }
    if (g_focus == hwnd) g_focus = 0;
    pthread_mutex_unlock(&g_lock);
    /* The surface keeps whatever was last drawn on it, so a window that goes
     * away leaves its pixels behind unless what was under it is drawn again. */
    if (had) repaint_area(w, ex, ey, ew, eh);
}

void w32_set_window_text(w32 *w, uint64_t hwnd, const char *s) {
    (void)w;
    if (!s) s = "";
    size_t n = strlen(s);
    char *copy = 0;
    if (n >= sizeof ((wwin *)0)->text) {
        copy = (char *)malloc(n + 1);
        if (copy) memcpy(copy, s, n + 1);
    }
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p) {
        snprintf(p->text, sizeof p->text, "%s", s);
        free(p->big);
        p->big = copy;
        p->biglen = copy ? (int)n : 0;
        p->top = 0;
        p->scroll_pos = 0;
        copy = 0;
    }
    pthread_mutex_unlock(&g_lock);
    free(copy);
}
/* The whole text, or the short one when there is no long one. Callers hold
 * g_lock; the pointer is only valid while they do. */
static const char *win_text(const wwin *p) {
    return p && p->big ? p->big : (p ? p->text : "");
}
void w32_get_window_text(w32 *w, uint64_t hwnd, char *out, size_t n) {
    (void)w;
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    snprintf(out, n, "%s", p ? win_text(p) : "");
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
    int x = p ? p->x : 0, y = p ? p->y : 0, cw = p ? p->w : 0, ch = p ? p->h : 0;
    if (p) p->visible = ARG(1) != SW_HIDE;
    int now = p ? p->visible : 0;
    pthread_mutex_unlock(&g_lock);
    if (now) invalidate(ARG(0));
    else if (was) repaint_area(w, x, y, cw, ch);   /* it was covering something */
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

/* Moving a window uncovers where it used to be. NSIS moves its inner page
 * dialog rather than recreating it on some pages, so this is the same bug in
 * a different disguise. */
static void move_window(w32 *w, uint64_t hwnd, int nx, int ny, int ncw, int nch, int repaint) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (!p) { pthread_mutex_unlock(&g_lock); return; }
    int ox = p->x, oy = p->y, ow = p->w, oh = p->h;
    int was_visible = p->visible;
    if (p->parent) {
        wwin *pp = win_of(p->parent);
        if (pp) { nx += pp->x + pp->cxo; ny += pp->y + pp->cyo; }
    }
    p->x = nx; p->y = ny;
    if (ncw > 0 && nch > 0) {
        p->w = ncw; p->h = nch;
        p->cw = ncw; p->ch = nch - p->cyo > 1 ? nch - p->cyo : 1;
    }
    int moved = (ox != nx || oy != ny || ow != p->w || oh != p->h);
    pthread_mutex_unlock(&g_lock);
    if (!moved) { if (repaint) invalidate(hwnd); return; }
    if (was_visible) repaint_area(w, ox, oy, ow, oh);
    if (repaint) invalidate(hwnd);
}

static void u_MoveWindow(w32 *w) {
    uint64_t h = ARG(0);
    if (!h) { RET(0); return; }
    move_window(w, h, (int)(int32_t)(uint32_t)ARG(1), (int)(int32_t)(uint32_t)ARG(2),
                (int)(int32_t)(uint32_t)ARG(3), (int)(int32_t)(uint32_t)ARG(4),
                ARG(5) != 0);
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(h);
    if (p) push(h, WM_SIZE, 0, xy_lp(p->cw, p->ch), 0, 0);
    pthread_mutex_unlock(&g_lock);
    RET(p ? 1 : 0);
}
/* SetWindowPos(hwnd, after, x, y, cx, cy, flags): SWP_NOSIZE/NOMOVE are 1/2 */
static void u_SetWindowPos(w32 *w) {
    uint32_t f = (uint32_t)ARG(6);
    uint64_t h = ARG(0);
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(h);
    int nx = p ? p->x : 0, ny = p ? p->y : 0, ncw = 0, nch = 0;
    if (p) {
        /* MoveWindow takes parent-relative coordinates and so does this, so
         * the current screen position has to be turned back into one or the
         * window walks across the screen by its parent's origin each time. */
        int px = 0, py = 0;
        if (p->parent) { wwin *pp = win_of(p->parent); if (pp) { px = pp->x + pp->cxo; py = pp->y + pp->cyo; } }
        nx -= px; ny -= py;
        if (!(f & 2)) { nx = (int)(int32_t)(uint32_t)ARG(2); ny = (int)(int32_t)(uint32_t)ARG(3); }
        if (!(f & 1)) {
            int cx = (int)(int32_t)(uint32_t)ARG(4), cy = (int)(int32_t)(uint32_t)ARG(5);
            if (cx > 0 && cy > 0) { ncw = cx; nch = cy; }
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (!p) { RET(0); return; }
    move_window(w, h, nx, ny, ncw, nch, 1);
    if (ncw) {
        pthread_mutex_lock(&g_lock);
        wwin *q = win_of(h);
        if (q) push(h, WM_SIZE, 0, xy_lp(q->cw, q->ch), 0, 0);
        pthread_mutex_unlock(&g_lock);
    }
    /* SWP_SHOWWINDOW / SWP_HIDEWINDOW: 0x40 / 0x80. A wizard shows and hides
     * its pages through here as often as through ShowWindow. */
    if (f & 0x40) { pthread_mutex_lock(&g_lock); { wwin *q = win_of(h); if (q) q->visible = 1; } pthread_mutex_unlock(&g_lock); invalidate(h); }
    if (f & 0x80) {
        int ex = 0, ey = 0, ew = 0, eh = 0;
        int had = w32_window_area(h, 1, &ex, &ey, &ew, &eh);
        pthread_mutex_lock(&g_lock); { wwin *q = win_of(h); if (q) q->visible = 0; } pthread_mutex_unlock(&g_lock);
        if (had) repaint_area(w, ex, ey, ew, eh);
    }
    RET(1);
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

/* PeekMessage(msg, hwnd, min, max, flags)
 *
 * A program that peeks, finds nothing, and peeks again is idling as fast as
 * the CPU will let it. On a desktop that wastes a core; on a phone iOS
 * terminates the process for it -- 80% of a CPU averaged over a minute is the
 * documented limit, and a spin loop passes it in a minute exactly.
 *
 * A game's loop also peeks and finds nothing, every frame, and that one is
 * doing real work in between and must not be slowed down. The two are told
 * apart by whether anything happened: `w32_idle_tick` is reset whenever a
 * frame is presented or a message is queued, so it only ever climbs while the
 * guest is genuinely doing nothing, and a millisecond of sleep then costs
 * nothing and saves the process.
 */
static int g_idle_peeks;
void w32_note_activity(void) { g_idle_peeks = 0; }

static void u_PeekMessageA(w32 *w) {
    int got = take(w, ARG(0), ARG(1), (uint32_t)ARG(2), (uint32_t)ARG(3),
                   ((uint32_t)ARG(4) & PM_REMOVE) != 0);
    if (got) { g_idle_peeks = 0; RET(1); return; }
    if (++g_idle_peeks > 64) {
        struct timespec ts = { 0, 1000000L };      /* 1 ms */
        nanosleep(&ts, 0);
        if (g_idle_peeks > 1000000) g_idle_peeks = 1000;
    }
    RET(0);
}
static void u_PeekMessageW(w32 *w) { u_PeekMessageA(w); }

/* How long GetMessage waits for input before deciding nobody is coming.
 *
 * This is the difference between a program that works and a process iOS
 * kills, and it took a crash report to see it. GetMessage used to return
 * WM_QUIT the moment the queue was empty, on the reasoning that there was no
 * other thread to produce input. That was true of the command-line runner and
 * has not been true since the app existed: an installer that reaches its
 * first page and waits for a click has an empty queue, and telling it to quit
 * -- or leaving its loop to spin on PeekMessage -- is either an early exit or
 * a core held at 100% until iOS terminates the process for it.
 *
 * So it waits. On a device it waits for a touch, which is what Windows does.
 * With nobody there to touch anything -- the test suite, a headless run -- it
 * gives up after this long and returns WM_QUIT as before, so a guest still
 * terminates rather than hanging a CI job.
 */
enum { GETMSG_IDLE_MS = 15000 };

/* Wait for something to arrive, the guest to be asked to stop, or the patience
 * above to run out. Returns 1 if it is worth looking at the queue again. */
static int wait_for_message(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += GETMSG_IDLE_MS / 1000;
    ts.tv_nsec += (long)(GETMSG_IDLE_MS % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    int rc = 0;
    pthread_mutex_lock(&g_lock);
    while (g_qhead == g_qtail && !g_quit) {
        if (pthread_cond_timedwait(&g_qcond, &g_lock, &ts) != 0) break;
    }
    rc = g_qhead != g_qtail || g_quit;
    pthread_mutex_unlock(&g_lock);
    return rc;
}

/* GetMessage(msg, hwnd, min, max): blocks until there is one. */
static void u_GetMessageA(w32 *w) {
    for (int round = 0; round < 2; round++) {
        if (take(w, ARG(0), ARG(1), (uint32_t)ARG(2), (uint32_t)ARG(3), 1)) {
            uint64_t m = ARG(0);
            uint32_t msg = (uint32_t)w32_read(w, m + (w->is32 ? 4 : 8), 4);
            RET(msg == WM_QUIT ? 0 : 1);
            return;
        }
        if (round == 0 && wait_for_message()) continue;
        break;
    }
    qmsg q; memset(&q, 0, sizeof q);
    q.msg = WM_QUIT; q.time = tick_ms();
    put_msg(w, ARG(0), &q, 0);
    if (w->verbose) fprintf(stderr, "winrun: user32: GetMessage waited %d ms with an empty queue; returning WM_QUIT\n", GETMSG_IDLE_MS);
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
    /* An open menu comes first, because it is modal over the windows beneath
     * it: the click that dismisses a menu must not also press whatever it
     * landed on, and the arrow keys belong to the menu rather than to the
     * game behind it. lParam is still in screen coordinates here, which is
     * what a menu is positioned in. */
    if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || is_key_msg(msg)) {
        int sx = (int)(int16_t)(lp & 0xFFFF), sy = (int)(int16_t)((lp >> 16) & 0xFFFF);
        if (menu_input(w, msg, wp, sx, sy)) { surface_present(); RET(0); return; }
    }
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
        /* ...or a window of the program's own class that has a menu bar. A
         * program that builds a File/Edit/Help bar is a windowed application
         * and the bar is ours to draw, so it needs painting even though the
         * inside of the window is not ours; paint_control draws nothing for
         * CTL_NONE beyond the chrome, so this cannot land on the client area.
         *
         * A caption alone deliberately does not qualify. A game's window is
         * routinely WS_OVERLAPPEDWINDOW and then presents Direct3D frames
         * over the top of it, and painting a title bar into the desktop
         * surface for that window would put an empty desktop on the display
         * every time the pointer moved -- the surface and the game's frames
         * go to the same place, and only one of them is the picture. */
        int have = p && (p->ctl || menu_of_window(p));
        if (have) { snap = *p; snap.focused = (g_focus == hwnd); }
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
    g_mouse_moved = 1;
    pthread_mutex_unlock(&g_lock);
    cursor_damaged();
    RET(1);
}
static void u_ShowCursor(w32 *w) {
    pthread_mutex_lock(&g_lock);
    g_cursor_count += ARG(0) ? 1 : -1;
    g_cursor_shown = g_cursor_count >= 0;
    int c = g_cursor_count;
    pthread_mutex_unlock(&g_lock);
    cursor_damaged();
    RET((uint64_t)(int64_t)c);
}
/* ---------------------------------------------------------- pictures
 *
 * These used to return a plausible handle and no pixels, which is why an
 * installer came up with a blank banner, an empty title bar and no icons: the
 * program asked, was told yes, and drew nothing. Everything below turns the
 * ask into an actual decoded image (win32/image.c).
 *
 * The three sources are the same three Windows has. A resource id in the
 * program's own file is by far the most common. A path, when LR_LOADFROMFILE
 * is set -- which is how an installer shows artwork it has just unpacked. And
 * the system's own, for the standard cursors and the message-box symbols,
 * which are in nobody's resources and have to be drawn.
 */

/* Fetch one RT_ICON / RT_CURSOR by id, for the group directory walker. */
typedef struct { w32 *w; uint64_t inst; int type; } res_ctx;
static const uint8_t *fetch_icon_res(void *ctx, int id, uint32_t *size) {
    res_ctx *c = (res_ctx *)ctx;
    uint64_t hr = w32_find_resource(c->w, c->inst, (uint64_t)c->type, (uint64_t)id, 0);
    if (!hr) return 0;
    uint64_t data = w32_resource_data(c->w, hr, size);
    return data ? (const uint8_t *)W32P(c->w, data) : 0;
}

/* An icon or cursor out of a module: RT_GROUP_ICON names the sizes, each one
 * an RT_ICON of its own. `want` is the size the caller wants, so a 16-pixel
 * title bar gets the 16-pixel drawing rather than a shrunk 256. */
static uint64_t load_icon_res(w32 *w, uint64_t inst, uint64_t name, int wide,
                              int cursor, int want) {
    res_ctx c = { w, inst, cursor ? 1 /* RT_CURSOR */ : 3 /* RT_ICON */ };
    uint64_t hr = w32_find_resource(w, inst, cursor ? 12 : 14, name, wide);
    uint32_t size = 0;
    if (hr) {
        uint64_t data = w32_resource_data(w, hr, &size);
        if (data) {
            w32_image im = { 0, 0, 0 };
            int hx = 0, hy = 0;
            if (w32_icon_group((const uint8_t *)W32P(w, data), size, want,
                               fetch_icon_res, &c, &im, &hx, &hy))
                return w32_gdi_make_icon(im.w, im.h, im.px, hx, hy);
        }
    }
    /* Some programs name an RT_ICON directly rather than its group. */
    hr = w32_find_resource(w, inst, cursor ? 1 : 3, name, wide);
    if (hr) {
        uint64_t data = w32_resource_data(w, hr, &size);
        w32_image im = { 0, 0, 0 };
        if (data && w32_icon_decode((const uint8_t *)W32P(w, data), size, &im))
            return w32_gdi_make_icon(im.w, im.h, im.px, 0, 0);
    }
    return 0;
}

/* The whole of a file on the virtual C:, for LR_LOADFROMFILE. */
static uint8_t *slurp(w32 *w, const char *path, size_t *outn) {
    char host[1024];
    host[0] = 0;
    w32_host_path(w, path, host, sizeof host);
    FILE *fp = host[0] ? fopen(host, "rb") : 0;
    if (!fp) return 0;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return 0; }
    long n = ftell(fp);
    if (n <= 0 || n > 64 * 1024 * 1024) { fclose(fp); return 0; }
    rewind(fp);
    uint8_t *d = (uint8_t *)malloc((size_t)n);
    if (!d) { fclose(fp); return 0; }
    size_t got = fread(d, 1, (size_t)n, fp);
    fclose(fp);
    if (got != (size_t)n) { free(d); return 0; }
    *outn = got;
    return d;
}

/* LoadImage(hinst, name, type, cx, cy, flags). type: 0 bitmap, 1 icon,
 * 2 cursor. LR_LOADFROMFILE is 0x10. */
enum { IMAGE_BITMAP_ = 0, IMAGE_ICON_ = 1, IMAGE_CURSOR_ = 2, LR_LOADFROMFILE_ = 0x10 };

static void load_image(w32 *w, int wide) {
    uint64_t inst = ARG(0), name = ARG(1);
    int type = (int)ARG(2), cx = (int)(int32_t)(uint32_t)ARG(3),
        cy = (int)(int32_t)(uint32_t)ARG(4);
    uint32_t flags = (uint32_t)ARG(5);
    (void)cy;

    if (flags & LR_LOADFROMFILE_) {
        char path[512];
        if (wide) w32_wtoa(w, name, path, sizeof path);
        else snprintf(path, sizeof path, "%.511s", (const char *)W32P(w, name));
        size_t n = 0;
        uint8_t *d = slurp(w, path, &n);
        if (!d) { RET(0); return; }
        w32_image im = { 0, 0, 0 };
        int hx = 0, hy = 0, ok = 0;
        uint64_t h = 0;
        if (type == IMAGE_BITMAP_ && n > 14 && d[0] == 'B' && d[1] == 'M') {
            /* A .bmp file is a 14-byte file header and then the packed DIB
             * an RT_BITMAP resource holds on its own. */
            ok = w32_dib_decode(d + 14, n - 14, &im);
            if (ok) h = w32_gdi_make_bitmap(im.w, im.h, im.px);
        } else {
            ok = w32_ico_file(d, n, cx > 0 ? cx : 0, &im, &hx, &hy);
            if (ok) h = w32_gdi_make_icon(im.w, im.h, im.px, hx, hy);
        }
        free(d);
        RET(h);
        return;
    }

    if (type == IMAGE_BITMAP_) {
        uint64_t hr = w32_find_resource(w, inst, 2 /* RT_BITMAP */, name, wide);
        uint32_t size = 0;
        if (hr) {
            uint64_t data = w32_resource_data(w, hr, &size);
            w32_image im = { 0, 0, 0 };
            if (data && w32_dib_decode((const uint8_t *)W32P(w, data), size, &im)) {
                RET(w32_gdi_make_bitmap(im.w, im.h, im.px));
                return;
            }
        }
        RET(0);
        return;
    }
    uint64_t h = load_icon_res(w, inst, name, wide, type == IMAGE_CURSOR_,
                               cx > 0 ? cx : (type == IMAGE_CURSOR_ ? 32 : 32));
    if (!h && !inst) {
        /* A system image: the stock cursors, and the message-box symbols. */
        w32_image im = { 0, 0, 0 };
        int hx = 0, hy = 0;
        if (type == IMAGE_CURSOR_ ? w32_stock_cursor((int)name, &im, &hx, &hy)
                                  : w32_stock_icon((int)name, cx > 0 ? cx : 32, &im))
            h = w32_gdi_make_icon(im.w, im.h, im.px, hx, hy);
    }
    RET(h);
}

static void u_LoadImageA(w32 *w) { load_image(w, 0); }
static void u_LoadImageW(w32 *w) { load_image(w, 1); }

static void load_cursor(w32 *w, int wide) {
    uint64_t inst = ARG(0), name = ARG(1);
    uint64_t h = inst ? load_icon_res(w, inst, name, wide, 1, 32) : 0;
    if (!h) {
        w32_image im = { 0, 0, 0 };
        int hx = 0, hy = 0;
        if (w32_stock_cursor((int)name, &im, &hx, &hy))
            h = w32_gdi_make_icon(im.w, im.h, im.px, hx, hy);
    }
    RET(h);
}
static void load_icon(w32 *w, int wide) {
    uint64_t inst = ARG(0), name = ARG(1);
    uint64_t h = inst ? load_icon_res(w, inst, name, wide, 0, 32) : 0;
    if (!h) {
        w32_image im = { 0, 0, 0 };
        if (w32_stock_icon((int)name, 32, &im))
            h = w32_gdi_make_icon(im.w, im.h, im.px, 0, 0);
    }
    RET(h);
}
static void u_LoadCursorA(w32 *w) { load_cursor(w, 0); }
static void u_LoadCursorW(w32 *w) { load_cursor(w, 1); }
static void u_LoadIconA(w32 *w) { load_icon(w, 0); }
static void u_LoadIconW(w32 *w) { load_icon(w, 1); }

/* Which cursor is showing. The pointer is drawn by the compositor at the end
 * of a repaint (see draw_cursor), so setting one is just remembering it. */
static void u_SetCursor(w32 *w) {
    pthread_mutex_lock(&g_lock);
    uint64_t old = g_cursor;
    g_cursor = ARG(0);
    int changed = old != g_cursor;
    pthread_mutex_unlock(&g_lock);
    if (changed) cursor_damaged();
    RET(old);
}
static void u_GetCursor(w32 *w) {
    pthread_mutex_lock(&g_lock); uint64_t c = g_cursor; pthread_mutex_unlock(&g_lock);
    RET(c);
}
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
static void u_MonitorFromPoint(w32 *w) { (void)w; RET(0x9101); }   /* POINT by value; the answer does not depend on it */
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
/* Something drew. That is activity, so the idle pacing above stands down --
 * a program repainting is working, not spinning. */
void w32_desktop_damaged(void) { g_surf_dirty = 1; g_surf_live = 1; w32_note_activity(); }

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
    /* An open menu and the mouse pointer go in here and come straight back
     * out: they are above every window, so they cannot be painted before the
     * windows are, and they must not still be in the surface when the next
     * window paints into it. The callback has the pixels by the time it
     * returns -- it writes a .ppm or fills a texture -- so undoing this
     * immediately is safe and is what keeps the pointer from smearing. */
    overlay_compose();
    if (fn && b) fn(ctx, b, cx, cy, cx * 4);
    overlay_restore();
}
/* For a test, and for the probe: the frame as it stands. */
const uint32_t *w32_desktop_peek(int *cx, int *cy) { return w32_desktop_bits(cx, cy); }

/* Where a window's client area sits on the screen. `whole` asks for the
 * window rectangle instead, which is the same thing here -- there are no
 * decorations to subtract. */
/* The client area of this window's parent, if it has one. What a child is
 * allowed to draw inside. */
int w32_window_parent_area(uint64_t hwnd, int *x, int *y, int *cx, int *cy) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    wwin *pp = p ? win_of(p->parent) : 0;
    int ok = pp != 0;
    if (ok) {
        *x = pp->x + pp->cxo; *y = pp->y + pp->cyo;
        *cx = pp->cw; *cy = pp->ch;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

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
    /* Every RichEdit version there has ever been -- RichEdit, RichEdit20A/W,
     * RICHEDIT50W, RichEdit60W -- is drawn as a multi-line edit box. It is
     * not a rich text control and the formatting is dropped, but the *text*
     * appears, and an installer whose licence page was a blank white
     * rectangle now shows the licence. */
    if (!strncasecmp(cls, "richedit", 8)) return CTL_EDIT;
    if (!strcasecmp(cls, "edit")) return CTL_EDIT;
    if (!strcasecmp(cls, "listbox")) return CTL_LISTBOX;
    if (!strcasecmp(cls, "combobox")) return CTL_COMBOBOX;
    if (!strcasecmp(cls, "msctls_progress32")) return CTL_PROGRESS;
    if (!strcasecmp(cls, "syslistview32"))     return CTL_LISTVIEW;
    if (!strcasecmp(cls, "systreeview32"))     return CTL_TREEVIEW;
    if (!strcasecmp(cls, "systabcontrol32"))   return CTL_TAB;
    if (!strcasecmp(cls, "msctls_statusbar32"))return CTL_STATUS;
    if (!strcasecmp(cls, "msctls_trackbar32")) return CTL_TRACK;
    if (!strcasecmp(cls, "msctls_updown32"))   return CTL_UPDOWN;
    if (!strcasecmp(cls, "toolbarwindow32"))   return CTL_TOOLBAR;
    if (!strcasecmp(cls, "sysheader32"))       return CTL_HEADER;
    if (!strcasecmp(cls, "syslink"))           return CTL_LINK;
    if (!strcasecmp(cls, "comboboxex32"))      return CTL_COMBOBOX;
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
/* --- visual styles ------------------------------------------------------
 *
 * Windows has drawn its controls two ways since 2001. A program without a
 * manifest gets the grey 3D bevels of Windows 95; one with a comctl32 version
 * 6 dependency in its manifest gets the themed look -- flat fills, a thin
 * outline, a soft gradient, a blue edge on whatever has the keyboard. Every
 * program built in the last twenty years asks for the second, so drawing only
 * the first is why screenshots of this looked two decades old.
 *
 * The theme is not read from a .msstyles file: that is a PE full of bitmaps
 * belonging to whoever's Windows it came from, and it is not ours to ship.
 * What is drawn here is the *shape* of the modern look -- borders, gradients
 * and highlight colours -- which is what makes it read as current.
 */
static int g_themed = 1;
int w32_themes_enabled(void) { return g_themed; }
void w32_set_themes(int on) { g_themed = on != 0; }

/* A vertical gradient. Two fills would band visibly across a button, so this
 * is one row at a time with the interpolation done in integers -- the same
 * arithmetic on every machine, which matters because these pixels end up in
 * a frame checksum. */
static void gradient_v(uint64_t hdc, int l, int t, int r, int b,
                       uint32_t top, uint32_t bot) {
    int h = b - t;
    if (h <= 0 || r <= l) return;
    if (h == 1) { w32_gdi_fill_rect(hdc, l, t, r, b, top); return; }
    for (int y = 0; y < h; y++) {
        unsigned rr = (((top >> 16) & 0xFF) * (h - 1 - y) + ((bot >> 16) & 0xFF) * y) / (h - 1);
        unsigned gg = (((top >>  8) & 0xFF) * (h - 1 - y) + ((bot >>  8) & 0xFF) * y) / (h - 1);
        unsigned bb = (((top      ) & 0xFF) * (h - 1 - y) + ((bot      ) & 0xFF) * y) / (h - 1);
        w32_gdi_fill_rect(hdc, l, t + y, r, t + y + 1, rr << 16 | gg << 8 | bb);
    }
}

/* A rectangle with its four corner pixels left out. Not a real rounded
 * corner -- at these sizes a real one is a corner pixel and a lighter
 * neighbour -- but it is the difference between a control that looks drawn
 * for this decade and one that does not. */
static void rounded_frame(uint64_t hdc, int l, int t, int r, int b, uint32_t c) {
    if (r - l < 3 || b - t < 3) { w32_gdi_frame_rect(hdc, l, t, r, b, c); return; }
    w32_gdi_fill_rect(hdc, l + 1, t, r - 1, t + 1, c);
    w32_gdi_fill_rect(hdc, l + 1, b - 1, r - 1, b, c);
    w32_gdi_fill_rect(hdc, l, t + 1, l + 1, b - 1, c);
    w32_gdi_fill_rect(hdc, r - 1, t + 1, r, b - 1, c);
}

/* The 3D edge, or the themed equivalent. Every control's outline goes through
 * here, so switching the look is one branch rather than fifty. */
static void bevel(uint64_t hdc, int l, int t, int r, int b, int sunken) {
    if (g_themed) {
        /* Themed: one thin outline, darker for a sunken field than for a
         * raised one, and no white highlight -- the highlight is what makes
         * the old look old. */
        rounded_frame(hdc, l, t, r, b, sunken ? 0x9A8E85u : 0xACA39Au);
        return;
    }
    uint32_t tl = sunken ? sys_color(COLOR_BTNSHADOW) : 0xFFFFFF;
    uint32_t br = sunken ? 0xFFFFFF : sys_color(COLOR_3DDKSHADOW);
    w32_gdi_fill_rect(hdc, l, t, r, t + 1, tl);
    w32_gdi_fill_rect(hdc, l, t, l + 1, b, tl);
    w32_gdi_fill_rect(hdc, l, b - 1, r, b, br);
    w32_gdi_fill_rect(hdc, r - 1, t, r, b, br);
}

/* The face of a push button, in whichever look is in force. `state` is 0
 * normal, 1 pressed, 2 focused or default. */
static void button_face(uint64_t hdc, int l, int t, int r, int b, int state) {
    if (!g_themed) {
        w32_gdi_fill_rect(hdc, l, t, r, b, sys_color(COLOR_BTNFACE));
        bevel(hdc, l, t, r, b, state == 1);
        return;
    }
    /* The colours are the ones the Aero button actually uses: a near-white
     * top falling to a light grey, inverted while pressed, and a blue edge
     * when the button is the one Enter would press. */
    uint32_t top = state == 1 ? 0xE0E4E8u : 0xFDFEFFu;
    uint32_t bot = state == 1 ? 0xF0F3F5u : 0xE3E9EFu;
    gradient_v(hdc, l + 1, t + 1, r - 1, b - 1, top, bot);
    rounded_frame(hdc, l, t, r, b, state == 2 ? 0xC08040u : 0x9A8E85u);
    if (state == 2) rounded_frame(hdc, l + 1, t + 1, r - 1, b - 1, 0xE8C89Cu);
}

/* Text inside a rectangle, with the three alignments a control uses and
 * wrapping on word boundaries when it does not fit. Returns the height used,
 * which is what DrawText with DT_CALCRECT reports. */
enum { DT_LEFT = 0, DT_CENTER = 1, DT_RIGHT = 2, DT_VCENTER = 4, DT_BOTTOM = 8,
       DT_WORDBREAK = 0x10, DT_SINGLELINE = 0x20, DT_NOCLIP = 0x100,
       DT_CALCRECT = 0x400, DT_NOPREFIX = 0x800, DT_END_ELLIPSIS = 0x8000 };

/* How many characters of `s` fit in `avail` pixels. Measured, not divided:
 * the font is proportional, so "how many characters fit" has no answer that
 * does not involve looking at which characters they are. */
static int chars_that_fit(uint64_t hdc, const char *s, int len, int avail) {
    int lo = 0, hi = len;
    /* The widths are monotonic in the prefix length, so a binary search is
     * exact and costs six measurements instead of one per character. */
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        int w = 0, dummy = 0;
        w32_gdi_text_extent(hdc, s, mid, &w, &dummy);
        if (w <= avail) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static int draw_text_rect(uint64_t hdc, const char *s, int len,
                          int l, int t, int r, int b, uint32_t fmt) {
    int lh = w32_gdi_line_height(hdc);
    int width = r - l;
    if (width < 1) width = 1;
    /* Measure first, so DT_VCENTER and DT_CALCRECT know the height before
     * anything is drawn. Two passes over a label is nothing, and getting the
     * height from a single pass means drawing in the wrong place first. */
    /* Room for a licence agreement. Sixty-four lines was enough for a label
     * and a message box and nothing else -- the first page of an installer is
     * hundreds, and the ones past the limit were silently dropped, which is
     * both a wrong height for the scroll bar and missing text. */
    enum { MAX_LINES = 2048 };
    int lines = 0, i = 0;
    static int starts[MAX_LINES], lens[MAX_LINES];
    while (i < len && lines < MAX_LINES) {
        int take_n = len - i, hard = 0;
        for (int k = 0; k < take_n; k++) if (s[i + k] == '\n') { take_n = k; hard = 1; break; }
        if (!(fmt & DT_SINGLELINE)) {
            int fits = chars_that_fit(hdc, s + i, take_n, width);
            if (fits < take_n) {
                if (fmt & DT_WORDBREAK) {
                    /* Break on the last space that fits, so a word is not cut
                     * in half. A single word longer than the line has no such
                     * space, and is broken where it runs out. */
                    int cut = fits;
                    while (cut > 0 && s[i + cut] != ' ') cut--;
                    take_n = cut > 0 ? cut : (fits > 0 ? fits : 1);
                } else if (!(fmt & DT_NOCLIP)) {
                    take_n = fits > 0 ? fits : 1;
                }
            }
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
        int wpx = 0, dummy = 0;
        w32_gdi_text_extent(hdc, s + starts[k], lens[k], &wpx, &dummy);
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

/* The icon to show in a window's title bar: the one it was given by
 * WM_SETICON if any, otherwise its class's. Both are ordinary image handles
 * now that LoadIcon returns real pixels, so the caption can just draw it. */
static uint64_t window_icon(uint64_t hwnd) {
    uint64_t h = 0;
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p) {
        h = p->icon;
        if (!h) {
            wclass *c = class_of(p->cls);
            if (c) h = c->icon;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return h;
}

/* A check mark, drawn as two strokes rather than a glyph so it scales with
 * the box it sits in. Shared by the check box and the list view. */
static void draw_check(uint64_t hdc, int x, int y, int size, uint32_t colour) {
    if (size < 5) size = 5;
    int t = size >= 16 ? 2 : 1;
    for (int i = 0; i < size / 3; i++)
        w32_gdi_fill_rect(hdc, x + 2 + i, y + size / 2 + i, x + 2 + i + t,
                          y + size / 2 + i + t, colour);
    for (int i = 0; i < size * 2 / 3; i++)
        w32_gdi_fill_rect(hdc, x + 2 + size / 3 + i, y + size / 2 + size / 3 - i,
                          x + 2 + size / 3 + i + t, y + size / 2 + size / 3 - i + t, colour);
}

/* One end of a scroll bar: a raised box with a triangle in it. */
static void arrow_box(uint64_t hdc, int x, int y, int cw, int ch, int down) {
    w32_gdi_fill_rect(hdc, x, y, x + cw, y + ch, sys_color(COLOR_BTNFACE));
    bevel(hdc, x, y, x + cw, y + ch, 0);
    uint32_t k = sys_color(COLOR_BTNTEXT);
    int cx = x + cw / 2, cy = y + ch / 2;
    for (int i = 0; i < 4; i++) {
        int yy = down ? cy - 2 + i : cy + 2 - i;
        w32_gdi_fill_rect(hdc, cx - i, yy, cx + i + 1, yy + 1, k);
    }
}

/* A SysLink's caption is HTML-ish: "read the <a href=x>licence</a> first".
 * The markup is removed for drawing; `link_start` and `link_len` come back so
 * a click can be tested against the part that is actually a link. */
static int strip_link(const char *in, char *out, int cap, int *link_start, int *link_len) {
    int n = 0, ls = -1, le = 0;
    for (int i = 0; in[i] && n + 1 < cap; i++) {
        if (in[i] == '<') {
            int close = in[i + 1] == '/';
            if (!close && ls < 0) ls = n;
            if (close) le = n;
            while (in[i] && in[i] != '>') i++;
            continue;
        }
        out[n++] = in[i];
    }
    out[n] = 0;
    if (link_start) *link_start = ls < 0 ? 0 : ls;
    if (link_len) *link_len = ls < 0 ? n : (le > ls ? le - ls : n - ls);
    return n;
}

/* How much text there turned out to be, told to the window by its own
 * painter. A scroll bar cannot be drawn correctly before the text has been
 * laid out, and laying it out is what painting does. */
static void edit_extent(uint64_t hwnd, int over, int page, int line) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p) {
        p->scroll_max = over;
        p->scroll_page = page;
        p->step = line > 0 ? line : 13;
        if (p->scroll_pos > over) p->scroll_pos = over;
        if (p->scroll_pos < 0) p->scroll_pos = 0;
    }
    pthread_mutex_unlock(&g_lock);
}

/* ---- menus, drawn -------------------------------------------------------
 *
 * Two shapes, and they share their measuring: a bar across the top of a
 * window, and a popup dropped below one of its items or put wherever
 * TrackPopupMenu was told to put it. Both are laid out here and nowhere else,
 * because a hit test that measures items differently from the painter is a
 * menu where clicking one item runs another -- and that failure looks like a
 * bug in the program rather than in this file.
 */
enum { MENU_PAD = 3, MENU_SEP_H = 7, MENU_MIN_W = 80 };
/* The pale blue a themed menu highlights with, as a COLORREF -- which is
 * 0x00BBGGRR, so this is RGB(204, 232, 255) written the way GDI takes it and
 * not the other way round. Getting that backwards is a highlight that comes
 * out peach, which is exactly what the first version of this did. */
enum { MENU_HILITE = 0xFFE8CCu };

static int menu_item_h(const mitem *it, int lh) {
    return (it->flags & MF_SEPARATOR) ? MENU_SEP_H : lh + 6;
}

/* Where each bar item sits, left to right. */
static int bar_layout(uint64_t hdc, const wmenu *m, int *xs, int *ws) {
    int x = 2;
    for (int i = 0; i < m->n; i++) {
        char lab[MENU_TEXT];
        int ln = strip_amp(m->it[i].text, lab, sizeof lab);
        int tw = 0, th = 0;
        if (ln) w32_gdi_text_extent(hdc, lab, ln, &tw, &th);
        xs[i] = x;
        ws[i] = tw + 16;
        x += ws[i];
    }
    return m->n;
}

/* An item's caption is two strings, not one: "Save\tCtrl+S" is a name and the
 * shortcut it is drawn beside, right-aligned in its own column. Splitting
 * them here rather than at each of the two places that need it keeps the
 * measuring and the drawing agreeing about how wide an item is. */
static int menu_split(const mitem *it, char *lab, int cap, char *acc, int acap) {
    char all[MENU_TEXT];
    int n = strip_amp(it->text, all, sizeof all);
    acc[0] = 0;
    for (int i = 0; i < n; i++)
        if (all[i] == '\t') {
            all[i] = 0;
            snprintf(acc, (size_t)acap, "%s", all + i + 1);
            n = i;
            break;
        }
    snprintf(lab, (size_t)cap, "%s", all);
    return n;
}

/* How big a popup has to be to hold what is in it. The left gutter is the
 * check-mark column: reserving it whether or not anything in the menu is
 * checked is what stops the text jumping sideways the first time something
 * is ticked. */
static void popup_measure(uint64_t hdc, const wmenu *m, int lh, int *pw, int *ph) {
    int wid = 0, accw = 0, h = MENU_PAD * 2;
    for (int i = 0; i < m->n; i++) {
        char lab[MENU_TEXT], acc[MENU_TEXT];
        int ln = menu_split(&m->it[i], lab, sizeof lab, acc, sizeof acc);
        int tw = 0, th = 0;
        if (ln) w32_gdi_text_extent(hdc, lab, ln, &tw, &th);
        if (m->it[i].flags & MF_POPUP) tw += lh;
        if (tw > wid) wid = tw;
        if (acc[0]) {
            int aw = 0;
            w32_gdi_text_extent(hdc, acc, (int)strlen(acc), &aw, &th);
            if (aw > accw) accw = aw;
        }
        h += menu_item_h(&m->it[i], lh);
    }
    int w = lh + 6 + wid + (accw ? accw + 16 : 0) + 16;
    *pw = w < MENU_MIN_W ? MENU_MIN_W : w;
    *ph = h;
}

/* Which item a point inside a popup is over, or -1. Separators and greyed
 * items are still reported: the highlight has to move over them the way the
 * pointer does, and refusing them is the picker's job, not the hit test's. */
static int popup_item_at(const wmenu *m, int lh, int ry) {
    int y = MENU_PAD;
    for (int i = 0; i < m->n; i++) {
        int h = menu_item_h(&m->it[i], lh);
        if (ry >= y && ry < y + h) return i;
        y += h;
    }
    return -1;
}
/* The top of one item within its popup, which is where a submenu drops from. */
static int popup_item_top(const wmenu *m, int lh, int idx) {
    int y = MENU_PAD;
    for (int i = 0; i < idx && i < m->n; i++) y += menu_item_h(&m->it[i], lh);
    return y;
}

/* A small right-pointing triangle: the mark that says an item opens another
 * menu rather than doing something. Tall on its left edge and a point on its
 * right, which is the way round that means "there is more over here". */
static void submenu_arrow(uint64_t hdc, int x, int y, int size, uint32_t colour) {
    int half = size / 2;
    if (half < 2) half = 2;
    for (int i = 0; i <= half; i++)
        w32_gdi_fill_rect(hdc, x + i, y + i, x + i + 1, y + 2 * half - i, colour);
}

/* The bar, drawn into a device context over the whole window. `hot` is the
 * item whose popup is open, or -1. */
static void paint_menu_bar(uint64_t hdc, const wmenu *m, int top, int width, int height, int hot) {
    if (g_themed) gradient_v(hdc, 0, top, width, top + height, 0xF7F7F7u, 0xEDEDEDu);
    else w32_gdi_fill_rect(hdc, 0, top, width, top + height, sys_color(COLOR_MENU));
    w32_gdi_fill_rect(hdc, 0, top + height - 1, width, top + height, 0xC8C8C8u);
    int xs[MENU_ITEMS], ws[MENU_ITEMS];
    int n = bar_layout(hdc, m, xs, ws);
    int th = w32_gdi_line_height(hdc);
    for (int i = 0; i < n; i++) {
        char lab[MENU_TEXT];
        int ln = strip_amp(m->it[i].text, lab, sizeof lab);
        int greyed = (m->it[i].flags & (MF_GRAYED | MF_DISABLED)) != 0;
        if (i == hot) {
            /* The open item is drawn as a continuation of the popup below it,
             * which is what tells you which one you opened. */
            if (g_themed) w32_gdi_fill_rect(hdc, xs[i], top + 1, xs[i] + ws[i], top + height, MENU_HILITE);
            else w32_gdi_fill_rect(hdc, xs[i], top + 1, xs[i] + ws[i], top + height, sys_color(COLOR_HIGHLIGHT));
        }
        w32_gdi_set_text_color(hdc, greyed ? sys_color(COLOR_GRAYTEXT)
                               : (i == hot && !g_themed) ? sys_color(COLOR_HIGHLIGHTTEXT)
                               : sys_color(COLOR_MENUTEXT));
        w32_gdi_text_at(hdc, xs[i] + 8, top + (height - th) / 2, lab, ln);
    }
}

/* One popup, drawn straight onto the screen surface at an absolute position:
 * a menu is above every window, so it does not go through a window's DC. */
static void paint_popup(uint64_t hdc, const wmenu *m, int x, int y, int pw, int ph, int lh, int hot) {
    if (g_themed) {
        w32_gdi_fill_rect(hdc, x, y, x + pw, y + ph, 0xF7F7F7u);
        w32_gdi_frame_rect(hdc, x, y, x + pw, y + ph, 0xA0A0A0u);
    } else {
        w32_gdi_fill_rect(hdc, x, y, x + pw, y + ph, sys_color(COLOR_MENU));
        bevel(hdc, x, y, x + pw, y + ph, 0);
    }
    int gut = lh + 6;
    int iy = y + MENU_PAD;
    for (int i = 0; i < m->n; i++) {
        const mitem *it = &m->it[i];
        int h = menu_item_h(it, lh);
        if (it->flags & MF_SEPARATOR) {
            w32_gdi_fill_rect(hdc, x + gut, iy + h / 2, x + pw - 4, iy + h / 2 + 1, 0xC0C0C0u);
            iy += h;
            continue;
        }
        int greyed = (it->flags & (MF_GRAYED | MF_DISABLED)) != 0;
        if (i == hot && !greyed) {
            if (g_themed) w32_gdi_fill_rect(hdc, x + 2, iy, x + pw - 2, iy + h, MENU_HILITE);
            else w32_gdi_fill_rect(hdc, x + 2, iy, x + pw - 2, iy + h, sys_color(COLOR_HIGHLIGHT));
        }
        uint32_t fg = greyed ? sys_color(COLOR_GRAYTEXT)
                    : (i == hot && !g_themed) ? sys_color(COLOR_HIGHLIGHTTEXT)
                    : sys_color(COLOR_MENUTEXT);
        if (it->flags & MF_CHECKED)
            draw_check(hdc, x + 4, iy + (h - lh) / 2, lh, fg);
        char lab[MENU_TEXT], acc[MENU_TEXT];
        int ln = menu_split(it, lab, sizeof lab, acc, sizeof acc);
        w32_gdi_set_text_color(hdc, fg);
        w32_gdi_text_at(hdc, x + gut, iy + (h - lh) / 2, lab, ln);
        if (acc[0]) {
            int aw = 0, ah = 0;
            w32_gdi_text_extent(hdc, acc, (int)strlen(acc), &aw, &ah);
            w32_gdi_text_at(hdc, x + pw - 12 - aw, iy + (h - lh) / 2, acc, (int)strlen(acc));
        }
        if (it->flags & MF_POPUP)
            submenu_arrow(hdc, x + pw - 12, iy + (h - lh) / 2 + 2, lh - 4, fg);
        iy += h;
    }
}

/* ---- the overlay: what is drawn on top of every window -------------------
 *
 * An open menu and the mouse pointer are both above every window and inside
 * nobody's client area, so neither can go through a window's device context:
 * the next thing to paint would draw straight over them. They are composited
 * into the surface just before it is handed to the display and lifted out
 * again the moment it has been, so the surface a window paints into only ever
 * holds what the windows drew.
 *
 * Leaving them in and repainting what was underneath when they move is the
 * other way, and it is the way a compositor with a backing store per window
 * would do it. There is one shared surface here and no backing store, so
 * "repaint what was under the pointer" means asking every window that
 * overlaps it to paint itself again -- a guest callback for every pixel of
 * mouse travel. Saving the pixels and putting them back costs one
 * rectangle-sized copy and cannot leave a trail at all.
 */
typedef struct { uint32_t *px; size_t cap; int x, y, w, h, sw, sh; } saveunder;
static saveunder g_save_menu, g_save_cur;

/* The surface is reached through w32_desktop_bits, which takes g_lock, so
 * neither of these may be called with it held. */
static void save_under(saveunder *s, int x, int y, int w, int h) {
    s->w = 0;
    int sw = 0, sh = 0;
    uint32_t *b = w32_desktop_bits(&sw, &sh);
    if (!b) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;
    size_t need = (size_t)w * (size_t)h;
    if (need > s->cap) {
        uint32_t *n = realloc(s->px, need * sizeof *n);
        if (!n) return;
        s->px = n; s->cap = need;
    }
    for (int j = 0; j < h; j++)
        memcpy(s->px + (size_t)j * w, b + (size_t)(y + j) * sw + x, (size_t)w * 4);
    s->x = x; s->y = y; s->w = w; s->h = h; s->sw = sw; s->sh = sh;
}
static void restore_under(saveunder *s) {
    if (s->w <= 0 || !s->px) { s->w = 0; return; }
    int sw = 0, sh = 0;
    uint32_t *b = w32_desktop_bits(&sw, &sh);
    /* A screen-size change between the save and the restore reallocates the
     * surface, and putting old pixels back at old coordinates would be
     * scribbling on a stranger. */
    if (b && sw == s->sw && sh == s->sh)
        for (int j = 0; j < s->h; j++)
            memcpy(b + (size_t)(s->y + j) * sw + s->x, s->px + (size_t)j * s->w, (size_t)s->w * 4);
    s->w = 0;
}

/* The arrow to draw when the program has never set a cursor. Made once and
 * kept: it is drawn by image.c rather than decoded from anybody's resources,
 * and a program that destroys its own LoadCursor handle must not be able to
 * take the system's pointer with it. */
static uint64_t stock_arrow(void) {
    static uint64_t arrow;
    if (!arrow) {
        w32_image im = { 0, 0, 0 };
        int hx = 0, hy = 0;
        if (w32_stock_cursor(32512 /* IDC_ARROW */, &im, &hx, &hy))
            arrow = w32_gdi_make_icon(im.w, im.h, im.px, hx, hy);
    }
    return arrow;
}

/* The mouse pointer.
 *
 * Drawn at the cursor position less its hot spot, because the hot spot is the
 * pixel the coordinates refer to -- an arrow's tip, a cross-hair's centre --
 * and ignoring it puts the picture a dozen pixels away from what is being
 * clicked.
 *
 * ShowCursor's counter is honoured, and that is not a nicety: a game that
 * hides the pointer is saying it will draw its own, and drawing this one as
 * well gives it two.
 *
 * This is one half of a decision and the iOS app holds the other. The app
 * draws its own pointer overlay on top of the frame, for touch: a finger
 * covers what it is aiming at, so there has to be something on screen that
 * says where the tap will land. That one and this one must never both be
 * visible -- two arrows a few pixels apart are worse than none, because
 * neither is obviously the real one. This is the pointer a program *sets*
 * and a mouse moves; the app's is the pointer a finger drags. The app asks
 * w32_cursor_visible() before drawing its own, and when a real mouse or
 * trackpad is attached it must leave the drawing to this. */
static void draw_cursor(void) {
    pthread_mutex_lock(&g_lock);
    int shown = g_cursor_shown, x = g_mx, y = g_my;
    uint64_t cur = g_cursor;
    pthread_mutex_unlock(&g_lock);
    if (!shown) return;
    int cx = 0, cy = 0, hx = 0, hy = 0;
    /* A handle the program has since destroyed measures as nothing, and
     * "no pointer at all" is a worse answer than the system arrow. */
    if (!cur || !w32_gdi_icon_size(cur, &cx, &cy, &hx, &hy)) {
        cur = stock_arrow();
        if (!cur || !w32_gdi_icon_size(cur, &cx, &cy, &hx, &hy)) return;
    }
    save_under(&g_save_cur, x - hx, y - hy, cx, cy);
    uint64_t hdc = w32_dc_for_window(0, 0, 1);
    if (!hdc) return;
    w32_gdi_draw_image(hdc, cur, x - hx, y - hy, cx, cy);
    w32_dc_release(hdc);
}

static void overlay_compose(void) {
    /* The menu first and the pointer last, because the pointer is over
     * everything -- including the menu it is choosing from. */
    struct { uint64_t menu; int x, y, w, h, hot; } lv[MENU_DEPTH];
    int n, lx = 0, ty = 0, rx = 0, by = 0;
    pthread_mutex_lock(&g_lock);
    n = g_pop.n > MENU_DEPTH ? MENU_DEPTH : g_pop.n;
    for (int i = 0; i < n; i++) {
        lv[i].menu = g_pop.lv[i].menu;
        lv[i].x = g_pop.lv[i].x; lv[i].y = g_pop.lv[i].y;
        lv[i].w = g_pop.lv[i].w; lv[i].h = g_pop.lv[i].h;
        lv[i].hot = g_pop.lv[i].hot;
        if (!i) { lx = lv[i].x; ty = lv[i].y; rx = lx + lv[i].w; by = ty + lv[i].h; }
        else {
            if (lv[i].x < lx) lx = lv[i].x;
            if (lv[i].y < ty) ty = lv[i].y;
            if (lv[i].x + lv[i].w > rx) rx = lv[i].x + lv[i].w;
            if (lv[i].y + lv[i].h > by) by = lv[i].y + lv[i].h;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (n > 0) {
        /* One saved rectangle for all the levels together. A cascade is
         * contiguous by construction, so their union is barely bigger than
         * the popups themselves. */
        save_under(&g_save_menu, lx, ty, rx - lx, by - ty);
        uint64_t hdc = w32_dc_for_window(0, 0, 1);
        if (hdc) {
            w32_gdi_set_bk_mode(hdc, 1);
            int lh = w32_gdi_line_height(hdc);
            if (lh < 1) lh = 13;
            for (int i = 0; i < n; i++) {
                wmenu m;
                if (menu_snapshot(lv[i].menu, &m))
                    paint_popup(hdc, &m, lv[i].x, lv[i].y, lv[i].w, lv[i].h, lh, lv[i].hot);
            }
            w32_dc_release(hdc);
        }
    }
    draw_cursor();
}

/* Reverse order: the pointer was saved after the menu was drawn, so its
 * saved pixels are the menu's and have to go back first. */
static void overlay_restore(void) {
    restore_under(&g_save_cur);
    restore_under(&g_save_menu);
}

/* ---- painting one control ---------------------------------------------- */

/* The window frame: the title bar, its icon and close box, the menu bar, and
 * the border. Drawn for every top-level window that asked for one, whatever
 * its class.
 *
 * It lives out here rather than inside paint_control because paint_control is
 * only reached for a window this file draws the *inside* of. A program that
 * registers its own class -- a Delphi form, an MFC frame, most installers --
 * paints its own client area and expects the system to paint the frame around
 * it, exactly as Windows does. Leaving it to paint_control is why one of them
 * came up with a blank blue strip where its title should have been. */
static void paint_chrome(w32 *w, uint64_t hwnd, wwin *snap) {
    if (snap->cyo) {
        uint64_t wdc = w32_dc_for_window(w, hwnd, 1);
        if (wdc) {
            /* cyo is the caption and the menu bar together, so how much of it
             * belongs to which has to be worked out again here rather than
             * assumed -- a window with a bar and no caption has one and not
             * the other, and drawing the title gradient over the whole of cyo
             * would put the bar inside the title bar. */
            int cap_h = (!(snap->style & WS_CHILD) && (snap->style & WS_CAPTION) == WS_CAPTION)
                        ? caption_height() : 0;
            if (cap_h > snap->cyo) cap_h = snap->cyo;
            if (snap->font) w32_gdi_set_font(wdc, snap->font);
            w32_gdi_set_bk_mode(wdc, 1);
            if (cap_h) {
                if (g_themed)
                    gradient_v(wdc, 0, 0, snap->w, cap_h, 0xF2F6FBu, 0xD3DEEBu);
                else
                    w32_gdi_fill_rect(wdc, 0, 0, snap->w, cap_h, sys_color(COLOR_ACTIVECAPTION));
                w32_gdi_set_text_color(wdc, g_themed ? 0x3C3C3Cu : sys_color(COLOR_CAPTIONTEXT));
                int th = w32_gdi_line_height(wdc);
                int tx = 6;
                uint64_t icon = window_icon(hwnd);
                if (icon) {
                    int isz = cap_h - 6;
                    if (isz > 4) {
                        w32_gdi_draw_image(wdc, icon, 4, 3, isz, isz);
                        tx = 6 + isz + 4;
                    }
                }
                /* The close box. It is drawn rather than made a real button
                 * because there is no window manager here to own one, and a
                 * program that watches for WM_CLOSE gets it from the hit test. */
                int bs = cap_h - 8;
                if (bs > 6 && snap->w > bs + 12) {
                    int bx = snap->w - bs - 4, by = 4;
                    w32_gdi_fill_rect(wdc, bx, by, bx + bs, by + bs, sys_color(COLOR_BTNFACE));
                    uint32_t xk = sys_color(COLOR_BTNTEXT);
                    for (int i = 3; i < bs - 3; i++) {
                        w32_gdi_fill_rect(wdc, bx + i, by + i, bx + i + 1, by + i + 1, xk);
                        w32_gdi_fill_rect(wdc, bx + bs - 1 - i, by + i, bx + bs - i, by + i + 1, xk);
                    }
                }
                w32_gdi_text_at(wdc, tx, (cap_h - th) / 2, snap->text, (int)strlen(snap->text));
            }
            /* The menu bar fills whatever of cyo the caption did not. */
            if (snap->cyo > cap_h) {
                wmenu m;
                if (menu_snapshot(snap->menu, &m)) {
                    int hot = -1;
                    pthread_mutex_lock(&g_lock);
                    if (g_pop.n && g_pop.barwnd == hwnd) hot = g_pop.baritem;
                    pthread_mutex_unlock(&g_lock);
                    paint_menu_bar(wdc, &m, cap_h, snap->w, snap->cyo - cap_h, hot);
                }
            }
            w32_gdi_frame_rect(wdc, 0, 0, snap->w, snap->h, sys_color(COLOR_3DDKSHADOW));
            w32_dc_release(wdc);
        }
    }
}

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

    /* The title bar, for any window that asked for one -- drawn on a device
     * context over the *whole* window, because the client one cannot reach
     * above its own origin, which is where the caption is.
     *
     * The icon goes in it if the program gave its class one, and the close
     * box on the right, because a title bar without either reads as a
     * placeholder rather than a window. */

    switch (snap->ctl) {
    case CTL_DIALOG:
        w32_gdi_fill_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_BTNFACE));
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
                w32_gdi_text_extent(hdc, label, ln, &tw, &th);
                w32_gdi_fill_rect(hdc, 6, top - 1, 6 + tw + 6, top + 1, sys_color(COLOR_BTNFACE));
                w32_gdi_text_at(hdc, 9, 0, label, ln);
            }
            break;
        }
        if (kind == BS_CHECKBOX || kind == BS_AUTOCHECKBOX ||
            kind == BS_3STATE || kind == BS_AUTO3STATE ||
            kind == BS_RADIOBUTTON || kind == BS_AUTORADIOBUTTON) {
            /* Sized from the text beside it. A 13-pixel box next to
             * 24-pixel text looks like a mistake, because it is one. */
            int box = w32_gdi_line_height(hdc);
            if (box > ch) box = ch;
            if (box < 8) box = 8;
            int by = (ch - box) / 2;
            if (g_themed) {
                gradient_v(hdc, 1, by + 1, box - 1, by + box - 1, 0xF6FAFDu, 0xFFFFFFu);
                rounded_frame(hdc, 0, by, box, by + box, snap->focused ? 0xC08040u : 0x8E837Au);
            } else {
                w32_gdi_fill_rect(hdc, 0, by, box, by + box, 0xFFFFFF);
                bevel(hdc, 0, by, box, by + box, 1);
            }
            if (snap->checked) {
                /* A tick for a check box, a dot for a radio button: the two
                 * are different controls and have to look different, because
                 * that is how a user knows one choice from many. */
                uint32_t mark = sys_color(COLOR_BTNTEXT);
                int in = box / 4 < 2 ? 2 : box / 4;
                if (kind == BS_RADIOBUTTON || kind == BS_AUTORADIOBUTTON)
                    w32_gdi_fill_rect(hdc, in, by + in, box - in, by + box - in, mark);
                else {
                    /* A tick, thickened with the box so it does not vanish at
                     * one pixel on a dense display. */
                    int t = box / 8 + 1;
                    for (int k = 0; k < t; k++) {
                        w32_gdi_line(hdc, in, by + box / 2 + k, box / 2 - 1, by + box - in + k - 1, mark);
                        w32_gdi_line(hdc, box / 2 - 1, by + box - in + k - 1, box - in, by + in + k, mark);
                    }
                }
            }
            draw_text_rect(hdc, label, ln, box + box / 3 + 2, 0, cw, ch, DT_LEFT | DT_VCENTER | DT_WORDBREAK);
            break;
        }
        /* A push button. Pressed wins over focused, because a button being
         * held down is the more urgent thing to show. */
        button_face(hdc, 0, 0, cw, ch,
                    snap->pressed ? 1
                    : (snap->focused || kind == BS_DEFPUSHBUTTON) ? 2 : 0);
        if (!g_themed && kind == BS_DEFPUSHBUTTON)
            w32_gdi_frame_rect(hdc, 0, 0, cw, ch, 0x000000);
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
        } else if (snap->style & ES_MULTILINE) {
            /* A multi-line box holds the whole text, which for a licence
             * agreement is far more than fits, so it is drawn from the
             * scroll position down and the box tells the scroll bar how much
             * there is. Measuring the wrapped height with DT_CALCRECT and
             * then drawing is what Windows does, and it is the only way the
             * thumb's size can mean anything. */
            const char *t = win_text(snap);
            int len = (int)strlen(t);
            int inner = cw - 6 - ((snap->style & WS_VSCROLL) ? 16 : 0);
            if (inner < 8) inner = 8;
            int lh = w32_gdi_line_height(hdc);
            if (lh < 1) lh = 13;
            int total = draw_text_rect(hdc, t, len, 3, 2, 3 + inner, ch - 2,
                                       DT_WORDBREAK | DT_CALCRECT);
            int shown = ch - 4;
            /* Report the extent back, so the bar drawn below this switch and
             * the wheel handling both work from the real numbers. */
            edit_extent(hwnd, total > shown ? total - shown : 0, shown, lh);
            draw_text_rect(hdc, t, len, 3, 2 - snap->scroll_pos, 3 + inner,
                           ch - 2, DT_WORDBREAK | DT_NOCLIP);
        } else {
            draw_text_rect(hdc, snap->text, (int)strlen(snap->text), 3, 2, cw - 3, ch - 2,
                           DT_SINGLELINE | DT_VCENTER);
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
        w32_gdi_fill_rect(hdc, 0, 0, cw, ch, g_themed ? 0xF0EDEBu : sys_color(COLOR_WINDOW));
        bevel(hdc, 0, 0, cw, ch, 1);
        int span = snap->hi - snap->lo;
        int done = span > 0 ? (snap->pos - snap->lo) : 0;
        if (done < 0) done = 0;
        if (span > 0 && done > span) done = span;
        int fillw = span > 0 ? (cw - 4) * done / span : 0;
        /* The themed bar is a green gradient with a lighter band across the
         * top, which is the one thing about a progress bar people recognise
         * at a glance. */
        if (fillw > 0) {
            if (g_themed) {
                gradient_v(hdc, 2, 2, 2 + fillw, ch - 2, 0x36C13Au, 0x0A8A18u);
                gradient_v(hdc, 2, 2, 2 + fillw, 2 + (ch - 4) / 3, 0x86E58Cu, 0x36C13Au);
            } else {
                w32_gdi_fill_rect(hdc, 2, 2, 2 + fillw, ch - 2, 0x00A000);
            }
        }
        break;
    }

    case CTL_PANE:
        w32_gdi_fill_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_WINDOW));
        bevel(hdc, 0, 0, cw, ch, 1);
        break;

    /* A list view. Report mode -- a header row and columns -- is what an
     * installer's component page and a game's resolution list both use; list
     * and icon modes fall back to one string per row, which is what those
     * modes look like anyway at this level of detail.
     *
     * The check boxes are not decoration. LVS_EX_CHECKBOXES is how every
     * installer asks which components to install, and a list of components
     * with no way to see or change what is ticked is not usable. */
    case CTL_LISTVIEW: {
        w32_gdi_fill_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_WINDOW));
        bevel(hdc, 0, 0, cw, ch, 1);
        if (snap->list < 0) break;
        int lh = w32_gdi_line_height(hdc) + 4;
        int y = 2;
        int ncol = g_list[snap->list].ncol;
        int checks = (snap->exstyle & LVS_EX_CHECKBOXES_) != 0;
        if (ncol > 0) {
            /* The header. Drawn as buttons, because that is what it is. */
            int x = 2;
            for (int c = 0; c < ncol && x < cw; c++) {
                int cwid = g_list[snap->list].colw[c] > 0 ? g_list[snap->list].colw[c] : 100;
                w32_gdi_fill_rect(hdc, x, y, x + cwid, y + lh, sys_color(COLOR_BTNFACE));
                bevel(hdc, x, y, x + cwid, y + lh, 0);
                w32_gdi_set_text_color(hdc, sys_color(COLOR_BTNTEXT));
                w32_gdi_text_at(hdc, x + 4, y + 2, g_list[snap->list].colname[c],
                                (int)strlen(g_list[snap->list].colname[c]));
                x += cwid;
            }
            y += lh;
        }
        int n = g_list[snap->list].n;
        for (int i = snap->top; i < n && y + lh <= ch - 2; i++) {
            int sel = i == snap->sel;
            if (sel) w32_gdi_fill_rect(hdc, 2, y, cw - 2, y + lh, sys_color(COLOR_HIGHLIGHT));
            int x = 4;
            if (checks) {
                int b = lh - 6 > 8 ? lh - 6 : 8;
                w32_gdi_fill_rect(hdc, x, y + 2, x + b, y + 2 + b, sys_color(COLOR_WINDOW));
                bevel(hdc, x, y + 2, x + b, y + 2 + b, 1);
                if (g_list[snap->list].checked[i])
                    draw_check(hdc, x + 1, y + 3, b - 2, sys_color(COLOR_WINDOWTEXT));
                x += b + 4;
            }
            w32_gdi_set_text_color(hdc, sel ? sys_color(COLOR_HIGHLIGHTTEXT)
                                            : sys_color(COLOR_WINDOWTEXT));
            /* Report mode packs a row as column texts separated by tabs --
             * see the LVM_SETITEMTEXT handler, which builds them that way. */
            const char *it = g_list[snap->list].item[i];
            if (ncol > 0) {
                int c = 0, cx2 = x;
                const char *seg = it;
                while (c < ncol && cx2 < cw) {
                    const char *tab = strchr(seg, '\t');
                    int len = tab ? (int)(tab - seg) : (int)strlen(seg);
                    w32_gdi_text_at(hdc, cx2 + (c ? 4 : 0), y + 2, seg, len);
                    cx2 += g_list[snap->list].colw[c] > 0 ? g_list[snap->list].colw[c] : 100;
                    if (!tab) break;
                    seg = tab + 1; c++;
                }
            } else {
                w32_gdi_text_at(hdc, x, y + 2, it, (int)strlen(it));
            }
            y += lh;
        }
        break;
    }

    /* A tree view, drawn flat. The hierarchy is not modelled -- an item knows
     * its text and nothing else -- so this is a list with room on the left
     * where the expanders would be. That is enough for the two things a tree
     * is used for here, a directory picker and a component list, to be
     * legible rather than blank. */
    case CTL_TREEVIEW: {
        w32_gdi_fill_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_WINDOW));
        bevel(hdc, 0, 0, cw, ch, 1);
        if (snap->list < 0) break;
        int lh = w32_gdi_line_height(hdc) + 2;
        int y = 2, n = g_list[snap->list].n;
        for (int i = snap->top; i < n && y + lh <= ch - 2; i++) {
            int sel = i == snap->sel;
            if (sel) w32_gdi_fill_rect(hdc, 2, y, cw - 2, y + lh, sys_color(COLOR_HIGHLIGHT));
            w32_gdi_set_text_color(hdc, sel ? sys_color(COLOR_HIGHLIGHTTEXT)
                                            : sys_color(COLOR_WINDOWTEXT));
            const char *it = g_list[snap->list].item[i];
            w32_gdi_text_at(hdc, 16, y + 1, it, (int)strlen(it));
            y += lh;
        }
        break;
    }

    /* Tabs across the top, the selected one raised and joined to the page
     * below it. The page itself is a child window the program manages; all
     * this owes it is the strip and a body to sit on. */
    case CTL_TAB: {
        w32_gdi_fill_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_BTNFACE));
        if (snap->list < 0) break;
        int n = g_list[snap->list].n;
        int th = w32_gdi_line_height(hdc) + 8;
        int x = 2;
        for (int i = 0; i < n && x < cw; i++) {
            const char *lab = g_list[snap->list].item[i];
            int tw2 = 0, tht = 0;
            w32_gdi_text_extent(hdc, lab, (int)strlen(lab), &tw2, &tht);
            int wdt = tw2 + 16;
            int sel = i == snap->sel;
            int ty = sel ? 0 : 2;
            w32_gdi_fill_rect(hdc, x, ty, x + wdt, th, sys_color(COLOR_BTNFACE));
            bevel(hdc, x, ty, x + wdt, th + 2, 0);
            w32_gdi_set_text_color(hdc, sys_color(COLOR_BTNTEXT));
            w32_gdi_text_at(hdc, x + 8, ty + 4, lab, (int)strlen(lab));
            x += wdt + 1;
        }
        bevel(hdc, 0, th, cw, ch, 0);
        break;
    }

    /* The status bar along the bottom. Its panes come from SB_SETTEXT, one
     * per part, and a program that only ever sets part 0 gets one pane the
     * width of the window -- which is what most of them do. */
    case CTL_STATUS: {
        w32_gdi_fill_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_BTNFACE));
        if (snap->list < 0) break;
        int n = g_list[snap->list].n;
        if (n < 1) n = 1;
        int pw = cw / n;
        w32_gdi_set_text_color(hdc, sys_color(COLOR_BTNTEXT));
        for (int i = 0; i < n; i++) {
            int x = i * pw;
            bevel(hdc, x + 1, 1, x + pw - 1, ch - 1, 1);
            if (i < g_list[snap->list].n)
                w32_gdi_text_at(hdc, x + 5, 2, g_list[snap->list].item[i],
                                (int)strlen(g_list[snap->list].item[i]));
        }
        break;
    }

    /* A slider. A game's volume and gamma live on these, so the thumb has to
     * be somewhere a finger can find it, and it has to move when dragged --
     * see the WM_LBUTTONDOWN handling in ctl_proc. */
    case CTL_TRACK: {
        w32_gdi_fill_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_BTNFACE));
        int span = snap->hi - snap->lo;
        int vert = ch > cw;
        int track = 4;
        if (vert) {
            int x = cw / 2 - track / 2;
            w32_gdi_fill_rect(hdc, x, 4, x + track, ch - 4, sys_color(COLOR_BTNSHADOW));
            bevel(hdc, x, 4, x + track, ch - 4, 1);
            int pos = span > 0 ? (ch - 20) * (snap->pos - snap->lo) / span : 0;
            w32_gdi_fill_rect(hdc, 2, 4 + pos, cw - 2, 4 + pos + 12, sys_color(COLOR_BTNFACE));
            bevel(hdc, 2, 4 + pos, cw - 2, 4 + pos + 12, 0);
        } else {
            int y = ch / 2 - track / 2;
            w32_gdi_fill_rect(hdc, 4, y, cw - 4, y + track, sys_color(COLOR_BTNSHADOW));
            bevel(hdc, 4, y, cw - 4, y + track, 1);
            int pos = span > 0 ? (cw - 20) * (snap->pos - snap->lo) / span : 0;
            w32_gdi_fill_rect(hdc, 4 + pos, 2, 4 + pos + 12, ch - 2, sys_color(COLOR_BTNFACE));
            bevel(hdc, 4 + pos, 2, 4 + pos + 12, ch - 2, 0);
        }
        break;
    }

    /* A SysLink is a label with <a>...</a> in it. The markup is stripped and
     * the link part drawn the way a link is drawn, which is the entire point
     * of the control; clicking it sends NM_CLICK to the parent. */
    case CTL_LINK: {
        char plain[256];
        int pn = strip_link(snap->text, plain, sizeof plain, 0, 0);
        w32_gdi_set_text_color(hdc, 0xCC6600);            /* COLOR_HOTLIGHT, BGR */
        w32_gdi_text_at(hdc, 0, 0, plain, pn);
        int tw2 = 0, tht = 0;
        w32_gdi_text_extent(hdc, plain, pn, &tw2, &tht);
        w32_gdi_fill_rect(hdc, 0, tht - 1, tw2, tht, 0xCC6600);
        break;
    }

    case CTL_TOOLBAR:
    case CTL_HEADER:
        w32_gdi_fill_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_BTNFACE));
        w32_gdi_fill_rect(hdc, 0, ch - 1, cw, ch, sys_color(COLOR_BTNSHADOW));
        break;

    /* The two little arrows beside a field. */
    case CTL_UPDOWN: {
        w32_gdi_fill_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_BTNFACE));
        bevel(hdc, 0, 0, cw, ch / 2, 0);
        bevel(hdc, 0, ch / 2, cw, ch, 0);
        uint32_t k = sys_color(COLOR_BTNTEXT);
        for (int i = 0; i < 3; i++) {
            w32_gdi_fill_rect(hdc, cw / 2 - i, ch / 4 + i, cw / 2 + i + 1, ch / 4 + i + 1, k);
            w32_gdi_fill_rect(hdc, cw / 2 - i, ch * 3 / 4 - i, cw / 2 + i + 1, ch * 3 / 4 - i + 1, k);
        }
        break;
    }

    default:
        break;
    }

    /* Scroll bars, for anything that asked for one.
     *
     * This is not decoration either: a licence agreement in a read-only edit
     * box is taller than the box, and with no bar there is nothing to say the
     * text continues and nothing to move it with. The thumb's size is the
     * visible fraction, which is the only part of a scroll bar people
     * actually read. */
    if (snap->style & WS_VSCROLL) {
        int sbw = 16;
        int x = cw - sbw;
        if (x > 0) {
            w32_gdi_fill_rect(hdc, x, 0, cw, ch, sys_color(COLOR_BTNFACE));
            arrow_box(hdc, x, 0, sbw, sbw, 0);
            arrow_box(hdc, x, ch - sbw, sbw, sbw, 1);
            int span = ch - 2 * sbw;
            if (span > 8) {
                int total = snap->scroll_max > 0 ? snap->scroll_max : 1;
                int page = snap->scroll_page > 0 ? snap->scroll_page : 1;
                int th2 = span * page / (total + page);
                if (th2 < 8) th2 = 8;
                if (th2 > span) th2 = span;
                int at = total > 0 ? (span - th2) * snap->scroll_pos / total : 0;
                if (at < 0) at = 0;
                if (at > span - th2) at = span - th2;
                w32_gdi_fill_rect(hdc, x, sbw + at, cw, sbw + at + th2, sys_color(COLOR_BTNFACE));
                bevel(hdc, x, sbw + at, cw, sbw + at + th2, 0);
            }
        }
    }
    if ((snap->style & WS_BORDER) && snap->ctl != CTL_EDIT && snap->ctl != CTL_LISTBOX)
        w32_gdi_frame_rect(hdc, 0, 0, cw, ch, sys_color(COLOR_WINDOWFRAME));
    /* Where the keyboard is. Without this, Tab moves something invisible and
     * Enter presses a button the person cannot see they have selected. */
    if (snap->focused && snap->ctl && snap->ctl != CTL_DIALOG && snap->ctl != CTL_STATIC) {
        int in = snap->ctl == CTL_BUTTON ? 3 : 1;
        for (int x = in; x < cw - in; x += 2) {
            w32_gdi_fill_rect(hdc, x, in, x + 1, in + 1, sys_color(COLOR_BTNTEXT));
            w32_gdi_fill_rect(hdc, x, ch - in - 1, x + 1, ch - in, sys_color(COLOR_BTNTEXT));
        }
        for (int y = in; y < ch - in; y += 2) {
            w32_gdi_fill_rect(hdc, in, y, in + 1, y + 1, sys_color(COLOR_BTNTEXT));
            w32_gdi_fill_rect(hdc, cw - in - 1, y, cw - in, y + 1, sys_color(COLOR_BTNTEXT));
        }
    }
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

/* ---- the common controls' own messages ---------------------------------
 *
 * Each of these is small on its own; together they are the difference between
 * a control that exists and one that has anything in it. They are here rather
 * than inline in ctl_proc because that switch is long enough already.
 */

static int list_count(const wwin *snap) {
    return snap->list >= 0 ? g_list[snap->list].n : 0;
}
static uint64_t list_clear(uint64_t hwnd) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p && p->list >= 0) { g_list[p->list].n = 0; p->sel = -1; p->top = 0; }
    pthread_mutex_unlock(&g_lock);
    invalidate(hwnd);
    return 1;
}

/* Read a string out of a guest struct field that may be char* or wchar_t*. */
static void read_str_field(w32 *w, uint64_t ptr, int wide, char *out, size_t n) {
    out[0] = 0;
    if (!ptr) return;
    if (wide) w32_wtoa(w, ptr, out, n);
    else snprintf(out, n, "%.*s", (int)n - 1, (const char *)W32P(w, ptr));
}

/* LVCOLUMN: mask, fmt, cx, pszText, cchTextMax, iSubItem... The first three
 * are DWORDs and the fourth is a pointer, so the text offset is 12 either
 * way and the pointer's width is the only thing that differs. */
static uint64_t lv_insert_column(w32 *w, uint64_t hwnd, uint64_t p_, int wide) {
    if (!p_) return (uint64_t)-1;
    int cx = (int)(int32_t)w32_read(w, p_ + 8, 4);
    uint64_t txt = w32_read(w, p_ + 12, w32_ptrsize(w));
    char name[32];
    read_str_field(w, txt, wide, name, sizeof name);
    int idx = -1;
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p && p->list >= 0 && g_list[p->list].ncol < 8) {
        idx = g_list[p->list].ncol++;
        g_list[p->list].colw[idx] = cx > 0 ? cx : 100;
        snprintf(g_list[p->list].colname[idx], 32, "%s", name);
    }
    pthread_mutex_unlock(&g_lock);
    invalidate(hwnd);
    return (uint64_t)(int64_t)idx;
}

/* LVITEM: mask, iItem, iSubItem, state, stateMask, pszText, ... -- five
 * DWORDs then a pointer. */
static uint64_t lv_insert_item(w32 *w, uint64_t hwnd, uint64_t p_, int wide) {
    if (!p_) return (uint64_t)-1;
    int at = (int)(int32_t)w32_read(w, p_ + 4, 4);
    uint64_t txt = w32_read(w, p_ + 20, w32_ptrsize(w));
    char buf[LIST_TEXT];
    read_str_field(w, txt, wide, buf, sizeof buf);
    int idx = -1;
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p && p->list >= 0 && g_list[p->list].n < LIST_ITEMS) {
        int n = g_list[p->list].n;
        if (at < 0 || at > n) at = n;
        for (int i = n; i > at; i--) {
            memcpy(g_list[p->list].item[i], g_list[p->list].item[i - 1], LIST_TEXT);
            g_list[p->list].checked[i] = g_list[p->list].checked[i - 1];
        }
        snprintf(g_list[p->list].item[at], LIST_TEXT, "%s", buf);
        g_list[p->list].checked[at] = 0;
        g_list[p->list].n = n + 1;
        idx = at;
    }
    pthread_mutex_unlock(&g_lock);
    invalidate(hwnd);
    return (uint64_t)(int64_t)idx;
}

/* A sub-item's text. Report mode packs a row as its columns joined by tabs,
 * which is what the painter walks back apart. */
static uint64_t lv_set_item_text(w32 *w, uint64_t hwnd, int item, uint64_t p_, int wide) {
    if (!p_) return 0;
    int col = (int)(int32_t)w32_read(w, p_ + 8, 4);
    uint64_t txt = w32_read(w, p_ + 20, w32_ptrsize(w));
    char buf[LIST_TEXT];
    read_str_field(w, txt, wide, buf, sizeof buf);
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p && p->list >= 0 && item >= 0 && item < g_list[p->list].n) {
        char row[LIST_TEXT], out[LIST_TEXT];
        snprintf(row, sizeof row, "%s", g_list[p->list].item[item]);
        int at = 0, c = 0, n = 0;
        const char *seg = row;
        out[0] = 0;
        /* Rebuild the row, substituting the one column that changed. */
        for (c = 0; c < 8; c++) {
            const char *tab = strchr(seg, '\t');
            int len = tab ? (int)(tab - seg) : (int)strlen(seg);
            const char *src = c == col ? buf : seg;
            int slen = c == col ? (int)strlen(buf) : len;
            if (n && n + 1 < LIST_TEXT) out[n++] = '\t';
            for (int i = 0; i < slen && n + 1 < LIST_TEXT; i++) out[n++] = src[i];
            out[n] = 0;
            if (!tab) { if (col <= c) break; seg = ""; } else seg = tab + 1;
            if (!tab && col <= c) break;
        }
        (void)at;
        snprintf(g_list[p->list].item[item], LIST_TEXT, "%s", out);
    }
    pthread_mutex_unlock(&g_lock);
    invalidate(hwnd);
    return 1;
}

/* LVIS_STATEIMAGEMASK in the top byte of the low word carries the check
 * state: image 2 is ticked, 1 is not. That encoding is how every installer
 * sets a component on, and reading it wrong means the ticks never move. */
static uint64_t lv_set_state(w32 *w, uint64_t hwnd, int item, uint64_t p_) {
    if (!p_) return 0;
    uint32_t state = (uint32_t)w32_read(w, p_ + 12, 4);
    uint32_t mask  = (uint32_t)w32_read(w, p_ + 16, 4);
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p && p->list >= 0) {
        int lo = item < 0 ? 0 : item, hi = item < 0 ? g_list[p->list].n - 1 : item;
        for (int i = lo; i <= hi && i < g_list[p->list].n; i++) {
            if (mask & 0xF000u) g_list[p->list].checked[i] = ((state >> 12) & 0xF) >= 2;
            if ((mask & 1) && i == item) p->sel = (state & 1) ? i : p->sel;
        }
    }
    pthread_mutex_unlock(&g_lock);
    invalidate(hwnd);
    return 1;
}
static uint64_t lv_get_state(const wwin *snap, int item, uint32_t mask) {
    if (snap->list < 0 || item < 0 || item >= g_list[snap->list].n) return 0;
    uint32_t st = 0;
    if (g_list[snap->list].checked[item]) st |= 0x2000u;
    else st |= 0x1000u;
    if (item == snap->sel) st |= 1;
    return st & (mask ? mask : 0xFFFFFFFFu);
}

/* TVINSERTSTRUCT: hParent, hInsertAfter, then a TVITEM whose text pointer is
 * at mask+hItem+state+stateMask -- four DWORDs in. */
static uint64_t tv_insert(w32 *w, uint64_t hwnd, uint64_t p_, int wide) {
    if (!p_) return 0;
    int ps = (int)w32_ptrsize(w);
    uint64_t item = p_ + 2u * (unsigned)ps;
    uint64_t txt = w32_read(w, item + 4 + (unsigned)ps + 8, (unsigned)ps);
    char buf[LIST_TEXT];
    read_str_field(w, txt, wide, buf, sizeof buf);
    uint64_t h = 0;
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p && p->list >= 0 && g_list[p->list].n < LIST_ITEMS) {
        int at = g_list[p->list].n++;
        snprintf(g_list[p->list].item[at], LIST_TEXT, "%s", buf);
        h = 0x00050000u + (uint64_t)at * 4;
    }
    pthread_mutex_unlock(&g_lock);
    invalidate(hwnd);
    return h;
}

/* TCITEM: mask, dwState, dwStateMask, pszText -- three DWORDs then a
 * pointer. */
static uint64_t tab_insert(w32 *w, uint64_t hwnd, int at, uint64_t p_, int wide) {
    if (!p_) return (uint64_t)-1;
    uint64_t txt = w32_read(w, p_ + 12, w32_ptrsize(w));
    char buf[LIST_TEXT];
    read_str_field(w, txt, wide, buf, sizeof buf);
    int idx = -1;
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p && p->list >= 0 && g_list[p->list].n < LIST_ITEMS) {
        int n = g_list[p->list].n;
        if (at < 0 || at > n) at = n;
        for (int i = n; i > at; i--) memcpy(g_list[p->list].item[i], g_list[p->list].item[i-1], LIST_TEXT);
        snprintf(g_list[p->list].item[at], LIST_TEXT, "%s", buf);
        g_list[p->list].n = n + 1;
        if (p->sel < 0) p->sel = 0;
        idx = at;
    }
    pthread_mutex_unlock(&g_lock);
    invalidate(hwnd);
    return (uint64_t)(int64_t)idx;
}

/* SB_SETTEXT's wParam is the part index in its low byte; the rest is drawing
 * style, which does not change what the text says. */
static uint64_t sb_set_text(w32 *w, uint64_t hwnd, int part, uint64_t txt, int wide) {
    char buf[LIST_TEXT];
    read_str_field(w, txt, wide, buf, sizeof buf);
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p && p->list >= 0 && part >= 0 && part < LIST_ITEMS) {
        while (g_list[p->list].n <= part) g_list[p->list].item[g_list[p->list].n++][0] = 0;
        snprintf(g_list[p->list].item[part], LIST_TEXT, "%s", buf);
    }
    pthread_mutex_unlock(&g_lock);
    invalidate(hwnd);
    return 1;
}

/* A slider's position and range. `pos` of a very negative number means "do
 * not change it", which keeps the four TBM_ messages one function. */
static void track_set(uint64_t hwnd, int pos, int lo, int hi) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p) {
        if (lo != -1 || hi != -1) { if (lo != -1) p->lo = lo; if (hi != -1) p->hi = hi; }
        if (pos > -1000000000) p->pos = pos;
        if (p->hi <= p->lo) p->hi = p->lo + 1;
        if (p->pos < p->lo) p->pos = p->lo;
        if (p->pos > p->hi) p->pos = p->hi;
    }
    pthread_mutex_unlock(&g_lock);
    invalidate(hwnd);
}

/* Rich text, reduced to text.
 *
 * A licence agreement streamed in as RTF is a brace-nested control-word
 * soup, and rendering it properly is a project of its own. Stripping it is
 * not: drop the control words, drop the groups that exist only to carry
 * fonts and colours, turn \par into a newline, and what is left is the
 * sentences somebody has to read before clicking I Agree. That is the part
 * that matters, and showing it beats showing a blank box. */
static int rtf_to_text(const char *in, int n, char *out, int cap) {
    int o = 0, depth = 0, skip_depth = -1;
    for (int i = 0; i < n && o + 1 < cap; i++) {
        char c = in[i];
        if (c == '{') { depth++; continue; }
        if (c == '}') { if (skip_depth >= 0 && depth <= skip_depth) skip_depth = -1; depth--; continue; }
        if (c == '\\') {
            int j = i + 1;
            if (j < n && !((in[j] >= 'a' && in[j] <= 'z') || (in[j] >= 'A' && in[j] <= 'Z'))) {
                /* An escaped literal: \{ \} \\ , or \'xx for a byte. */
                if (in[j] == '\'' && j + 2 < n) { i = j + 2; continue; }
                if (skip_depth < 0) out[o++] = in[j];
                i = j;
                continue;
            }
            char word[32];
            int wn = 0;
            while (j < n && ((in[j] >= 'a' && in[j] <= 'z') || (in[j] >= 'A' && in[j] <= 'Z'))
                   && wn + 1 < (int)sizeof word) word[wn++] = in[j++];
            word[wn] = 0;
            while (j < n && (in[j] == '-' || (in[j] >= '0' && in[j] <= '9'))) j++;
            if (j < n && in[j] == ' ') j++;
            i = j - 1;
            if (!strcmp(word, "par") || !strcmp(word, "line")) { if (skip_depth < 0) out[o++] = '\n'; }
            else if (!strcmp(word, "tab")) { if (skip_depth < 0) out[o++] = '\t'; }
            else if (!strcmp(word, "fonttbl") || !strcmp(word, "colortbl")
                  || !strcmp(word, "stylesheet") || !strcmp(word, "info")
                  || !strcmp(word, "generator") || !strcmp(word, "pict")) {
                if (skip_depth < 0) skip_depth = depth;
            }
            continue;
        }
        if (c == '\r' || c == '\n') continue;          /* RTF's own line breaks are not text */
        if (skip_depth < 0) out[o++] = c;
    }
    out[o] = 0;
    return o;
}

/* EM_STREAMIN(format, EDITSTREAM*). EDITSTREAM is { cookie, error, callback }
 * -- a pointer, a DWORD and a pointer, so the callback sits two pointers in
 * once alignment is accounted for. The callback is
 * DWORD (*)(DWORD_PTR cookie, BYTE *buf, LONG cb, LONG *pcb), and it is
 * called until it reports fewer bytes than were asked for. */
static uint64_t em_stream_in(w32 *w, uint64_t hwnd, uint32_t fmt, uint64_t es) {
    if (!es) return 0;
    int ps = (int)w32_ptrsize(w);
    uint64_t cookie = w32_read(w, es, (unsigned)ps);
    uint64_t cb = w32_read(w, es + 2u * (unsigned)ps, (unsigned)ps);
    if (!cb) return 0;

    /* Somewhere for the callback to write, and somewhere to count what it
     * wrote -- both in guest memory, because the guest writes to them. */
    enum { CHUNK = 4096 };
    uint64_t buf = w32_alloc(w, CHUNK + 16, 0);
    if (!buf) return 0;
    uint64_t pcb = buf + CHUNK;

    char *acc = (char *)malloc(64 * 1024);
    if (!acc) return 0;
    size_t an = 0, acap = 64 * 1024;
    int unicode = (fmt & 0x20) != 0;

    for (int guard = 0; guard < 512; guard++) {
        w32_write(w, pcb, 4, 0);
        uint64_t args[4] = { cookie, buf, CHUNK, pcb };
        uint64_t rc = w32_call_guest(w, cb, 4, args);
        int got = (int)(int32_t)w32_read(w, pcb, 4);
        if (rc != 0 || got <= 0) break;
        if (got > CHUNK) got = CHUNK;
        const unsigned char *src = (const unsigned char *)W32P(w, buf);
        if (!src) break;
        /* UTF-16 in, ASCII out: this layer draws one byte per character. */
        int step = unicode ? 2 : 1;
        for (int i = 0; i + step <= got; i += step) {
            if (an + 2 >= acap) {
                char *bigger = (char *)realloc(acc, acap * 2);
                if (!bigger) { got = 0; break; }
                acc = bigger; acap *= 2;
            }
            unsigned ch = unicode ? (unsigned)(src[i] | src[i + 1] << 8) : src[i];
            acc[an++] = (char)(ch < 256 ? ch : '?');
        }
        if (got < CHUNK) break;
    }
    acc[an] = 0;

    /* SF_RTF is 2. Some programs say SF_TEXT and hand over RTF anyway, so the
     * signature is trusted over the flag. */
    char *text = acc;
    char *plain = 0;
    if ((fmt & 2) || (an > 5 && !strncmp(acc, "{\\rtf", 5))) {
        plain = (char *)malloc(an + 2);
        if (plain) { rtf_to_text(acc, (int)an, plain, (int)an + 1); text = plain; }
    }

    w32_set_window_text(w, hwnd, text);
    free(plain);
    free(acc);
    invalidate(hwnd);
    return 0;
}

/* Move a scrolling control by `lines`, and repaint if anything moved. */
static void scroll_by(w32 *w, uint64_t hwnd, const wwin *snap, int lines) {
    (void)w;
    int moved = 0;
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p) {
        int step = p->step > 0 ? p->step : 13;
        int was = p->ctl == CTL_LISTVIEW || p->ctl == CTL_TREEVIEW ? p->top : p->scroll_pos;
        if (p->ctl == CTL_LISTVIEW || p->ctl == CTL_TREEVIEW) {
            p->top += lines;
            if (p->top < 0) p->top = 0;
            if (p->list >= 0 && p->top > g_list[p->list].n - 1)
                p->top = g_list[p->list].n > 0 ? g_list[p->list].n - 1 : 0;
            moved = p->top != was;
        } else {
            p->scroll_pos += lines * step;
            if (p->scroll_pos < 0) p->scroll_pos = 0;
            if (p->scroll_pos > p->scroll_max) p->scroll_pos = p->scroll_max;
            moved = p->scroll_pos != was;
        }
    }
    pthread_mutex_unlock(&g_lock);
    (void)snap;
    if (moved) invalidate(hwnd);
}

/* A click in the vertical scroll bar: the arrows step, the track pages. */
static void scroll_click(w32 *w, uint64_t hwnd, const wwin *snap, int my) {
    int sbw = 16;
    if (my < sbw) scroll_by(w, hwnd, snap, -1);
    else if (my > snap->ch - sbw) scroll_by(w, hwnd, snap, 1);
    else {
        int page = snap->scroll_page > 0 ? snap->scroll_page : snap->ch;
        int step = snap->step > 0 ? snap->step : 13;
        int lines = page / step;
        if (lines < 1) lines = 1;
        int span = snap->ch - 2 * sbw;
        int at = span > 0 && snap->scroll_max > 0
               ? sbw + span * snap->scroll_pos / (snap->scroll_max + page) : sbw;
        scroll_by(w, hwnd, snap, my < at ? -lines : lines);
    }
}

/* WM_NOTIFY, which is how a common control tells its parent something
 * happened. NMHDR is { HWND hwndFrom; UINT_PTR idFrom; UINT code; } -- two
 * pointer-sized fields and a DWORD, so it has to be built in guest memory at
 * the guest's own widths. */
static void post_notify(w32 *w, uint64_t parent, uint64_t from, uint64_t id, uint32_t code) {
    if (!parent) return;
    int ps = (int)w32_ptrsize(w);
    uint64_t nm = w32_alloc(w, 64, 0);
    if (!nm) return;
    w32_write(w, nm, (unsigned)ps, from);
    w32_write(w, nm + (unsigned)ps, (unsigned)ps, id);
    w32_write(w, nm + 2u * (unsigned)ps, 4, code);
    send_to(w, parent, WM_NOTIFY, id, nm, 1);
}

/* A click on one of the controls this file draws itself. */
static void ctl_click(w32 *w, uint64_t hwnd, const wwin *snap, int mx, int my) {
    pthread_mutex_lock(&g_lock);
    g_focus = hwnd;
    pthread_mutex_unlock(&g_lock);

    switch (snap->ctl) {
    case CTL_LISTVIEW: {
        if (snap->list < 0) break;
        uint64_t hdc = w32_dc_for_window(w, hwnd, 0);
        int lh = (hdc ? w32_gdi_line_height(hdc) : 13) + 4;
        if (hdc) w32_dc_release(hdc);
        int y = 2 + (g_list[snap->list].ncol > 0 ? lh : 0);
        int idx = snap->top + (my - y) / (lh > 0 ? lh : 17);
        pthread_mutex_lock(&g_lock);
        wwin *p = win_of(hwnd);
        if (p && idx >= 0 && idx < g_list[snap->list].n) {
            p->sel = idx;
            /* Clicking the box toggles the tick; clicking the row selects.
             * That is the distinction an installer's component page lives on. */
            if ((snap->exstyle & LVS_EX_CHECKBOXES_) && mx < 4 + lh)
                g_list[snap->list].checked[idx] = !g_list[snap->list].checked[idx];
        } else idx = -1;
        pthread_mutex_unlock(&g_lock);
        invalidate(hwnd);
        if (idx >= 0) post_notify(w, snap->parent, hwnd, snap->id, (uint32_t)-101);  /* LVN_ITEMCHANGED */
        break;
    }
    case CTL_TREEVIEW: {
        if (snap->list < 0) break;
        uint64_t hdc = w32_dc_for_window(w, hwnd, 0);
        int lh = (hdc ? w32_gdi_line_height(hdc) : 13) + 2;
        if (hdc) w32_dc_release(hdc);
        int idx = snap->top + (my - 2) / (lh > 0 ? lh : 15);
        pthread_mutex_lock(&g_lock);
        wwin *p = win_of(hwnd);
        if (p && idx >= 0 && idx < g_list[snap->list].n) p->sel = idx; else idx = -1;
        pthread_mutex_unlock(&g_lock);
        invalidate(hwnd);
        if (idx >= 0) post_notify(w, snap->parent, hwnd, snap->id, (uint32_t)-402);  /* TVN_SELCHANGED */
        break;
    }
    case CTL_TAB: {
        if (snap->list < 0) break;
        uint64_t hdc = w32_dc_for_window(w, hwnd, 0);
        int th = (hdc ? w32_gdi_line_height(hdc) : 13) + 8;
        if (my > th) { if (hdc) w32_dc_release(hdc); break; }
        int x = 2, hit = -1;
        for (int i = 0; i < g_list[snap->list].n; i++) {
            const char *lab = g_list[snap->list].item[i];
            int tw2 = 0, tht = 0;
            if (hdc) w32_gdi_text_extent(hdc, lab, (int)strlen(lab), &tw2, &tht);
            int wdt = tw2 + 16;
            if (mx >= x && mx < x + wdt) { hit = i; break; }
            x += wdt + 1;
        }
        if (hdc) w32_dc_release(hdc);
        if (hit < 0) break;
        pthread_mutex_lock(&g_lock);
        wwin *p = win_of(hwnd);
        if (p) p->sel = hit;
        pthread_mutex_unlock(&g_lock);
        invalidate(hwnd);
        post_notify(w, snap->parent, hwnd, snap->id, (uint32_t)-551);   /* TCN_SELCHANGE */
        break;
    }
    case CTL_TRACK: {
        int span = snap->hi - snap->lo;
        if (span <= 0) break;
        int vert = snap->ch > snap->cw;
        int at = vert ? my - 10 : mx - 10;
        int len = (vert ? snap->ch : snap->cw) - 20;
        int val = len > 0 ? snap->lo + span * at / len : snap->lo;
        track_set(hwnd, val, -1, -1);
        /* WM_HSCROLL/WM_VSCROLL with SB_THUMBTRACK is how a program reads a
         * slider it did not move itself. */
        send_to(w, snap->parent, vert ? WM_VSCROLL : WM_HSCROLL,
                (uint64_t)(5u | ((uint32_t)val << 16)), hwnd, 1);
        break;
    }
    case CTL_LINK:
        post_notify(w, snap->parent, hwnd, snap->id, (uint32_t)-2);     /* NM_CLICK */
        break;
    default: break;
    }
}

static uint64_t ctl_proc(w32 *w, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int wide) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (!p) { pthread_mutex_unlock(&g_lock); return 0; }
    wwin snap = *p;
    snap.focused = (g_focus == hwnd);
    pthread_mutex_unlock(&g_lock);

    /* The common-control messages. Each family is WM_USER plus an index, so
     * the numbers collide between families -- TBM_SETPOS and SB_SETPARTS are
     * both 0x0405 -- and they are told apart by which control is being sent
     * to, which is why this is dispatched on `snap.ctl` before the general
     * switch below. */
    switch (snap.ctl) {
    case CTL_LISTVIEW:
        switch (msg) {
        case 0x1004: return list_count(&snap);                       /* GETITEMCOUNT */
        case 0x1009: return list_clear(hwnd);                        /* DELETEALLITEMS */
        case 0x1036:                                                 /* SETEXTENDEDLISTVIEWSTYLE */
            pthread_mutex_lock(&g_lock);
            if ((p = win_of(hwnd))) p->exstyle |= (uint32_t)lp;
            pthread_mutex_unlock(&g_lock);
            invalidate(hwnd);
            return 0;
        case 0x101B: case 0x1061: return lv_insert_column(w, hwnd, lp, msg == 0x1061);
        case 0x1007: case 0x104D: return lv_insert_item(w, hwnd, lp, msg == 0x104D);
        case 0x102E: case 0x1074: return lv_set_item_text(w, hwnd, (int)wp, lp, msg == 0x1074);
        case 0x102B: return lv_set_state(w, hwnd, (int)(int32_t)wp, lp);  /* SETITEMSTATE */
        case 0x102C: return lv_get_state(&snap, (int)wp, (uint32_t)lp);   /* GETITEMSTATE */
        case 0x100C:                                                 /* GETNEXTITEM */
            return (uint64_t)(int64_t)(snap.sel >= 0 && (uint32_t)lp ? snap.sel : -1);
        default: break;
        }
        break;
    case CTL_TREEVIEW:
        switch (msg) {
        case 0x1105: return list_clear(hwnd);                        /* TVM_DELETEITEM(ROOT) */
        case 0x1100: case 0x1132: return tv_insert(w, hwnd, lp, msg == 0x1132);
        default: break;
        }
        break;
    case CTL_TAB:
        switch (msg) {
        case 0x1304: return list_count(&snap);                       /* GETITEMCOUNT */
        case 0x1309: return list_clear(hwnd);                        /* DELETEALLITEMS */
        case 0x1307: case 0x133E: return tab_insert(w, hwnd, (int)wp, lp, msg == 0x133E);
        case 0x130B: return (uint64_t)(int64_t)snap.sel;             /* GETCURSEL */
        case 0x130C: {                                               /* SETCURSEL */
            int old = snap.sel;
            pthread_mutex_lock(&g_lock);
            if ((p = win_of(hwnd))) p->sel = (int)(int32_t)wp;
            pthread_mutex_unlock(&g_lock);
            invalidate(hwnd);
            return (uint64_t)(int64_t)old;
        }
        default: break;
        }
        break;
    case CTL_STATUS:
        switch (msg) {
        case 0x0401: case 0x040B: return sb_set_text(w, hwnd, (int)(wp & 0xFF), lp, msg == 0x040B);
        case 0x0404:                                                 /* SB_SETPARTS */
            pthread_mutex_lock(&g_lock);
            if ((p = win_of(hwnd)) && p->list >= 0 && (int)wp <= LIST_ITEMS) {
                while (g_list[p->list].n < (int)wp) g_list[p->list].item[g_list[p->list].n++][0] = 0;
            }
            pthread_mutex_unlock(&g_lock);
            invalidate(hwnd);
            return 1;
        default: break;
        }
        break;
    case CTL_TRACK:
        switch (msg) {
        case 0x0400: return (uint64_t)(int64_t)snap.pos;             /* TBM_GETPOS */
        case 0x0401: return (uint64_t)(int64_t)snap.lo;              /* TBM_GETRANGEMIN */
        case 0x0402: return (uint64_t)(int64_t)snap.hi;              /* TBM_GETRANGEMAX */
        case 0x0405: track_set(hwnd, (int)(int32_t)lp, -1, -1); return 0;   /* TBM_SETPOS */
        case 0x0406: track_set(hwnd, -1000000000, (int)(int16_t)(lp & 0xFFFF),
                               (int)(int16_t)((lp >> 16) & 0xFFFF)); return 0;
        case 0x0407: track_set(hwnd, -1000000000, (int)(int32_t)lp, -1); return 0;
        case 0x0408: track_set(hwnd, -1000000000, -1, (int)(int32_t)lp); return 0;
        default: break;
        }
        break;
    case CTL_EDIT:
        /* EM_STREAMIN is how a RichEdit is filled: the program hands over a
         * callback and this pulls the text out of it in chunks. Every Inno
         * Setup licence page arrives this way, which is why one of them was a
         * blank white box until now. */
        if (msg == 0x0449) return em_stream_in(w, hwnd, (uint32_t)wp, lp);
        if (msg == 0x0443) return 0;                    /* EM_SETBKGNDCOLOR */
        if (msg == 0x0445) return 0;                    /* EM_SETEVENTMASK */
        if (msg == 0x0444) return 1;                    /* EM_SETCHARFORMAT: accepted, ignored */
        if (msg == 0x045B) return 0;                    /* EM_AUTOURLDETECT */
        if (msg == 0x0435) return 0;                    /* EM_EXLIMITTEXT */
        break;
    default: break;
    }

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
        } else if (snap.enabled) {
            int mx = (int)(int16_t)(lp & 0xFFFF), my = (int)(int16_t)((lp >> 16) & 0xFFFF);
            ctl_click(w, hwnd, &snap, mx, my);
        }
        /* A click in a scroll bar, whatever the control is. Handled after the
         * control's own hit test so a list view's rows still take clicks that
         * are not in the bar. */
        if (snap.style & WS_VSCROLL) {
            int mx = (int)(int16_t)(lp & 0xFFFF), my = (int)(int16_t)((lp >> 16) & 0xFFFF);
            if (mx >= snap.cw - 16) scroll_click(w, hwnd, &snap, my);
        }
        return 0;

    /* The wheel. A licence agreement is the reason this exists: it is how
     * anybody actually reads one, and a page with no wheel and no reachable
     * scroll bar cannot be got to the bottom of at all. */
    case WM_MOUSEWHEEL: {
        int delta = (int)(int16_t)((wp >> 16) & 0xFFFF);
        scroll_by(w, hwnd, &snap, -delta * 3 / 120);
        return 0;
    }

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
    /* The frame, before the window paints its inside.
     *
     * Here rather than in paint_control because paint_control is only reached
     * for a window this file draws the inside of; a program with its own
     * window class paints its own client area and expects the system to paint
     * the frame around it, and doing that only for the classes we recognise
     * is why an installer's title bar came up blank. Here rather than only in
     * repaint_area because most repaints arrive as a queued WM_PAINT and
     * never go through the compositor at all. */
    if (msg == WM_PAINT) {
        pthread_mutex_lock(&g_lock);
        wwin *cp = win_of(hwnd);
        wwin snap;
        int chrome = cp && cp->visible && cp->cyo > 0 && !(cp->style & WS_CHILD);
        if (chrome) snap = *cp;
        pthread_mutex_unlock(&g_lock);
        if (chrome) paint_chrome(w, hwnd, &snap);
    }
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
/* Give an area of the screen back to whoever is under it.
 *
 * There is one surface and no per-window backing store, so a window that
 * hides, moves or is destroyed leaves its pixels exactly where they were.
 * On a game that never happens -- one window, never moved. On an installer
 * it happens on every page: an NSIS wizard is an outer dialog with an inner
 * child dialog per page, and it destroys one and creates the next in the
 * same place. Without this the previous page stays on screen underneath the
 * new one, which is precisely what "glitchy" looks like.
 *
 * Painter's order, so the answer is: paint the desktop there, then every
 * window that overlaps it, oldest first. */
static void repaint_area(w32 *w, int x, int y, int cx, int cy) {
    if (cx <= 0 || cy <= 0) return;
    uint64_t dc = w32_dc_for_window(w, 0, 1);
    if (dc) {
        w32_gdi_fill_rect(dc, x, y, x + cx, y + cy, sys_color(COLOR_BACKGROUND));
        w32_dc_release(dc);
    }
    for (int i = 0; i < MAX_WINDOWS; i++) {
        pthread_mutex_lock(&g_lock);
        wwin *p = &g_win[i];
        int hit = p->used && p->visible && p->w > 0 && p->h > 0 &&
                  p->x < x + cx && p->x + p->w > x && p->y < y + cy && p->y + p->h > y;
        uint64_t h = hit ? HW_BASE + (uint64_t)i * HW_STEP : 0;
        pthread_mutex_unlock(&g_lock);
        if (h) deliver(w, h, WM_PAINT, 0, 0, 0);
    }
    w32_desktop_damaged();
}

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

/* The default push button: the one Enter presses. A template marks it with
 * BS_DEFPUSHBUTTON, and it is the difference between a wizard you can get
 * through with a keyboard and one you cannot. */
static uint64_t default_button(uint64_t dlg) {
    uint64_t first_button = 0;
    pthread_mutex_lock(&g_lock);
    uint64_t found = 0;
    for (int i = 0; i < MAX_WINDOWS && !found; i++) {
        wwin *p = &g_win[i];
        if (!p->used || p->parent != dlg || p->ctl != CTL_BUTTON || !p->visible || !p->enabled) continue;
        uint64_t h = HW_BASE + (uint64_t)i * HW_STEP;
        if ((p->style & BS_TYPEMASK) == BS_DEFPUSHBUTTON) found = h;
        else if (!first_button && (p->style & BS_TYPEMASK) == 0) first_button = h;
    }
    pthread_mutex_unlock(&g_lock);
    /* No marked default: the first plain push button is what Enter does on
     * Windows too, and on a wizard that is Next. */
    return found ? found : first_button;
}

/* Tab order. Windows walks the controls in the order the template created
 * them and stops at the ones with WS_TABSTOP, wrapping at the end -- which
 * is exactly the array order here, because the template created them in
 * that order. */
static uint64_t next_tabstop(uint64_t dlg, uint64_t from, int back) {
    pthread_mutex_lock(&g_lock);
    int start = -1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_win[i].used && HW_BASE + (uint64_t)i * HW_STEP == from) { start = i; break; }
    uint64_t found = 0;
    for (int step = 1; step <= MAX_WINDOWS && !found; step++) {
        int i = start < 0 ? (back ? MAX_WINDOWS - step : step - 1)
                          : ((start + (back ? -step : step)) % MAX_WINDOWS + MAX_WINDOWS) % MAX_WINDOWS;
        wwin *p = &g_win[i];
        if (p->used && p->parent == dlg && p->visible && p->enabled && (p->style & WS_TABSTOP))
            found = HW_BASE + (uint64_t)i * HW_STEP;
    }
    pthread_mutex_unlock(&g_lock);
    return found;
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
        /* Getting through a wizard without a mouse. On a phone this is not a
         * convenience: a Next button is a small target on a frame that has
         * been scaled to the panel, and Enter is the reliable way to press
         * the one the installer is waiting on. */
        if (wp == VK_ESCAPE) { end_dialog(hwnd, IDCANCEL); return 1; }
        if (wp == VK_RETURN) {
            pthread_mutex_lock(&g_lock);
            uint64_t f = g_focus;
            wwin *fp = win_of(f);
            int focus_is_button = fp && fp->parent == hwnd && fp->ctl == CTL_BUTTON && fp->enabled;
            pthread_mutex_unlock(&g_lock);
            /* Enter presses the focused button if there is one, and the
             * dialog's default button otherwise. */
            uint64_t target = focus_is_button ? f : default_button(hwnd);
            if (target) { send_to(w, target, BM_CLICK, 0, 0, wide); return 1; }
            end_dialog(hwnd, IDOK);
            return 1;
        }
        if (wp == VK_TAB) {
            pthread_mutex_lock(&g_lock);
            uint64_t from = g_focus;
            int shift = (g_keys[VK_SHIFT] & 0x80) != 0;
            pthread_mutex_unlock(&g_lock);
            uint64_t nxt = next_tabstop(hwnd, from, shift);
            if (nxt) {
                pthread_mutex_lock(&g_lock);
                g_focus = nxt;
                pthread_mutex_unlock(&g_lock);
                invalidate(hwnd);
            }
            return 1;
        }
        if (wp == VK_SPACE) {
            pthread_mutex_lock(&g_lock);
            uint64_t f = g_focus;
            wwin *fp = win_of(f);
            int is_button = fp && fp->parent == hwnd && fp->ctl == CTL_BUTTON && fp->enabled;
            pthread_mutex_unlock(&g_lock);
            if (is_button) { send_to(w, f, BM_CLICK, 0, 0, wide); return 1; }
            return 0;
        }
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
        /* The horizontal unit is the *average* character width -- that is
         * how Windows defines it, and taking any single letter's width
         * instead scales every control in the template by the ratio between
         * that letter and the average. */
        cx = w32_gdi_average_width(hdc);
        cy = w32_gdi_line_height(hdc);
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
    int cap = ((style & WS_CAPTION) == WS_CAPTION) ? caption_height() : 0;
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

    /* At the DPI we are claiming, not at 96. Converting the template's point
     * size against a fixed 96 while the layout scales with the real DPI is
     * what gives a dialog twice the size with the same tiny text in it. */
    uint64_t font = pointsize > 0 ? w32_make_font(-w32_points_to_pixels(pointsize), face) : 0;
    /* deliver, not send_to: the dialog's procedure is the guest's, it
     * returns FALSE for a message it does not handle, and the default
     * handling is what records the font. Without it the dialog measures its
     * units in the system font and its controls draw in the template's --
     * a correctly sized dialog full of wrongly sized text. */
    if (font) deliver(w, dlg, WM_SETFONT, font, 0, 1);

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
        /* A menu open over a modal dialog takes the input, the same way it
         * does in the program's own loop. */
        if ((q.msg >= WM_MOUSEFIRST && q.msg <= WM_MOUSELAST) || is_key_msg(q.msg))
            if (menu_input(w, q.msg, q.wparam, q.x, q.y)) { surface_present(); continue; }
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
    /* Somewhere for the keyboard to be, *before* WM_INITDIALOG. Windows sets
     * the default focus first and a dialog procedure returning TRUE means
     * "keep it" -- so choosing it afterwards would both override a procedure
     * that set its own and leave the first painted frame showing no focus at
     * all. */
    { uint64_t first = next_tabstop(dlg, 0, 0);
      if (first) { pthread_mutex_lock(&g_lock); g_focus = first; pthread_mutex_unlock(&g_lock); } }
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
/* ---- the window calls a game makes that an installer does not ----------- */

static void u_BringWindowToTop(w32 *w) { RET(send_to(w, ARG(0), 0, 0, 0, 0) == 0 ? 1 : 1); }
static void u_IntersectRect(w32 *w) {
    uint64_t out = ARG(0), a = ARG(1), b = ARG(2);
    if (!out || !a || !b) { RET(0); return; }
    int32_t al = (int32_t)w32_read(w, a, 4), at = (int32_t)w32_read(w, a + 4, 4);
    int32_t ar = (int32_t)w32_read(w, a + 8, 4), ab = (int32_t)w32_read(w, a + 12, 4);
    int32_t bl = (int32_t)w32_read(w, b, 4), bt = (int32_t)w32_read(w, b + 4, 4);
    int32_t br = (int32_t)w32_read(w, b + 8, 4), bb = (int32_t)w32_read(w, b + 12, 4);
    int32_t l = al > bl ? al : bl, t = at > bt ? at : bt;
    int32_t r = ar < br ? ar : br, bo = ab < bb ? ab : bb;
    if (l >= r || t >= bo) { put_rect(w, out, 0, 0, 0, 0); RET(0); return; }
    put_rect(w, out, l, t, r, bo);
    RET(1);
}
static void u_UnionRect(w32 *w) {
    uint64_t out = ARG(0), a = ARG(1), b = ARG(2);
    if (!out || !a || !b) { RET(0); return; }
    int32_t al = (int32_t)w32_read(w, a, 4), at = (int32_t)w32_read(w, a + 4, 4);
    int32_t ar = (int32_t)w32_read(w, a + 8, 4), ab = (int32_t)w32_read(w, a + 12, 4);
    int32_t bl = (int32_t)w32_read(w, b, 4), bt = (int32_t)w32_read(w, b + 4, 4);
    int32_t br = (int32_t)w32_read(w, b + 8, 4), bb = (int32_t)w32_read(w, b + 12, 4);
    put_rect(w, out, al < bl ? al : bl, at < bt ? at : bt,
                     ar > br ? ar : br, ab > bb ? ab : bb);
    RET(1);
}
static void u_IsRectEmpty(w32 *w) {
    uint64_t r = ARG(0);
    if (!r) { RET(1); return; }
    RET((int32_t)w32_read(w, r, 4) >= (int32_t)w32_read(w, r + 8, 4) ||
        (int32_t)w32_read(w, r + 4, 4) >= (int32_t)w32_read(w, r + 12, 4));
}
/* PtInRect takes a POINT *by value*, and where that value lives is not the
 * same in the two bitnesses: x64 packs an eight-byte structure into a single
 * register, so both coordinates arrive as one argument, while x86 pushes
 * them as two words. Reading it as two arguments on x64 gives the x
 * coordinate and then whatever was in the next register -- which is how a
 * hit test passes on one build and fails on the other. */
static void point_arg(w32 *w, int first, int32_t *x, int32_t *y) {
    if (w->is32) {
        *x = (int32_t)(uint32_t)w32_arg(w, first);
        *y = (int32_t)(uint32_t)w32_arg(w, first + 1);
    } else {
        uint64_t pt = w32_arg(w, first);
        *x = (int32_t)(uint32_t)pt;
        *y = (int32_t)(uint32_t)(pt >> 32);
    }
}
static void u_PtInRect(w32 *w) {
    uint64_t r = ARG(0);
    if (!r) { RET(0); return; }
    int32_t x, y;
    point_arg(w, 1, &x, &y);
    RET(x >= (int32_t)w32_read(w, r, 4) && x < (int32_t)w32_read(w, r + 8, 4) &&
        y >= (int32_t)w32_read(w, r + 4, 4) && y < (int32_t)w32_read(w, r + 12, 4));
}
static void u_SetRect(w32 *w) {
    put_rect(w, ARG(0), (int)(int32_t)(uint32_t)ARG(1), (int)(int32_t)(uint32_t)ARG(2),
                        (int)(int32_t)(uint32_t)ARG(3), (int)(int32_t)(uint32_t)ARG(4));
    RET(1);
}
static void u_OffsetRect(w32 *w) {
    uint64_t r = ARG(0);
    if (!r) { RET(0); return; }
    int32_t dx = (int32_t)(uint32_t)ARG(1), dy = (int32_t)(uint32_t)ARG(2);
    w32_write(w, r,      4, (uint64_t)(uint32_t)((int32_t)w32_read(w, r, 4) + dx));
    w32_write(w, r + 4,  4, (uint64_t)(uint32_t)((int32_t)w32_read(w, r + 4, 4) + dy));
    w32_write(w, r + 8,  4, (uint64_t)(uint32_t)((int32_t)w32_read(w, r + 8, 4) + dx));
    w32_write(w, r + 12, 4, (uint64_t)(uint32_t)((int32_t)w32_read(w, r + 12, 4) + dy));
    RET(1);
}
/* MapWindowPoints(from, to, points, count): screen coordinates are the
 * common currency, so each point goes out of `from` and into `to`. */
static void u_MapWindowPoints(w32 *w) {
    uint64_t from = ARG(0), to = ARG(1), pts = ARG(2);
    uint32_t n = (uint32_t)ARG(3);
    int fx = 0, fy = 0, tx = 0, ty = 0, d1 = 0, d2 = 0;
    if (from) w32_window_area(from, 0, &fx, &fy, &d1, &d2);
    if (to)   w32_window_area(to, 0, &tx, &ty, &d1, &d2);
    int dx = fx - tx, dy = fy - ty;
    for (uint32_t i = 0; i < n && i < 4096 && pts; i++) {
        uint64_t p = pts + (uint64_t)i * 8;
        w32_write(w, p,     4, (uint64_t)(uint32_t)((int32_t)w32_read(w, p, 4) + dx));
        w32_write(w, p + 4, 4, (uint64_t)(uint32_t)((int32_t)w32_read(w, p + 4, 4) + dy));
    }
    RET(((uint64_t)(uint32_t)dy << 16) | (uint32_t)(dx & 0xFFFF));
}
/* EnumWindows over the top-level windows this program made. There is no
 * desktop full of other applications, and saying there is would be a lie a
 * game could act on -- it enumerates to find its own window. */
static void u_EnumWindows(w32 *w) {
    uint64_t fn = ARG(0), param = ARG(1);
    if (!fn) { RET(0); return; }
    for (int i = 0; i < MAX_WINDOWS; i++) {
        pthread_mutex_lock(&g_lock);
        uint64_t h = (g_win[i].used && !g_win[i].parent) ? HW_BASE + (uint64_t)i * HW_STEP : 0;
        pthread_mutex_unlock(&g_lock);
        if (!h) continue;
        uint64_t args[2] = { h, param };
        if (!w32_call_guest(w, fn, 2, args) || w->exited) break;
    }
    RET(1);
}
static void u_EnumThreadWindows(w32 *w) {
    uint64_t saved = ARG(0);
    (void)saved;
    RET(1);
}
/* FindWindowEx(parent, after, class, title): a game looks for its own
 * window, or for another copy of itself to hand the command line to. */
static void find_window_ex(w32 *w, int wide) {
    uint64_t parent = ARG(0), after = ARG(1);
    char cls[96] = "", title[256] = "";
    if (ARG(2) >= 0x10000) { if (wide) w32_wtoa(w, ARG(2), cls, sizeof cls); else snprintf(cls, sizeof cls, "%.95s", w32_str(w, ARG(2))); }
    if (ARG(3) >= 0x10000) { if (wide) w32_wtoa(w, ARG(3), title, sizeof title); else snprintf(title, sizeof title, "%.255s", w32_str(w, ARG(3))); }
    int past = after == 0;
    pthread_mutex_lock(&g_lock);
    uint64_t found = 0;
    for (int i = 0; i < MAX_WINDOWS && !found; i++) {
        wwin *p = &g_win[i];
        uint64_t h = HW_BASE + (uint64_t)i * HW_STEP;
        if (!p->used) continue;
        if (!past) { if (h == after) past = 1; continue; }
        if (parent && p->parent != parent) continue;
        if (cls[0] && strcasecmp(p->cls, cls)) continue;
        if (title[0] && strcmp(p->text, title)) continue;
        found = h;
    }
    pthread_mutex_unlock(&g_lock);
    RET(found);
}
static void u_FindWindowExA(w32 *w) { find_window_ex(w, 0); }
static void u_FindWindowExW(w32 *w) { find_window_ex(w, 1); }
static void u_FindWindowW(w32 *w) {
    /* FindWindow is FindWindowEx with no parent and no predecessor, and its
     * two arguments are the last two of the four. */
    uint64_t cls = ARG(0), title = ARG(1);
    (void)cls; (void)title;
    RET(0);
}
/* WINDOWPLACEMENT: length, flags, showCmd, min point, max point, normal rect. */
static void u_GetWindowPlacement(w32 *w) {
    uint64_t p = ARG(1);
    if (!p) { RET(0); return; }
    int x = 0, y = 0, cx = 0, cy = 0;
    if (!w32_window_area(ARG(0), 1, &x, &y, &cx, &cy)) { RET(0); return; }
    w32_write(w, p, 4, 44);
    w32_write(w, p + 4, 4, 0);
    w32_write(w, p + 8, 4, 1);                       /* SW_SHOWNORMAL */
    put_rect(w, p + 28, x, y, x + cx, y + cy);
    RET(1);
}
static void u_SetWindowPlacement(w32 *w) {
    uint64_t p = ARG(1);
    if (!p) { RET(0); return; }
    int l = (int)(int32_t)w32_read(w, p + 28, 4), t = (int)(int32_t)w32_read(w, p + 32, 4);
    int r = (int)(int32_t)w32_read(w, p + 36, 4), b = (int)(int32_t)w32_read(w, p + 40, 4);
    if (r > l && b > t) move_window(w, ARG(0), l, t, r - l, b - t, 1);
    RET(1);
}
/* Layered windows: a per-window alpha. Nothing here composites, so the
 * alpha is recorded and reported back unchanged -- a game that fades its
 * window in reads back what it set and its fade completes. */
static void u_SetLayeredWindowAttributes(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    if (p) p->userdata = (ARG(1) & 0xFFFFFF) | ((ARG(2) & 0xFF) << 24) | ((ARG(3) & 0xFF) << 32);
    pthread_mutex_unlock(&g_lock);
    RET(p ? 1 : 0);
}
static void u_GetLayeredWindowAttributes(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    uint64_t v = p ? p->userdata : 0;
    pthread_mutex_unlock(&g_lock);
    if (!p) { RET(0); return; }
    if (ARG(1)) w32_write(w, ARG(1), 4, v & 0xFFFFFF);
    if (ARG(2)) w32_write(w, ARG(2), 1, (v >> 24) & 0xFF);
    if (ARG(3)) w32_write(w, ARG(3), 4, (v >> 32) & 0xFF);
    RET(1);
}
static void u_GetMonitorInfoW(w32 *w) { u_GetMonitorInfoA(w); }
static void u_EnumDisplayDevicesW(w32 *w) { (void)w; RET(0); }
/* MsgWaitForMultipleObjects: a message loop that also waits on handles. With
 * nothing else to wait for, the answer is "a message arrived" as soon as one
 * has -- and the timeout keeps a loop that has neither from spinning. */
static void u_MsgWaitForMultipleObjectsEx(w32 *w) {
    uint32_t ms = (uint32_t)ARG(3);
    uint32_t count = (uint32_t)ARG(0);
    pthread_mutex_lock(&g_lock);
    int have = g_qhead != g_qtail || g_quit;
    pthread_mutex_unlock(&g_lock);
    if (have) { RET(count); return; }              /* WAIT_OBJECT_0 + count: a message */
    if (ms) w32_host_idle();
    pthread_mutex_lock(&g_lock);
    have = g_qhead != g_qtail || g_quit;
    pthread_mutex_unlock(&g_lock);
    RET(have ? count : 0x00000102u);               /* WAIT_TIMEOUT */
}
static void u_MsgWaitForMultipleObjects(w32 *w) { u_MsgWaitForMultipleObjectsEx(w); }
/* keybd_event synthesises a key press. A game uses it to release a stuck
 * modifier, and it goes into the same queue a real key would. */
static void u_keybd_event(w32 *w) {
    int vk = (int)(ARG(0) & 0xFF);
    int up = ((uint32_t)ARG(2) & 2) != 0;          /* KEYEVENTF_KEYUP */
    w32_input_key(vk, up ? 0 : 1);
    RET(0);
}
static void u_mouse_event(w32 *w) {
    uint32_t f = (uint32_t)ARG(0);
    if (f & 0x0002) w32_input_mouse_button(0, 1);  /* LEFTDOWN */
    if (f & 0x0004) w32_input_mouse_button(0, 0);
    if (f & 0x0008) w32_input_mouse_button(1, 1);
    if (f & 0x0010) w32_input_mouse_button(1, 0);
    RET(0);
}

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

/* --- menus ---------------------------------------------------------------
 *
 * A menu used to be a handle with nothing behind it: CreateMenu returned a
 * distinct number, AppendMenu said yes, GetMenuItemCount honestly said zero,
 * and nothing was drawn or clickable. That is enough for an installer, which
 * has no menu bar and asks for its system menu only so it can grey out Close.
 * It is not enough for a program whose File/Options/Help bar is how anything
 * is reached: the bar simply was not there, and a right-click did nothing.
 *
 * The store is above, beside the list store, for the same reason: one entry
 * per menu that exists. What follows is the four separable parts -- building
 * a menu, loading one from a resource, putting one on screen, and taking the
 * clicks.
 */

/* ---- building one ------------------------------------------------------- */

static void menu_item_set(mitem *it, uint32_t flags, uint64_t idnew, const char *text) {
    memset(it, 0, sizeof *it);
    it->flags = flags & ~(uint32_t)MF_BYPOSITION;
    if (flags & MF_POPUP) it->sub = idnew;
    it->id = (uint32_t)idnew;
    snprintf(it->text, MENU_TEXT, "%s", text ? text : "");
}

/* Read the lpNewItem argument the menu calls share. It is a string only some
 * of the time: with MF_BITMAP it is a bitmap handle and with MF_OWNERDRAW it
 * is the program's own pointer, and reading either as text is how a menu call
 * turns into a fault. */
static void menu_arg_text(w32 *w, uint32_t flags, uint64_t p, int wide, char *out, size_t n) {
    out[0] = 0;
    if (!p || (flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR))) return;
    if (wide) w32_wtoa(w, p, out, n);
    else snprintf(out, n, "%.*s", (int)n - 1, w32_str(w, p));
}

/* Append, insert or replace: the three differ only in where the item lands,
 * so they are one function with a mode rather than three that drift apart.
 * `mode` is 0 append, 1 insert before `which`, 2 replace `which`. */
static uint64_t menu_put(w32 *w, uint64_t hmenu, uint32_t which, uint32_t flags,
                         uint64_t idnew, uint64_t textp, int wide, int mode) {
    char text[MENU_TEXT];
    menu_arg_text(w, flags, textp, wide, text, sizeof text);
    pthread_mutex_lock(&g_lock);
    wmenu *m = menu_of(hmenu);
    int ok = 0;
    if (m) {
        int at = m->n;
        if (mode) {
            mitem *f = menu_find(hmenu, which, flags, 0);
            /* Only an item of *this* menu can be positioned against; a
             * command that lives in a submenu is found but is not ours to
             * insert beside, and Windows appends in that case too. */
            at = (f >= m->it && f < m->it + m->n) ? (int)(f - m->it) : m->n;
        }
        if (mode == 2) {
            if (at < m->n) { menu_item_set(&m->it[at], flags, idnew, text); ok = 1; }
        } else if (m->n < MENU_ITEMS) {
            for (int i = m->n; i > at; i--) m->it[i] = m->it[i - 1];
            menu_item_set(&m->it[at], flags, idnew, text);
            m->n++;
            ok = 1;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ok ? 1 : 0;
}

/* ---- a menu out of a resource ------------------------------------------- */

/* The RT_MENU format, which has two versions exactly as RT_DIALOG does.
 *
 * Version 0 is a header of two words followed by items: a flags word, then
 * an id word for anything that is not a popup, then a NUL-terminated UTF-16
 * string; 0x0010 marks a popup, whose own items follow it inline, and 0x0080
 * marks the last item at a level. Version 1 -- MENUEX, which is what a
 * resource compiler emits the moment the script uses anything added after
 * Windows 95 -- is the same tree with wider fields and DWORD alignment.
 *
 * Both are here for the same reason both dialog templates are: a program
 * built this century usually carries the second, and reading only the first
 * would mean a menu bar that appears for old programs and not for new ones. */
static int menu_parse_res(w32 *w, tcur *c, uint64_t base, uint64_t hmenu, int ex, int depth) {
    if (depth > MENU_DEPTH + 2) return 0;
    for (;;) {
        if (c->at + 2 > c->end) return 0;
        uint32_t flags = 0, id = 0;
        int popup = 0, last = 0;
        if (ex) {
            uint32_t type = t32(c), state = t32(c);
            id = t32(c);
            uint16_t res = t16(c);
            popup = (res & 0x01) != 0;
            last  = (res & 0x80) != 0;
            /* MFT_SEPARATOR, MFS_CHECKED and MFS_GRAYED happen to have the
             * same values as their MF_ counterparts, which is the one piece
             * of luck in this format. */
            flags = (type & (uint32_t)MF_SEPARATOR) | (state & (uint32_t)(MF_CHECKED | MF_GRAYED | MF_DISABLED));
        } else {
            uint16_t f = t16(c);
            last = (f & MF_END) != 0;
            flags = f & ~(uint32_t)MF_END;
            popup = (flags & MF_POPUP) != 0;
            if (!popup) id = t16(c);
        }
        char text[MENU_TEXT];
        size_t i = 0;
        while (c->at + 2 <= c->end) {
            uint16_t ch = (uint16_t)w32_read(c->w, c->at, 2);
            c->at += 2;
            if (!ch) break;
            if (i + 1 < sizeof text) text[i++] = ch < 128 ? (char)ch : '?';
        }
        text[i] = 0;
        if (ex) {
            /* The fields after the text are DWORD aligned, and the alignment
             * is against the start of the resource rather than against
             * whatever address the image happened to be mapped at. */
            talign(c, base);
            if (popup) c->at += 4;        /* the submenu's help id, which nothing here uses */
        }
        if (popup) {
            uint64_t sub = 0;
            pthread_mutex_lock(&g_lock);
            sub = menu_new();
            pthread_mutex_unlock(&g_lock);
            if (!sub) return 0;
            if (!menu_parse_res(w, c, base, sub, ex, depth + 1)) return 0;
            pthread_mutex_lock(&g_lock);
            wmenu *m = menu_of(hmenu);
            if (m && m->n < MENU_ITEMS) menu_item_set(&m->it[m->n++], flags | MF_POPUP, sub, text);
            pthread_mutex_unlock(&g_lock);
        } else {
            pthread_mutex_lock(&g_lock);
            wmenu *m = menu_of(hmenu);
            if (m && m->n < MENU_ITEMS)
                menu_item_set(&m->it[m->n++], flags | (text[0] ? 0u : (uint32_t)MF_SEPARATOR), id, text);
            pthread_mutex_unlock(&g_lock);
        }
        if (last) return 1;
    }
}

static uint64_t load_menu(w32 *w, uint64_t inst, uint64_t name, int wide) {
    uint32_t size = 0;
    uint64_t hr = w32_find_resource(w, inst, 4 /* RT_MENU */, name, wide);
    if (!hr) return 0;
    uint64_t data = w32_resource_data(w, hr, &size);
    if (!data) return 0;
    tcur c = { w, data, data + (size ? size : 0x10000) };
    uint16_t version = t16(&c);
    uint16_t off = t16(&c);
    c.at = data + 4 + off;                /* both versions say where the items begin */
    uint64_t h;
    pthread_mutex_lock(&g_lock);
    h = menu_new();
    pthread_mutex_unlock(&g_lock);
    if (!h) return 0;
    if (!menu_parse_res(w, &c, data, h, version == 1, 0)) {
        pthread_mutex_lock(&g_lock);
        wmenu *m = menu_of(h);
        if (m) m->used = 0;
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    return h;
}

/* ---- putting one on screen ---------------------------------------------- */

/* Menus are laid out in the shell font, not in whatever font the window
 * underneath happens to be using, so the line height comes from a bare screen
 * device context. */
static int menu_line_h(void) {
    uint64_t hdc = w32_dc_for_window(0, 0, 1);
    int lh = hdc ? w32_gdi_line_height(hdc) : 13;
    if (hdc) w32_dc_release(hdc);
    return lh < 1 ? 13 : lh;
}

/* Which item of this window's bar is at a screen point, or -1. The bar is
 * measured here through the same bar_layout the painter uses, because a hit
 * test that measures differently is a menu where clicking File opens Edit. */
static int bar_item_at(uint64_t hwnd, int sx, int sy) {
    uint64_t bar = window_menu(hwnd);
    if (!bar) return -1;
    wwin snap;
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (!p || !p->visible) { pthread_mutex_unlock(&g_lock); return -1; }
    snap = *p;
    pthread_mutex_unlock(&g_lock);
    int cap_h = (!(snap.style & WS_CHILD) && (snap.style & WS_CAPTION) == WS_CAPTION)
                ? caption_height() : 0;
    int bh = menu_bar_height();
    if (sy < snap.y + cap_h || sy >= snap.y + cap_h + bh) return -1;
    if (sx < snap.x || sx >= snap.x + snap.w) return -1;
    wmenu m;
    if (!menu_snapshot(bar, &m)) return -1;
    uint64_t hdc = w32_dc_for_window(0, hwnd, 1);
    if (!hdc) return -1;
    int xs[MENU_ITEMS], ws[MENU_ITEMS];
    int n = bar_layout(hdc, &m, xs, ws);
    w32_dc_release(hdc);
    for (int i = 0; i < n; i++)
        if (sx - snap.x >= xs[i] && sx - snap.x < xs[i] + ws[i]) return i;
    return -1;
}

/* The topmost window whose bar is under this point, the way window_at picks
 * a window. */
static uint64_t bar_window_at(int sx, int sy, int *item) {
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        pthread_mutex_lock(&g_lock);
        uint64_t h = (g_win[i].used && g_win[i].visible) ? HW_BASE + (uint64_t)i * HW_STEP : 0;
        pthread_mutex_unlock(&g_lock);
        if (!h) continue;
        int it = bar_item_at(h, sx, sy);
        if (it >= 0) { *item = it; return h; }
    }
    return 0;
}

/* Drop a popup at a level. The corner is nudged back onto the display if the
 * menu would run off it -- a submenu near the right edge is flipped to the
 * left of its parent, which is what makes a deep menu tree usable at all
 * rather than half off the screen. */
static int menu_push_level(int level, uint64_t hmenu, int x, int y, int flip_from) {
    wmenu m;
    if (level < 0 || level >= MENU_DEPTH) return 0;
    if (!menu_snapshot(hmenu, &m) || !m.n) return 0;
    uint64_t hdc = w32_dc_for_window(0, 0, 1);
    if (!hdc) return 0;
    int lh = w32_gdi_line_height(hdc);
    if (lh < 1) lh = 13;
    int pw = 0, ph = 0;
    popup_measure(hdc, &m, lh, &pw, &ph);
    w32_dc_release(hdc);
    int sw = 0, sh = 0;
    w32_screen_size(&sw, &sh);
    if (x + pw > sw) x = flip_from >= 0 ? flip_from - pw : sw - pw;
    if (x < 0) x = 0;
    if (y + ph > sh) y = sh - ph;
    if (y < 0) y = 0;
    pthread_mutex_lock(&g_lock);
    g_pop.lv[level].menu = hmenu;
    g_pop.lv[level].x = x; g_pop.lv[level].y = y;
    g_pop.lv[level].w = pw; g_pop.lv[level].h = ph;
    g_pop.lv[level].hot = -1;
    g_pop.n = level + 1;
    pthread_mutex_unlock(&g_lock);
    w32_desktop_damaged();
    return 1;
}

static void menu_close(void) {
    pthread_mutex_lock(&g_lock);
    uint64_t bw = g_pop.barwnd;
    g_pop.n = 0; g_pop.barwnd = 0; g_pop.baritem = -1;
    pthread_mutex_unlock(&g_lock);
    w32_desktop_damaged();
    /* The bar keeps a highlight on whichever item was open, so it has to be
     * drawn again once nothing is. */
    if (bw) invalidate(bw);
}

/* Something was chosen. A menu command reaches the owner as WM_COMMAND with
 * the notification code zero, which is exactly how a program tells a menu
 * apart from a button (BN_CLICKED is zero too, but a menu's lParam is NULL
 * where a button's is the control). TrackPopupMenu with TPM_RETURNCMD wants
 * the number back instead, and posts nothing. */
static void menu_choose(uint32_t id) {
    pthread_mutex_lock(&g_lock);
    uint64_t owner = g_pop.owner, bw = g_pop.barwnd;
    int track = g_pop.tracking;
    uint32_t tf = g_pop.tflags;
    if (track) g_pop.chosen = (int)id;
    g_pop.n = 0; g_pop.barwnd = 0; g_pop.baritem = -1;
    pthread_mutex_unlock(&g_lock);
    w32_desktop_damaged();
    if (bw) invalidate(bw);
    if (!track || !(tf & TPM_RETURNCMD)) post_command(owner, id, 0, 0);
}

/* Open a bar item's popup, directly below the item. Opened from the keyboard
 * it starts with its first choosable item highlighted, because a keyboard
 * menu with nothing selected is one where Enter does nothing; opened with the
 * mouse it starts with nothing, because the pointer has not landed on
 * anything yet. */
static void menu_open_bar_kb(uint64_t hwnd, int item, int kb) {
    uint64_t bar = window_menu(hwnd);
    if (!bar) return;
    wmenu m;
    if (!menu_snapshot(bar, &m) || item < 0 || item >= m.n) return;
    wwin snap;
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (!p) { pthread_mutex_unlock(&g_lock); return; }
    snap = *p;
    pthread_mutex_unlock(&g_lock);
    if (m.it[item].flags & (MF_GRAYED | MF_DISABLED)) return;
    /* A bar item is normally a popup, but nothing stops one being a plain
     * command -- a "Help" that opens no menu at all -- and that is a
     * WM_COMMAND and not a menu to drop. */
    if (!(m.it[item].flags & MF_POPUP)) {
        menu_close();
        post_command(hwnd, m.it[item].id, 0, 0);
        return;
    }
    uint64_t hdc = w32_dc_for_window(0, hwnd, 1);
    if (!hdc) return;
    int xs[MENU_ITEMS], ws[MENU_ITEMS];
    bar_layout(hdc, &m, xs, ws);
    w32_dc_release(hdc);
    int cap_h = (!(snap.style & WS_CHILD) && (snap.style & WS_CAPTION) == WS_CAPTION)
                ? caption_height() : 0;
    pthread_mutex_lock(&g_lock);
    g_pop.owner = hwnd; g_pop.barwnd = hwnd; g_pop.baritem = item;
    g_pop.n = 0;
    pthread_mutex_unlock(&g_lock);
    if (!menu_push_level(0, m.it[item].sub, snap.x + xs[item],
                         snap.y + cap_h + menu_bar_height(), -1)) {
        menu_close();
        return;
    }
    if (kb) {
        wmenu sm;
        if (menu_snapshot(m.it[item].sub, &sm)) {
            for (int i = 0; i < sm.n; i++)
                if (!(sm.it[i].flags & (MF_SEPARATOR | MF_GRAYED | MF_DISABLED))) {
                    pthread_mutex_lock(&g_lock);
                    if (g_pop.n) g_pop.lv[0].hot = i;
                    pthread_mutex_unlock(&g_lock);
                    break;
                }
        }
    }
    invalidate(hwnd);
}
static void menu_open_bar(uint64_t hwnd, int item) { menu_open_bar_kb(hwnd, item, 0); }

/* Which open level a screen point is in, and which of its items. */
static int popup_hit(int sx, int sy, int *item) {
    struct { uint64_t menu; int x, y, w, h; } lv[MENU_DEPTH];
    int n;
    pthread_mutex_lock(&g_lock);
    n = g_pop.n > MENU_DEPTH ? MENU_DEPTH : g_pop.n;
    for (int i = 0; i < n; i++) {
        lv[i].menu = g_pop.lv[i].menu;
        lv[i].x = g_pop.lv[i].x; lv[i].y = g_pop.lv[i].y;
        lv[i].w = g_pop.lv[i].w; lv[i].h = g_pop.lv[i].h;
    }
    pthread_mutex_unlock(&g_lock);
    int lh = menu_line_h();
    for (int i = n - 1; i >= 0; i--) {
        if (sx < lv[i].x || sx >= lv[i].x + lv[i].w) continue;
        if (sy < lv[i].y || sy >= lv[i].y + lv[i].h) continue;
        wmenu m;
        if (!menu_snapshot(lv[i].menu, &m)) return -1;
        *item = popup_item_at(&m, lh, sy - lv[i].y);
        return i;
    }
    return -1;
}

/* An item that opens another menu opens it as soon as the highlight reaches
 * it, which is how a cascade behaves and why it needs no second click. */
static void menu_follow(int level, int item) {
    uint64_t hm;
    int px, py, pw;
    pthread_mutex_lock(&g_lock);
    if (level < 0 || level >= g_pop.n) { pthread_mutex_unlock(&g_lock); return; }
    hm = g_pop.lv[level].menu;
    px = g_pop.lv[level].x; py = g_pop.lv[level].y; pw = g_pop.lv[level].w;
    pthread_mutex_unlock(&g_lock);
    wmenu m;
    if (!menu_snapshot(hm, &m) || item < 0 || item >= m.n) return;
    if (!(m.it[item].flags & MF_POPUP)) return;
    if (m.it[item].flags & (MF_GRAYED | MF_DISABLED)) return;
    int lh = menu_line_h();
    menu_push_level(level + 1, m.it[item].sub, px + pw - 3,
                    py + popup_item_top(&m, lh, item) - MENU_PAD, px);
}

static void menu_hover(int sx, int sy) {
    int item = -1;
    int lvl = popup_hit(sx, sy, &item);
    if (lvl < 0) {
        /* Sliding along the bar with a menu down moves to the next one,
         * which is the only way a bar is usable with a mouse held. */
        uint64_t bw;
        int cur;
        pthread_mutex_lock(&g_lock);
        bw = g_pop.barwnd; cur = g_pop.baritem;
        pthread_mutex_unlock(&g_lock);
        if (bw) {
            int bi = bar_item_at(bw, sx, sy);
            if (bi >= 0 && bi != cur) menu_open_bar(bw, bi);
        }
        return;
    }
    pthread_mutex_lock(&g_lock);
    /* Leaving a submenu closes it: the levels below the one the pointer is
     * in are no longer what is being pointed at. */
    if (g_pop.n > lvl + 1) g_pop.n = lvl + 1;
    int changed = g_pop.lv[lvl].hot != item;
    g_pop.lv[lvl].hot = item;
    pthread_mutex_unlock(&g_lock);
    if (changed) w32_desktop_damaged();
    menu_follow(lvl, item);
}

static void menu_activate(int level, int item) {
    uint64_t hm;
    pthread_mutex_lock(&g_lock);
    if (level < 0 || level >= g_pop.n) { pthread_mutex_unlock(&g_lock); return; }
    hm = g_pop.lv[level].menu;
    g_pop.lv[level].hot = item;
    pthread_mutex_unlock(&g_lock);
    wmenu m;
    if (!menu_snapshot(hm, &m) || item < 0 || item >= m.n) return;
    const mitem *it = &m.it[item];
    if (it->flags & (MF_SEPARATOR | MF_GRAYED | MF_DISABLED)) return;
    if (it->flags & MF_POPUP) { menu_follow(level, item); return; }
    menu_choose(it->id);
}

static void menu_click(int sx, int sy) {
    int item = -1;
    int lvl = popup_hit(sx, sy, &item);
    if (lvl >= 0) { menu_activate(lvl, item); return; }
    /* Not in a popup. On the bar it switches or closes; anywhere else it
     * dismisses, and the click is still swallowed -- pressing the button
     * underneath as well is the thing a menu exists to prevent. */
    uint64_t bw;
    int cur;
    pthread_mutex_lock(&g_lock);
    bw = g_pop.barwnd; cur = g_pop.baritem;
    pthread_mutex_unlock(&g_lock);
    if (bw) {
        int bi = bar_item_at(bw, sx, sy);
        if (bi >= 0) {
            if (bi == cur) menu_close(); else menu_open_bar(bw, bi);
            return;
        }
    }
    menu_close();
}

/* Move the highlight within the deepest open popup, skipping the items that
 * cannot be chosen -- a highlight that stops on a separator is a highlight
 * that Enter does nothing with. */
static void menu_arrow_move(int dir) {
    uint64_t hm;
    int lvl, hot;
    pthread_mutex_lock(&g_lock);
    lvl = g_pop.n - 1;
    if (lvl < 0) { pthread_mutex_unlock(&g_lock); return; }
    hm = g_pop.lv[lvl].menu;
    hot = g_pop.lv[lvl].hot;
    pthread_mutex_unlock(&g_lock);
    wmenu m;
    if (!menu_snapshot(hm, &m) || m.n <= 0) return;
    if (hot < 0) hot = dir > 0 ? -1 : 0;
    for (int k = 0; k < m.n; k++) {
        hot = (hot + dir + m.n) % m.n;
        if (!(m.it[hot].flags & (MF_SEPARATOR | MF_GRAYED | MF_DISABLED))) break;
    }
    pthread_mutex_lock(&g_lock);
    if (lvl < g_pop.n) g_pop.lv[lvl].hot = hot;
    pthread_mutex_unlock(&g_lock);
    w32_desktop_damaged();
}

/* Sideways along the bar, which is what Left and Right do when there is
 * nothing deeper to go into. */
static void menu_bar_step(int dir) {
    uint64_t bw;
    int cur;
    pthread_mutex_lock(&g_lock);
    bw = g_pop.barwnd; cur = g_pop.baritem;
    pthread_mutex_unlock(&g_lock);
    if (!bw) return;
    wmenu m;
    uint64_t bar = window_menu(bw);
    if (!bar || !menu_snapshot(bar, &m) || m.n <= 0) return;
    menu_open_bar_kb(bw, ((cur + dir) % m.n + m.n) % m.n, 1);
}

static int is_key_msg(uint32_t msg) {
    return msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR ||
           msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_SYSCHAR;
}

/* The whole of the menu's claim on input.
 *
 * With a menu open this returns 1 for every mouse and key message, because a
 * menu is modal over the windows under it: the click that dismisses it must
 * not also press what it landed on, and the arrow keys belong to the menu
 * and not to the game behind it. With nothing open the only things it wants
 * are a click in a menu bar and Alt.
 */
static int menu_input(w32 *w, uint32_t msg, uint64_t wp, int sx, int sy) {
    (void)w;
    int open;
    pthread_mutex_lock(&g_lock);
    open = g_pop.n > 0;
    pthread_mutex_unlock(&g_lock);

    if (!open) {
        if (msg == WM_LBUTTONDOWN) {
            int item = -1;
            uint64_t hwnd = bar_window_at(sx, sy, &item);
            if (!hwnd) return 0;
            menu_open_bar(hwnd, item);
            return 1;
        }
        /* Alt on its own opens the first bar item, the way it does on
         * Windows. Alt with a letter is an access key, which is not here.
         *
         * Either message, because Alt pressed by itself is not yet a system
         * key here -- w32_input_key marks a key as one only when Alt was
         * already down when it arrived, so the Alt that starts the chord
         * comes through as an ordinary WM_KEYDOWN. */
        if ((msg == WM_SYSKEYDOWN || msg == WM_KEYDOWN) && wp == VK_MENU) {
            uint64_t f;
            pthread_mutex_lock(&g_lock);
            f = g_focus;
            pthread_mutex_unlock(&g_lock);
            if (!f || !window_menu(f)) return 0;
            menu_open_bar_kb(f, 0, 1);
            return 1;
        }
        return 0;
    }

    if (msg == WM_MOUSEMOVE) { menu_hover(sx, sy); return 1; }
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) { menu_click(sx, sy); return 1; }
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return 1;
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        switch ((int)wp) {
        case VK_ESCAPE: {
            int n;
            pthread_mutex_lock(&g_lock);
            n = g_pop.n;
            if (n > 1) g_pop.n = n - 1;
            pthread_mutex_unlock(&g_lock);
            if (n > 1) w32_desktop_damaged(); else menu_close();
            return 1;
        }
        case VK_DOWN:  menu_arrow_move(1); return 1;
        case VK_UP:    menu_arrow_move(-1); return 1;
        case VK_RIGHT: {
            int lvl, hot, deeper = 0;
            pthread_mutex_lock(&g_lock);
            lvl = g_pop.n - 1;
            hot = lvl >= 0 ? g_pop.lv[lvl].hot : -1;
            pthread_mutex_unlock(&g_lock);
            if (hot >= 0) {
                int before;
                pthread_mutex_lock(&g_lock); before = g_pop.n; pthread_mutex_unlock(&g_lock);
                menu_follow(lvl, hot);
                pthread_mutex_lock(&g_lock); deeper = g_pop.n > before; pthread_mutex_unlock(&g_lock);
                if (deeper) menu_arrow_move(1);
            }
            if (!deeper) menu_bar_step(1);
            return 1;
        }
        case VK_LEFT: {
            int n;
            pthread_mutex_lock(&g_lock);
            n = g_pop.n;
            if (n > 1) g_pop.n = n - 1;
            pthread_mutex_unlock(&g_lock);
            if (n > 1) w32_desktop_damaged(); else menu_bar_step(-1);
            return 1;
        }
        case VK_RETURN: {
            int lvl, hot;
            pthread_mutex_lock(&g_lock);
            lvl = g_pop.n - 1;
            hot = lvl >= 0 ? g_pop.lv[lvl].hot : -1;
            pthread_mutex_unlock(&g_lock);
            if (hot >= 0) menu_activate(lvl, hot);
            return 1;
        }
        default: return 1;
        }
    }
    return is_key_msg(msg) ? 1 : 0;
}

/* TrackPopupMenu's own message loop.
 *
 * The call does not return until something is chosen or the menu is
 * dismissed -- that is what makes TPM_RETURNCMD possible at all -- so this is
 * run_modal's shape again: take a message, offer it to the menu, and give
 * anything the menu did not want to the window it was addressed to, so paints
 * and timers still happen behind an open menu. Bounded the same way
 * GetMessage is, because with nobody there to click anything a headless run
 * has to end rather than wedge a CI job. */
static int track_popup_loop(w32 *w) {
    int idle = 0;
    for (;;) {
        if (w->exited) break;
        int open, quit;
        pthread_mutex_lock(&g_lock);
        open = g_pop.n > 0; quit = g_quit;
        pthread_mutex_unlock(&g_lock);
        if (!open || quit) break;

        qmsg q;
        int have = 0;
        pthread_mutex_lock(&g_lock);
        if (g_qhead != g_qtail) { q = g_q[g_qhead]; g_qhead = (g_qhead + 1) % MAX_MSGS; have = 1; }
        pthread_mutex_unlock(&g_lock);
        if (!have) {
            surface_present();
            if (++idle > GETMSG_IDLE_MS) break;
            w32_host_idle();
            continue;
        }
        idle = 0;
        int mouse = q.msg >= WM_MOUSEFIRST && q.msg <= WM_MOUSELAST;
        if (mouse || is_key_msg(q.msg)) {
            if (menu_input(w, q.msg, q.wparam, q.x, q.y)) { surface_present(); continue; }
        }
        uint64_t target = q.hwnd;
        if (!target) {
            pthread_mutex_lock(&g_lock);
            target = mouse ? window_at(q.x, q.y) : g_focus;
            pthread_mutex_unlock(&g_lock);
        }
        if (!target) continue;
        uint64_t lp = q.lparam;
        if (mouse) {
            int wx = 0, wy = 0, ww = 0, wh = 0;
            if (w32_window_area(target, 0, &wx, &wy, &ww, &wh)) lp = xy_lp(q.x - wx, q.y - wy);
        }
        deliver(w, target, q.msg, q.wparam, lp, 1);
        surface_present();
    }
    int chosen;
    pthread_mutex_lock(&g_lock);
    chosen = g_pop.chosen;
    g_pop.tracking = 0; g_pop.chosen = 0;
    g_pop.n = 0; g_pop.barwnd = 0; g_pop.baritem = -1;
    pthread_mutex_unlock(&g_lock);
    w32_desktop_damaged();
    surface_present();
    return chosen;
}

/* ---- the exports -------------------------------------------------------- */

static void u_CreateMenu(w32 *w) {
    pthread_mutex_lock(&g_lock);
    uint64_t h = menu_new();
    pthread_mutex_unlock(&g_lock);
    RET(h);
}
static void u_CreatePopupMenu(w32 *w) { u_CreateMenu(w); }

/* Destroying a menu destroys the popups hanging off it, because the program
 * holds no handle to those -- it gave them to AppendMenu and forgot them.
 * Leaving them behind is a slow leak of the one resource here that is a fixed
 * array, so a program that rebuilds its menu every time a document opens
 * would eventually run out. */
static void menu_destroy(uint64_t h, int depth) {
    if (depth > MENU_DEPTH + 2) return;
    wmenu *m = menu_of(h);
    if (!m) return;
    for (int i = 0; i < m->n; i++)
        if (m->it[i].flags & MF_POPUP) menu_destroy(m->it[i].sub, depth + 1);
    m->used = 0; m->n = 0;
}
static void u_DestroyMenu(w32 *w) {
    uint64_t h = ARG(0);
    pthread_mutex_lock(&g_lock);
    /* A window still showing this menu would keep drawing a bar out of a
     * store entry that has been handed to somebody else. */
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_win[i].used && g_win[i].menu == h) { g_win[i].menu = 0; apply_chrome(&g_win[i]); }
    menu_destroy(h, 0);
    /* A menu that is on screen when it is destroyed has to come off it, or
     * the input stays modal against a menu that no longer exists and there is
     * no way left to dismiss it. */
    for (int i = 0; i < g_pop.n; i++)
        if (!menu_of(g_pop.lv[i].menu)) { g_pop.n = 0; g_pop.barwnd = 0; g_pop.baritem = -1; break; }
    pthread_mutex_unlock(&g_lock);
    w32_desktop_damaged();
    RET(1);
}

/* One system menu, made when it is first asked for. Every caller wants the
 * same thing from it -- EnableMenuItem(SC_CLOSE, MF_GRAYED), so the window
 * cannot be closed while an install is running -- and there is no window
 * manager here to obey that, but the call has to succeed and the item has to
 * be there for it to be found. */
static void u_GetSystemMenu(w32 *w) {
    if (ARG(1)) { RET(0); return; }           /* bRevert: reset it, which we never customised */
    static uint64_t sysmenu;
    pthread_mutex_lock(&g_lock);
    if (!menu_of(sysmenu)) {
        sysmenu = menu_new();
        wmenu *m = menu_of(sysmenu);
        if (m) {
            menu_item_set(&m->it[m->n++], MF_STRING, 0xF010, "Move");
            menu_item_set(&m->it[m->n++], MF_STRING, 0xF000, "Size");
            menu_item_set(&m->it[m->n++], MF_SEPARATOR, 0, "");
            menu_item_set(&m->it[m->n++], MF_STRING, 0xF060, "Close");
        }
    }
    uint64_t h = sysmenu;
    pthread_mutex_unlock(&g_lock);
    RET(h);
}

static void u_GetMenu(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(ARG(0));
    uint64_t m = p && !(p->style & WS_CHILD) ? p->menu : 0;
    pthread_mutex_unlock(&g_lock);
    RET(m);
}
static void u_SetMenu(w32 *w) {
    uint64_t hwnd = ARG(0);
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    int ok = p != 0;
    if (p) { p->menu = ARG(1); apply_chrome(p); }
    pthread_mutex_unlock(&g_lock);
    /* The bar appeared or went away, so the client area moved: everything in
     * it has to be drawn again where it now is. */
    if (ok) invalidate(hwnd);
    RET(ok);
}
static void u_DrawMenuBar(w32 *w) {
    uint64_t hwnd = ARG(0);
    pthread_mutex_lock(&g_lock);
    wwin *p = win_of(hwnd);
    if (p) apply_chrome(p);
    pthread_mutex_unlock(&g_lock);
    invalidate(hwnd);
    RET(1);
}

static void u_GetSubMenu(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wmenu *m = menu_of(ARG(0));
    int i = (int)(int32_t)(uint32_t)ARG(1);
    uint64_t sub = (m && i >= 0 && i < m->n && (m->it[i].flags & MF_POPUP)) ? m->it[i].sub : 0;
    pthread_mutex_unlock(&g_lock);
    RET(sub);
}
static void u_GetMenuItemCount(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wmenu *m = menu_of(ARG(0));
    int n = m ? m->n : -1;
    pthread_mutex_unlock(&g_lock);
    RET((uint64_t)(uint32_t)n);
}
/* -1 for a separator or a submenu, which is what a caller walking a menu by
 * position uses to tell those apart from a command. */
static void u_GetMenuItemID(w32 *w) {
    pthread_mutex_lock(&g_lock);
    wmenu *m = menu_of(ARG(0));
    int i = (int)(int32_t)(uint32_t)ARG(1);
    uint32_t id = (uint32_t)-1;
    if (m && i >= 0 && i < m->n && !(m->it[i].flags & (MF_POPUP | MF_SEPARATOR))) id = m->it[i].id;
    pthread_mutex_unlock(&g_lock);
    RET(id);
}
static void u_GetMenuState(w32 *w) {
    pthread_mutex_lock(&g_lock);
    mitem *it = menu_find(ARG(0), (uint32_t)ARG(1), (uint32_t)ARG(2), 0);
    uint32_t s = (uint32_t)-1;
    if (it) {
        s = it->flags;
        /* For a popup the high word is its item count, which is the only way
         * a caller can size a submenu without a handle to it. */
        if (it->flags & MF_POPUP) {
            wmenu *sm = menu_of(it->sub);
            if (sm) s |= (uint32_t)sm->n << 8;
        }
    }
    pthread_mutex_unlock(&g_lock);
    RET(s);
}

static void get_menu_string(w32 *w, int wide) {
    char text[MENU_TEXT];
    text[0] = 0;
    pthread_mutex_lock(&g_lock);
    mitem *it = menu_find(ARG(0), (uint32_t)ARG(1), (uint32_t)ARG(4), 0);
    if (it) snprintf(text, sizeof text, "%s", it->text);
    int found = it != 0;
    pthread_mutex_unlock(&g_lock);
    if (!found) { RET(0); return; }
    size_t n = strlen(text);
    uint64_t out = ARG(2);
    int cap = (int)(int32_t)(uint32_t)ARG(3);
    /* A null buffer asks for the length, which is what every caller does
     * first so it knows how much to allocate. */
    if (!out || cap <= 0) { RET(n); return; }
    if (n > (size_t)cap - 1) n = (size_t)cap - 1;
    for (size_t i = 0; i < n; i++)
        w32_write(w, out + (wide ? i * 2 : i), wide ? 2 : 1, (uint8_t)text[i]);
    w32_write(w, out + (wide ? n * 2 : n), wide ? 2 : 1, 0);
    RET(n);
}
static void u_GetMenuStringA(w32 *w) { get_menu_string(w, 0); }
static void u_GetMenuStringW(w32 *w) { get_menu_string(w, 1); }

/* Toggling a flag on one item, which CheckMenuItem and EnableMenuItem both
 * are. Both return the item's previous state, or -1 if it is not there --
 * and a caller that ignores the difference between "was unchecked" and "not
 * found" is why the -1 matters. */
static uint32_t menu_flip(uint64_t hmenu, uint32_t which, uint32_t flags, uint32_t mask) {
    pthread_mutex_lock(&g_lock);
    mitem *it = menu_find(hmenu, which, flags, 0);
    uint32_t was = (uint32_t)-1;
    if (it) {
        was = it->flags & mask;
        it->flags = (it->flags & ~mask) | (flags & mask);
    }
    pthread_mutex_unlock(&g_lock);
    w32_desktop_damaged();
    return was;
}
static void u_CheckMenuItem(w32 *w) {
    RET(menu_flip(ARG(0), (uint32_t)ARG(1), (uint32_t)ARG(2), MF_CHECKED));
}
static void u_EnableMenuItem(w32 *w) {
    RET(menu_flip(ARG(0), (uint32_t)ARG(1), (uint32_t)ARG(2), MF_GRAYED | MF_DISABLED));
}
/* One of a run of items is checked and the rest are cleared, which is the
 * whole of what makes a menu radio group behave like one. */
static void u_CheckMenuRadioItem(w32 *w) {
    uint64_t h = ARG(0);
    uint32_t first = (uint32_t)ARG(1), last = (uint32_t)ARG(2), pick = (uint32_t)ARG(3);
    uint32_t flags = (uint32_t)ARG(4);
    int ok = 0;
    pthread_mutex_lock(&g_lock);
    wmenu *m = menu_of(h);
    if (m) {
        if (flags & MF_BYPOSITION) {
            for (uint32_t i = first; i <= last && (int)i < m->n; i++)
                m->it[i].flags = (m->it[i].flags & ~(uint32_t)MF_CHECKED) | (i == pick ? MF_CHECKED : 0);
        } else {
            for (int i = 0; i < m->n; i++) {
                uint32_t id = m->it[i].id;
                if (id < first || id > last) continue;
                m->it[i].flags = (m->it[i].flags & ~(uint32_t)MF_CHECKED) | (id == pick ? MF_CHECKED : 0);
            }
        }
        ok = 1;
    }
    pthread_mutex_unlock(&g_lock);
    w32_desktop_damaged();
    RET(ok);
}

static void u_AppendMenuA(w32 *w) { RET(menu_put(w, ARG(0), 0, (uint32_t)ARG(1), ARG(2), ARG(3), 0, 0)); }
static void u_AppendMenuW(w32 *w) { RET(menu_put(w, ARG(0), 0, (uint32_t)ARG(1), ARG(2), ARG(3), 1, 0)); }
static void u_InsertMenuA(w32 *w) { RET(menu_put(w, ARG(0), (uint32_t)ARG(1), (uint32_t)ARG(2), ARG(3), ARG(4), 0, 1)); }
static void u_InsertMenuW(w32 *w) { RET(menu_put(w, ARG(0), (uint32_t)ARG(1), (uint32_t)ARG(2), ARG(3), ARG(4), 1, 1)); }
static void u_ModifyMenuA(w32 *w) { RET(menu_put(w, ARG(0), (uint32_t)ARG(1), (uint32_t)ARG(2), ARG(3), ARG(4), 0, 2)); }
static void u_ModifyMenuW(w32 *w) { RET(menu_put(w, ARG(0), (uint32_t)ARG(1), (uint32_t)ARG(2), ARG(3), ARG(4), 1, 2)); }

/* DeleteMenu destroys a submenu it removes; RemoveMenu leaves it alive, so
 * the program can put it somewhere else. That is the entire difference
 * between them and it is the reason both exist. */
static uint64_t menu_take_out(uint64_t hmenu, uint32_t which, uint32_t flags, int destroy) {
    pthread_mutex_lock(&g_lock);
    wmenu *m = menu_of(hmenu);
    mitem *it = menu_find(hmenu, which, flags, 0);
    int ok = 0;
    if (m && it >= m->it && it < m->it + m->n) {
        int at = (int)(it - m->it);
        if (destroy && (it->flags & MF_POPUP)) menu_destroy(it->sub, 0);
        for (int i = at; i + 1 < m->n; i++) m->it[i] = m->it[i + 1];
        m->n--;
        ok = 1;
    }
    pthread_mutex_unlock(&g_lock);
    w32_desktop_damaged();
    return ok;
}
static void u_DeleteMenu(w32 *w) { RET(menu_take_out(ARG(0), (uint32_t)ARG(1), (uint32_t)ARG(2), 1)); }
static void u_RemoveMenu(w32 *w) { RET(menu_take_out(ARG(0), (uint32_t)ARG(1), (uint32_t)ARG(2), 0)); }

/* MENUITEMINFO. The offsets were read out of mingw-w64's headers with
 * offsetof, by a program compiled for Windows and run on this emulator, the
 * same way the window structures were: hSubMenu is pointer-sized and every
 * field after it moves between the two bitnesses. */
typedef struct { int mask, type, state, id, sub, data, typedata, cch; } mii_off;
static const mii_off MII32 = { 4, 8, 12, 16, 20, 32, 36, 40 };
static const mii_off MII64 = { 4, 8, 12, 16, 24, 48, 56, 64 };

static void set_menu_item_info(w32 *w, int wide) {
    const mii_off *o = w->is32 ? &MII32 : &MII64;
    uint64_t p = ARG(3);
    if (!p) { RET(0); return; }
    uint32_t mask = (uint32_t)w32_read(w, p + o->mask, 4);
    uint32_t type = (uint32_t)w32_read(w, p + o->type, 4);
    uint32_t state = (uint32_t)w32_read(w, p + o->state, 4);
    uint32_t id = (uint32_t)w32_read(w, p + o->id, 4);
    uint64_t sub = w32_read(w, p + o->sub, w32_ptrsize(w));
    uint64_t td = w32_read(w, p + o->typedata, w32_ptrsize(w));
    char text[MENU_TEXT];
    text[0] = 0;
    if ((mask & (MIIM_STRING | MIIM_TYPE)) && td && !(type & (MF_BITMAP | MF_OWNERDRAW)))
        menu_arg_text(w, 0, td, wide, text, sizeof text);
    pthread_mutex_lock(&g_lock);
    mitem *it = menu_find(ARG(0), (uint32_t)ARG(1), ARG(2) ? MF_BYPOSITION : MF_BYCOMMAND, 0);
    int ok = it != 0;
    if (it) {
        if (mask & (MIIM_TYPE | MIIM_FTYPE))
            it->flags = (it->flags & ~(uint32_t)MF_SEPARATOR) | (type & (uint32_t)MF_SEPARATOR);
        if (mask & MIIM_STATE)
            it->flags = (it->flags & ~(uint32_t)(MF_CHECKED | MF_GRAYED | MF_DISABLED)) |
                        (state & (uint32_t)(MF_CHECKED | MF_GRAYED | MF_DISABLED));
        if (mask & MIIM_ID) it->id = id;
        if (mask & MIIM_SUBMENU) {
            it->sub = sub;
            it->flags = sub ? (it->flags | MF_POPUP) : (it->flags & ~(uint32_t)MF_POPUP);
        }
        if ((mask & (MIIM_STRING | MIIM_TYPE)) && text[0]) snprintf(it->text, MENU_TEXT, "%s", text);
    }
    pthread_mutex_unlock(&g_lock);
    w32_desktop_damaged();
    RET(ok);
}
static void u_SetMenuItemInfoA(w32 *w) { set_menu_item_info(w, 0); }
static void u_SetMenuItemInfoW(w32 *w) { set_menu_item_info(w, 1); }

static void get_menu_item_info(w32 *w, int wide) {
    const mii_off *o = w->is32 ? &MII32 : &MII64;
    uint64_t p = ARG(3);
    if (!p) { RET(0); return; }
    uint32_t mask = (uint32_t)w32_read(w, p + o->mask, 4);
    char text[MENU_TEXT];
    uint32_t flags = 0, id = 0;
    uint64_t sub = 0;
    pthread_mutex_lock(&g_lock);
    mitem *it = menu_find(ARG(0), (uint32_t)ARG(1), ARG(2) ? MF_BYPOSITION : MF_BYCOMMAND, 0);
    int ok = it != 0;
    if (it) {
        flags = it->flags; id = it->id; sub = it->sub;
        snprintf(text, sizeof text, "%s", it->text);
    } else text[0] = 0;
    pthread_mutex_unlock(&g_lock);
    if (!ok) { RET(0); return; }
    if (mask & (MIIM_TYPE | MIIM_FTYPE)) w32_write(w, p + o->type, 4, flags & MF_SEPARATOR);
    if (mask & MIIM_STATE)
        w32_write(w, p + o->state, 4, flags & (MF_CHECKED | MF_GRAYED | MF_DISABLED));
    if (mask & MIIM_ID) w32_write(w, p + o->id, 4, id);
    if (mask & MIIM_SUBMENU) w32_write(w, p + o->sub, w32_ptrsize(w), sub);
    if (mask & (MIIM_STRING | MIIM_TYPE)) {
        uint64_t td = w32_read(w, p + o->typedata, w32_ptrsize(w));
        uint32_t cap = (uint32_t)w32_read(w, p + o->cch, 4);
        size_t n = strlen(text);
        /* cch comes back as the length whether or not there was a buffer,
         * which is how a caller asks how much room the text needs. */
        w32_write(w, p + o->cch, 4, (uint32_t)n);
        if (td && cap) {
            if (n > cap - 1) n = cap - 1;
            for (size_t i = 0; i < n; i++)
                w32_write(w, td + (wide ? i * 2 : i), wide ? 2 : 1, (uint8_t)text[i]);
            w32_write(w, td + (wide ? n * 2 : n), wide ? 2 : 1, 0);
        }
    }
    RET(1);
}
static void u_GetMenuItemInfoA(w32 *w) { get_menu_item_info(w, 0); }
static void u_GetMenuItemInfoW(w32 *w) { get_menu_item_info(w, 1); }

/* InsertMenuItem is InsertMenu with the information in a structure instead of
 * in the arguments, so it goes in through the same door: build the item, then
 * put it where the position argument says. */
static void insert_menu_item(w32 *w, int wide) {
    const mii_off *o = w->is32 ? &MII32 : &MII64;
    uint64_t p = ARG(3);
    if (!p) { RET(0); return; }
    uint32_t mask = (uint32_t)w32_read(w, p + o->mask, 4);
    uint32_t type = (uint32_t)w32_read(w, p + o->type, 4);
    uint32_t state = (uint32_t)w32_read(w, p + o->state, 4);
    uint32_t id = (uint32_t)w32_read(w, p + o->id, 4);
    uint64_t sub = (mask & MIIM_SUBMENU) ? w32_read(w, p + o->sub, w32_ptrsize(w)) : 0;
    uint64_t td = w32_read(w, p + o->typedata, w32_ptrsize(w));
    char text[MENU_TEXT];
    text[0] = 0;
    if (td && !(type & (MF_BITMAP | MF_OWNERDRAW))) menu_arg_text(w, 0, td, wide, text, sizeof text);
    uint32_t flags = (type & (uint32_t)MF_SEPARATOR) |
                     (state & (uint32_t)(MF_CHECKED | MF_GRAYED | MF_DISABLED)) |
                     (sub ? (uint32_t)MF_POPUP : 0u);
    uint32_t where = (uint32_t)ARG(1);
    int bypos = ARG(2) != 0;
    pthread_mutex_lock(&g_lock);
    wmenu *m = menu_of(ARG(0));
    int ok = 0;
    if (m && m->n < MENU_ITEMS) {
        int at = m->n;
        if (bypos) { if (where < (uint32_t)m->n) at = (int)where; }
        else {
            mitem *f = menu_find(ARG(0), where, MF_BYCOMMAND, 0);
            if (f >= m->it && f < m->it + m->n) at = (int)(f - m->it);
        }
        for (int i = m->n; i > at; i--) m->it[i] = m->it[i - 1];
        menu_item_set(&m->it[at], flags, sub ? sub : id, text);
        if (sub) m->it[at].id = id;
        m->n++;
        ok = 1;
    }
    pthread_mutex_unlock(&g_lock);
    w32_desktop_damaged();
    RET(ok);
}
static void u_InsertMenuItemA(w32 *w) { insert_menu_item(w, 0); }
static void u_InsertMenuItemW(w32 *w) { insert_menu_item(w, 1); }

static void u_LoadMenuA(w32 *w) { RET(load_menu(w, ARG(0), ARG(1), 0)); }
static void u_LoadMenuW(w32 *w) { RET(load_menu(w, ARG(0), ARG(1), 1)); }

/* TrackPopupMenu(hMenu, uFlags, x, y, nReserved, hWnd, prcRect) -- a context
 * menu at a screen point. The alignment flags move the menu relative to that
 * point, which matters for the one that is actually used: a menu asked for at
 * the bottom of the screen with TPM_BOTTOMALIGN goes above the point rather
 * than off the display. */
static void track_popup(w32 *w, int ex) {
    uint64_t hmenu = ARG(0);
    uint32_t flags = (uint32_t)ARG(1);
    int x = (int)(int32_t)(uint32_t)ARG(2), y = (int)(int32_t)(uint32_t)ARG(3);
    uint64_t owner = ex ? ARG(4) : ARG(5);
    wmenu m;
    if (!menu_snapshot(hmenu, &m) || !m.n) { RET(0); return; }
    uint64_t hdc = w32_dc_for_window(w, 0, 1);
    if (!hdc) { RET(0); return; }
    int lh = w32_gdi_line_height(hdc);
    if (lh < 1) lh = 13;
    int pw = 0, ph = 0;
    popup_measure(hdc, &m, lh, &pw, &ph);
    w32_dc_release(hdc);
    if (flags & TPM_CENTERALIGN) x -= pw / 2;
    else if (flags & TPM_RIGHTALIGN) x -= pw;
    if (flags & TPM_VCENTERALIGN) y -= ph / 2;
    else if (flags & TPM_BOTTOMALIGN) y -= ph;

    pthread_mutex_lock(&g_lock);
    g_pop.owner = owner; g_pop.barwnd = 0; g_pop.baritem = -1;
    g_pop.tracking = 1; g_pop.tflags = flags; g_pop.chosen = 0;
    g_pop.n = 0;
    pthread_mutex_unlock(&g_lock);
    if (!menu_push_level(0, hmenu, x, y, -1)) {
        pthread_mutex_lock(&g_lock); g_pop.tracking = 0; pthread_mutex_unlock(&g_lock);
        RET(0);
        return;
    }
    int chosen = track_popup_loop(w);
    /* Without TPM_RETURNCMD the command was posted and the answer is only
     * "the menu was shown", which is what every caller of that form tests. */
    RET((flags & TPM_RETURNCMD) ? (uint64_t)(uint32_t)chosen : 1);
}
static void u_TrackPopupMenu(w32 *w)   { track_popup(w, 0); }
static void u_TrackPopupMenuEx(w32 *w) { track_popup(w, 1); }

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

/* LoadBitmap is LoadImage with the type fixed and no size: the old call. */
static void u_LoadBitmapA(w32 *w) {
    uint64_t hr = w32_find_resource(w, ARG(0), 2, ARG(1), 0);
    uint32_t size = 0;
    w32_image im = { 0, 0, 0 };
    uint64_t data = hr ? w32_resource_data(w, hr, &size) : 0;
    if (data && w32_dib_decode((const uint8_t *)W32P(w, data), size, &im))
        RET(w32_gdi_make_bitmap(im.w, im.h, im.px));
    else RET(0);
}
static void u_LoadBitmapW(w32 *w) {
    uint64_t hr = w32_find_resource(w, ARG(0), 2, ARG(1), 1);
    uint32_t size = 0;
    w32_image im = { 0, 0, 0 };
    uint64_t data = hr ? w32_resource_data(w, hr, &size) : 0;
    if (data && w32_dib_decode((const uint8_t *)W32P(w, data), size, &im))
        RET(w32_gdi_make_bitmap(im.w, im.h, im.px));
    else RET(0);
}
static void u_DestroyIcon(w32 *w) { w32_gdi_delete_object(ARG(0)); RET(1); }
static void u_DestroyCursor(w32 *w) { w32_gdi_delete_object(ARG(0)); RET(1); }

/* DrawIconEx(hdc, x, y, hicon, cx, cy, step, brush, flags). DrawIcon is the
 * same at the icon's own size. */
static void u_DrawIconEx(w32 *w) {
    w32_gdi_draw_image(ARG(0), ARG(3), (int)(int32_t)(uint32_t)ARG(1),
                       (int)(int32_t)(uint32_t)ARG(2),
                       (int)(int32_t)(uint32_t)ARG(4), (int)(int32_t)(uint32_t)ARG(5));
    RET(1);
}
static void u_DrawIcon(w32 *w) {
    w32_gdi_draw_image(ARG(0), ARG(3), (int)(int32_t)(uint32_t)ARG(1),
                       (int)(int32_t)(uint32_t)ARG(2), 0, 0);
    RET(1);
}
/* GetIconInfo(hicon, ICONINFO*): fIcon, xHotspot, yHotspot, then two bitmap
 * handles. A program calls this to find out how big an icon is. */
static void u_GetIconInfo(w32 *w) {
    int cx = 0, cy = 0, hx = 0, hy = 0;
    if (!ARG(1) || !w32_gdi_icon_size(ARG(0), &cx, &cy, &hx, &hy)) { RET(0); return; }
    w32_write(w, ARG(1), 4, (uint64_t)(hx == 0 && hy == 0));
    w32_write(w, ARG(1) + 4, 4, (uint64_t)(uint32_t)hx);
    w32_write(w, ARG(1) + 8, 4, (uint64_t)(uint32_t)hy);
    RET(1);
}
static void u_CopyImage(w32 *w) { RET(ARG(0)); }
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
    F(LoadCursorA, 2), F(LoadCursorW, 2), F(LoadIconA, 2), F(LoadIconW, 2),
    F(LoadImageA, 6), F(LoadImageW, 6), F(GetCursor, 0),
    F(DrawIcon, 4), F(DrawIconEx, 9), F(GetIconInfo, 2), F(CopyImage, 5),
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
    /* rectangles, which a game does its own layout with */
    F(IntersectRect, 3), F(UnionRect, 3), F(IsRectEmpty, 1), F(PtInRect, 3),
    F(SetRect, 5), F(OffsetRect, 3), F(MapWindowPoints, 4),
    /* finding and placing windows */
    F(EnumWindows, 2), F(EnumThreadWindows, 3),
    F(FindWindowExA, 4), F(FindWindowExW, 4), F(FindWindowW, 2),
    F(GetWindowPlacement, 2), F(SetWindowPlacement, 2), F(BringWindowToTop, 1),
    F(SetLayeredWindowAttributes, 4), F(GetLayeredWindowAttributes, 4),
    F(GetMonitorInfoW, 2), F(EnumDisplayDevicesW, 4),
    F(MsgWaitForMultipleObjectsEx, 5), F(MsgWaitForMultipleObjects, 5),
    F(keybd_event, 4), F(mouse_event, 5),
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
    /* Menus. Argument counts matter here and nowhere more: a stdcall callee
     * pops its own arguments, so one of these being wrong corrupts the
     * caller's stack somewhere far away from the call. */
    F(CreateMenu, 0), F(CreatePopupMenu, 0), F(DestroyMenu, 1), F(GetSystemMenu, 2),
    F(GetMenu, 1), F(SetMenu, 2), F(GetSubMenu, 2),
    F(GetMenuItemCount, 1), F(GetMenuItemID, 2), F(GetMenuState, 3),
    F(GetMenuStringA, 5), F(GetMenuStringW, 5),
    F(EnableMenuItem, 3), F(CheckMenuItem, 3), F(CheckMenuRadioItem, 5),
    F(DeleteMenu, 3), F(RemoveMenu, 3),
    F(AppendMenuA, 4), F(AppendMenuW, 4),
    F(InsertMenuA, 5), F(InsertMenuW, 5),
    F(ModifyMenuA, 5), F(ModifyMenuW, 5),
    F(InsertMenuItemA, 4), F(InsertMenuItemW, 4),
    F(SetMenuItemInfoA, 4), F(SetMenuItemInfoW, 4),
    F(GetMenuItemInfoA, 4), F(GetMenuItemInfoW, 4),
    F(LoadMenuA, 2), F(LoadMenuW, 2), F(DrawMenuBar, 1),
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
