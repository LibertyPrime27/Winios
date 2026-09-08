/* DirectInput: the keyboard, the mouse, and a gamepad, the way a game from
 * before XInput asks for them.
 *
 * Three devices, always present. The keyboard and the mouse are the ones
 * user32.c already tracks for GetAsyncKeyState and the message queue, read
 * here in DirectInput's own shapes: the 256 DIK scancodes, the relative
 * DIMOUSESTATE, the DIJOYSTATE whose axes and buttons come from the same
 * keyboard-as-pad that XInput reports, so a game that only reads a joystick
 * is playable from the keyboard or the on-screen keys. Buffered data --
 * GetDeviceData -- is produced by diffing the state at each look, which is
 * what a device without an interrupt line can do, and is enough for a game
 * that reads a queue of key transitions instead of a state.
 *
 * Both DirectInput 8 (dinput8.dll) and the older dinput.dll interfaces are
 * here, A and W. The vtable slots come from vtables_gen.h, which comes from
 * the header; nothing here counts them by hand. Force feedback, effects,
 * action maps and control panels are not implemented and say so through the
 * documented codes rather than by pretending.
 */
#define _GNU_SOURCE
#include "w32.h"
#include "vtables_gen.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { S_OK_ = 0, S_FALSE_ = 1,
       E_NOTIMPL_ = (int)0x80004001, E_INVALIDARG_ = (int)0x80070057, E_POINTER_ = (int)0x80004003,
       DIERR_NOTACQUIRED_ = (int)0x8007000C, DIERR_ACQUIRED_ = (int)0x800700AA,
       DIERR_NOTBUFFERED_ = (int)0x80040207, DIERR_DEVICENOTREG_ = (int)0x80040154,
       DIERR_OBJECTNOTFOUND_ = (int)0x80070002 };
enum { DEV_KEYBOARD = 1, DEV_MOUSE = 2, DEV_JOYSTICK = 3 };
enum { DIENUM_STOP_ = 0 };
enum { DIDFT_RELAXIS_ = 1, DIDFT_ABSAXIS_ = 2, DIDFT_AXIS_ = 3, DIDFT_PSHBUTTON_ = 4, DIDFT_TGLBUTTON_ = 8,
       DIDFT_BUTTON_ = 12, DIDFT_POV_ = 0x10, DIDFT_ALL_ = 0 };
enum { DIPH_DEVICE_ = 0, DIPH_BYOFFSET_ = 1, DIPH_BYID_ = 2 };
enum { DIPROP_BUFFERSIZE_ = 1, DIPROP_AXISMODE_ = 2, DIPROP_GRANULARITY_ = 3, DIPROP_RANGE_ = 4,
       DIPROP_DEADZONE_ = 5, DIPROP_SATURATION_ = 6, DIPROP_FFGAIN_ = 7, DIPROP_INSTANCENAME_ = 13,
       DIPROP_PRODUCTNAME_ = 14, DIPROP_VIDPID_ = 15 };
enum { MAX_DEV = 16, QLEN = 256, MAX_PATH_ = 260 };

/* GUIDs from dinput.h, as bytes. */
static const uint8_t GUID_SysMouse_[16]    = W32_GUID(0x6F1D2B60, 0xD5A0, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
static const uint8_t GUID_SysKeyboard_[16] = W32_GUID(0x6F1D2B61, 0xD5A0, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
static const uint8_t GUID_Joystick_[16]    = W32_GUID(0x6F1D2B70, 0xD5A0, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
static const uint8_t GUID_XAxis_[16]  = W32_GUID(0xA36D02E0, 0xC9F3, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
static const uint8_t GUID_YAxis_[16]  = W32_GUID(0xA36D02E1, 0xC9F3, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
static const uint8_t GUID_ZAxis_[16]  = W32_GUID(0xA36D02E2, 0xC9F3, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
static const uint8_t GUID_RxAxis_[16] = W32_GUID(0xA36D02F4, 0xC9F3, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
static const uint8_t GUID_RyAxis_[16] = W32_GUID(0xA36D02F5, 0xC9F3, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
static const uint8_t GUID_RzAxis_[16] = W32_GUID(0xA36D02E3, 0xC9F3, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
static const uint8_t GUID_Button_[16] = W32_GUID(0xA36D02F0, 0xC9F3, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
static const uint8_t GUID_Key_[16]    = W32_GUID(0x55728220, 0xD33C, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
static const uint8_t GUID_POV_[16]    = W32_GUID(0xA36D02F2, 0xC9F3, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
static const uint8_t IID_IDirectInput8W_[16] = W32_GUID(0xBF798031, 0x483A, 0x4DA2, 0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00);
static const uint8_t IID_IDirectInput7W_[16] = W32_GUID(0x9A4CB685, 0x236D, 0x11D3, 0x8E,0x9D,0x00,0xC0,0x4F,0x68,0x44,0xAE);
static const uint8_t IID_IDirectInputW_[16]  = W32_GUID(0x89521361, 0xAA8A, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
static const uint8_t CLSID_DirectInput8_[16] = W32_GUID(0x25E609E4, 0xB259, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
static const uint8_t CLSID_DirectInput_[16]  = W32_GUID(0x25E609E0, 0xB259, 0x11CF, 0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);

/* Object fields (64-bit slots). A DirectInput object: F_WIDE only. A device:
 * all of them. */
enum { F_WIDE = 0, F_KIND = 1, F_ACQUIRED = 2, F_BUFSIZE = 3, F_EVENT = 4, F_FMTSIZE = 5, F_AXISMODE = 6,
       F_SLOT = 7, F_COOP = 8, F_LMIN = 9, F_LMAX = 10, F_LEGACY = 11, F_NFIELDS = 12 };
enum { TAG_DI = 0x44490000, TAG_DIDEV };

/* The host side of a device: the state it last reported and the events since. */
typedef struct {
    int      used;
    uint8_t  keys[256];                 /* DIK, 0x80 down */
    int32_t  mx, my, mw;                /* the user32 totals last seen */
    int32_t  ax[8];                     /* joystick axes and pov as last reported */
    uint8_t  buttons[32];
    int32_t  last_rel[3];               /* what GetDeviceState last reported the totals as (relative mode) */
    uint32_t seq;
    int      n, head, overflow;
    struct { uint32_t ofs, data, ts, seq; } q[QLEN];
} dev_state;
static dev_state g_dev[MAX_DEV];

static w32_com_class cls_di8, cls_di7, cls_dev8, cls_dev7;
static w32_api di8_methods[DI8_NSLOTS], di7_methods[DI7_NSLOTS], dev8_methods[DID8_NSLOTS], dev7_methods[DID7_NSLOTS];
static int g_built;

static uint32_t now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000); }

/* Virtual keys to DIK scancodes, US layout: the table a game's key-binding
 * screen is written against. Zero means "no DirectInput code". */
static uint8_t g_vk2dik[256];
static void build_vk2dik(void) {
    static const struct { uint8_t vk, dik; } t[] = {
        {0x08,0x0E},{0x09,0x0F},{0x0D,0x1C},{0x10,0x2A},{0x11,0x1D},{0x12,0x38},{0x13,0xC5},{0x14,0x3A},{0x1B,0x01},
        {0x20,0x39},{0x21,0xC9},{0x22,0xD1},{0x23,0xCF},{0x24,0xC7},{0x25,0xCB},{0x26,0xC8},{0x27,0xCD},{0x28,0xD0},
        {0x2C,0xB7},{0x2D,0xD2},{0x2E,0xD3},{0x30,0x0B},{0x31,0x02},{0x32,0x03},{0x33,0x04},{0x34,0x05},{0x35,0x06},
        {0x36,0x07},{0x37,0x08},{0x38,0x09},{0x39,0x0A},
        {'A',0x1E},{'B',0x30},{'C',0x2E},{'D',0x20},{'E',0x12},{'F',0x21},{'G',0x22},{'H',0x23},{'I',0x17},{'J',0x24},
        {'K',0x25},{'L',0x26},{'M',0x32},{'N',0x31},{'O',0x18},{'P',0x19},{'Q',0x10},{'R',0x13},{'S',0x1F},{'T',0x14},
        {'U',0x16},{'V',0x2F},{'W',0x11},{'X',0x2D},{'Y',0x15},{'Z',0x2C},
        {0x5B,0xDB},{0x5C,0xDC},{0x5D,0xDD},
        {0x60,0x52},{0x61,0x4F},{0x62,0x50},{0x63,0x51},{0x64,0x4B},{0x65,0x4C},{0x66,0x4D},{0x67,0x47},{0x68,0x48},{0x69,0x49},
        {0x6A,0x37},{0x6B,0x4E},{0x6D,0x4A},{0x6E,0x53},{0x6F,0xB5},
        {0x70,0x3B},{0x71,0x3C},{0x72,0x3D},{0x73,0x3E},{0x74,0x3F},{0x75,0x40},{0x76,0x41},{0x77,0x42},{0x78,0x43},{0x79,0x44},
        {0x7A,0x57},{0x7B,0x58},{0x7C,0x64},{0x7D,0x65},{0x7E,0x66},
        {0x90,0x45},{0x91,0x46},{0xA0,0x2A},{0xA1,0x36},{0xA2,0x1D},{0xA3,0x9D},{0xA4,0x38},{0xA5,0xB8},
        {0xBA,0x27},{0xBB,0x0D},{0xBC,0x33},{0xBD,0x0C},{0xBE,0x34},{0xBF,0x35},{0xC0,0x29},{0xDB,0x1A},{0xDC,0x2B},{0xDD,0x1B},{0xDE,0x28},
    };
    memset(g_vk2dik, 0, sizeof g_vk2dik);
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) g_vk2dik[t[i].vk] = t[i].dik;
}
static const char *dik_name(int dik) {
    static char buf[16];
    static const struct { uint8_t dik; const char *n; } t[] = {
        {0x01,"Esc"},{0x0E,"Backspace"},{0x0F,"Tab"},{0x1C,"Enter"},{0x1D,"Left Ctrl"},{0x2A,"Left Shift"},{0x36,"Right Shift"},
        {0x38,"Left Alt"},{0x39,"Space"},{0x3A,"Caps Lock"},{0x45,"Num Lock"},{0x46,"Scroll Lock"},{0x9D,"Right Ctrl"},
        {0xB8,"Right Alt"},{0xC5,"Pause"},{0xC7,"Home"},{0xC8,"Up"},{0xC9,"Page Up"},{0xCB,"Left"},{0xCD,"Right"},
        {0xCF,"End"},{0xD0,"Down"},{0xD1,"Page Down"},{0xD2,"Insert"},{0xD3,"Delete"},{0xDB,"Left Windows"},{0xDC,"Right Windows"},
        {0xDD,"Application"},{0x0B,"0"},{0x0C,"-"},{0x0D,"="},{0x1A,"["},{0x1B,"]"},{0x27,";"},{0x28,"'"},{0x29,"`"},{0x2B,"\\"},
        {0x33,","},{0x34,"."},{0x35,"/"},{0x37,"Num *"},{0x4A,"Num -"},{0x4E,"Num +"},{0x53,"Num ."},{0xB5,"Num /"},{0xB7,"Print Screen"},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i].dik == dik) return t[i].n;
    if (dik >= 0x02 && dik <= 0x0A) { snprintf(buf, sizeof buf, "%d", dik - 1); return buf; }
    if (dik >= 0x3B && dik <= 0x44) { snprintf(buf, sizeof buf, "F%d", dik - 0x3A); return buf; }
    if (dik == 0x57 || dik == 0x58) { snprintf(buf, sizeof buf, "F%d", dik - 0x57 + 11); return buf; }
    if (dik >= 0x47 && dik <= 0x53) { static const char *np[] = {"Num 7","Num 8","Num 9",0,"Num 4","Num 5","Num 6",0,"Num 1","Num 2","Num 3","Num 0",0}; if (np[dik - 0x47]) return np[dik - 0x47]; }
    for (int vk = 'A'; vk <= 'Z'; vk++) if (g_vk2dik[vk] == dik) { buf[0] = (char)vk; buf[1] = 0; return buf; }
    snprintf(buf, sizeof buf, "Key %02X", dik);
    return buf;
}

/* --- reading the devices ---------------------------------------------------- */

static void read_keyboard(uint8_t out[256]) {
    memset(out, 0, 256);
    for (int vk = 0; vk < 256; vk++) if (g_vk2dik[vk] && w32_key_down(vk)) out[g_vk2dik[vk]] = 0x80;
}
/* The pad as DIJOYSTATE axes: DirectInput's Y grows downward, XInput's up; the
 * triggers share the Z axis as most pads report them; the D-pad is the POV. */
static void read_joystick(const uint64_t *fields, int32_t ax[8], uint8_t buttons[32]) {
    w32_pad p; w32_pad_current(&p);
    int32_t lmin = (int32_t)(uint32_t)fields[F_LMIN], lmax = (int32_t)(uint32_t)fields[F_LMAX];
    int64_t range = (int64_t)lmax - lmin;
#define SCALE(v) (int32_t)(lmin + ((int64_t)((v) + 32768) * range) / 65535)
    ax[0] = SCALE(p.lx); ax[1] = SCALE(-p.ly - 1); ax[2] = SCALE((int32_t)p.rt * 128 - (int32_t)p.lt * 128);
    ax[3] = SCALE(p.rx); ax[4] = SCALE(-p.ry - 1); ax[5] = SCALE(0); ax[6] = SCALE(0); ax[7] = 0;
#undef SCALE
    uint32_t pov = 0xFFFFFFFFu; int u = p.buttons & 1, d = p.buttons & 2, l = p.buttons & 4, r = p.buttons & 8;
    if (u && r) pov = 4500; else if (r && d) pov = 13500; else if (d && l) pov = 22500; else if (l && u) pov = 31500;
    else if (u) pov = 0; else if (r) pov = 9000; else if (d) pov = 18000; else if (l) pov = 27000;
    ax[7] = (int32_t)pov;
    static const uint16_t order[10] = { 0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0x0020, 0x0010, 0x0040, 0x0080 };
    memset(buttons, 0, 32);
    for (int i = 0; i < 10; i++) buttons[i] = (p.buttons & order[i]) ? 0x80 : 0;
}

static void q_push(dev_state *d, uint32_t ofs, uint32_t data, uint32_t ts, int bufsize) {
    if (bufsize <= 0) return;
    if (bufsize > QLEN) bufsize = QLEN;
    if (d->n >= bufsize) { d->head = (d->head + 1) % QLEN; d->n--; d->overflow = 1; }   /* the oldest goes */
    int at = (d->head + d->n) % QLEN;
    d->q[at].ofs = ofs; d->q[at].data = data; d->q[at].ts = ts; d->q[at].seq = ++d->seq;
    d->n++;
}

/* Look at the device now: update the last-seen state and queue what changed.
 * Returns how many events were queued, so a notification event can fire. */
static int poll_device(w32 *w, uint64_t self, uint64_t *fields) {
    int slot = (int)fields[F_SLOT] - 1;
    if (slot < 0 || slot >= MAX_DEV) return 0;
    dev_state *d = &g_dev[slot];
    int bufsize = (int)fields[F_BUFSIZE], added = 0;
    uint32_t ts = now_ms();
    (void)w; (void)self;
    switch ((int)fields[F_KIND]) {
    case DEV_KEYBOARD: {
        uint8_t now[256]; read_keyboard(now);
        for (int i = 0; i < 256; i++) if (now[i] != d->keys[i]) { q_push(d, (uint32_t)i, now[i], ts, bufsize); added++; }
        memcpy(d->keys, now, 256);
        break;
    }
    case DEV_MOUSE: {
        int32_t tx, ty, tw; uint32_t b;
        w32_mouse_totals(&tx, &ty, &tw, &b);
        if (tx != d->mx) { q_push(d, 0, (uint32_t)(tx - d->mx), ts, bufsize); added++; }
        if (ty != d->my) { q_push(d, 4, (uint32_t)(ty - d->my), ts, bufsize); added++; }
        if (tw != d->mw) { q_push(d, 8, (uint32_t)(tw - d->mw), ts, bufsize); added++; }
        for (int i = 0; i < 8; i++) {
            uint8_t nb = (b >> i) & 1 ? 0x80 : 0;
            if (nb != d->buttons[i]) { q_push(d, 12 + (uint32_t)i, nb, ts, bufsize); added++; d->buttons[i] = nb; }
        }
        /* the totals are kept, not the deltas: GetDeviceState consumes them */
        d->ax[0] = tx; d->ax[1] = ty; d->ax[2] = tw;
        break;
    }
    case DEV_JOYSTICK: {
        int32_t ax[8]; uint8_t bt[32];
        read_joystick(fields, ax, bt);
        static const uint32_t ofs[8] = { 0, 4, 8, 12, 16, 20, 24, 32 };   /* X Y Z Rx Ry Rz Slider0 POV0 */
        for (int i = 0; i < 8; i++) if (ax[i] != d->ax[i]) { q_push(d, ofs[i], (uint32_t)ax[i], ts, bufsize); added++; }
        for (int i = 0; i < 32; i++) if (bt[i] != d->buttons[i]) { q_push(d, 48 + (uint32_t)i, bt[i], ts, bufsize); added++; }
        memcpy(d->ax, ax, sizeof ax); memcpy(d->buttons, bt, 32);
        break;
    }
    }
    if (added && fields[F_EVENT]) w32_event_set(w, fields[F_EVENT]);
    return added;
}

/* --- helpers for the structures a game hands us -------------------------- */

static void load_fields(w32 *w, uint64_t self, uint64_t *f) { for (int i = 0; i < F_NFIELDS; i++) f[i] = w32_com_get(w, self, i); }
static void put_chars(w32 *w, uint64_t at, uint32_t cap, int wide, const char *s) {
    uint32_t i = 0;
    for (; s[i] && i + 1 < cap; i++) w32_write(w, at + (wide ? 2u : 1u) * i, wide ? 2 : 1, (uint8_t)s[i]);
    w32_write(w, at + (wide ? 2u : 1u) * i, wide ? 2 : 1, 0);
}
static void put_guid(w32 *w, uint64_t at, const uint8_t g[16]) { for (int i = 0; i < 16; i++) w32_write(w, at + (uint64_t)i, 1, g[i]); }
static int guid_eq(w32 *w, uint64_t p, const uint8_t g[16]) { const uint8_t *q = W32PN(w, p, 16); return q && !memcmp(q, g, 16); }

static const char *dev_instance_name(int kind) { return kind == DEV_KEYBOARD ? "Keyboard" : kind == DEV_MOUSE ? "Mouse" : "Winios Gamepad"; }
static const char *dev_product_name(int kind)  { return kind == DEV_KEYBOARD ? "Keyboard" : kind == DEV_MOUSE ? "Mouse" : "Keyboard as gamepad"; }
static const uint8_t *dev_guid(int kind) { return kind == DEV_KEYBOARD ? GUID_SysKeyboard_ : kind == DEV_MOUSE ? GUID_SysMouse_ : GUID_Joystick_; }
/* dwDevType: DirectInput 8 types with their subtype in the second byte, or
 * the pre-8 numbering when the object came from dinput.dll. */
static uint32_t dev_type(int kind, int legacy) {
    if (legacy) return kind == DEV_KEYBOARD ? (3u | 4u << 8) : kind == DEV_MOUSE ? (2u | 2u << 8) : (4u | 4u << 8);
    return kind == DEV_KEYBOARD ? (0x13u | 4u << 8) : kind == DEV_MOUSE ? (0x12u | 2u << 8) : (0x15u | 1u << 8);
}

/* DIDEVICEINSTANCE: dwSize, guidInstance, guidProduct, dwDevType, tszInstanceName[260],
 * tszProductName[260], guidFFDriver, wUsagePage, wUsage. */
static uint32_t instance_size(int wide) { return 4 + 16 + 16 + 4 + (wide ? 520u : 260u) * 2 + 16 + 2 + 2; }
static void fill_instance(w32 *w, uint64_t at, int kind, int wide, int legacy, uint32_t size) {
    w32_write(w, at, 4, size);
    put_guid(w, at + 4, dev_guid(kind));
    put_guid(w, at + 20, dev_guid(kind));
    w32_write(w, at + 36, 4, dev_type(kind, legacy));
    uint32_t nsz = wide ? 520u : 260u;
    put_chars(w, at + 40, MAX_PATH_, wide, dev_instance_name(kind));
    put_chars(w, at + 40 + nsz, MAX_PATH_, wide, dev_product_name(kind));
    if (size >= 40 + 2 * nsz + 20) {                      /* the DX5+ tail */
        for (int i = 0; i < 16; i++) w32_write(w, at + 40 + 2 * nsz + (uint64_t)i, 1, 0);
        w32_write(w, at + 40 + 2 * nsz + 16, 2, 0); w32_write(w, at + 40 + 2 * nsz + 18, 2, 0);
    }
}

/* The objects a device has, for EnumObjects and GetObjectInfo. */
typedef struct { uint32_t ofs, type; const uint8_t *guid; const char *name; } dobj;
static int device_objects(int kind, dobj *out, int max) {
    int n = 0;
#define ADD(o, t, g, nm) do { if (n < max) { out[n].ofs = (o); out[n].type = (t); out[n].guid = (g); out[n].name = (nm); } n++; } while (0)
    if (kind == DEV_MOUSE) {
        ADD(0, DIDFT_RELAXIS_ | 0 << 8, GUID_XAxis_, "X-axis"); ADD(4, DIDFT_RELAXIS_ | 1 << 8, GUID_YAxis_, "Y-axis");
        ADD(8, DIDFT_RELAXIS_ | 2 << 8, GUID_ZAxis_, "Wheel");
        static const char *bn[8] = { "Button 0", "Button 1", "Button 2", "Button 3", "Button 4", "Button 5", "Button 6", "Button 7" };
        for (int i = 0; i < 8; i++) ADD(12 + (uint32_t)i, DIDFT_PSHBUTTON_ | (uint32_t)i << 8, GUID_Button_, bn[i]);
    } else if (kind == DEV_JOYSTICK) {
        ADD(0, DIDFT_ABSAXIS_ | 0 << 8, GUID_XAxis_, "X Axis"); ADD(4, DIDFT_ABSAXIS_ | 1 << 8, GUID_YAxis_, "Y Axis");
        ADD(8, DIDFT_ABSAXIS_ | 2 << 8, GUID_ZAxis_, "Z Axis"); ADD(12, DIDFT_ABSAXIS_ | 3 << 8, GUID_RxAxis_, "X Rotation");
        ADD(16, DIDFT_ABSAXIS_ | 4 << 8, GUID_RyAxis_, "Y Rotation"); ADD(20, DIDFT_ABSAXIS_ | 5 << 8, GUID_RzAxis_, "Z Rotation");
        ADD(32, DIDFT_POV_ | 0 << 8, GUID_POV_, "Hat Switch");
        static const char *bn[10] = { "Button 0", "Button 1", "Button 2", "Button 3", "Button 4", "Button 5", "Button 6", "Button 7", "Button 8", "Button 9" };
        for (int i = 0; i < 10; i++) ADD(48 + (uint32_t)i, DIDFT_PSHBUTTON_ | (uint32_t)i << 8, GUID_Button_, bn[i]);
    } else {
        for (int dik = 1; dik < 256; dik++) {
            int have = 0; for (int vk = 0; vk < 256; vk++) if (g_vk2dik[vk] == dik) { have = 1; break; }
            if (have) ADD((uint32_t)dik, DIDFT_PSHBUTTON_ | (uint32_t)dik << 8, GUID_Key_, 0);
        }
    }
#undef ADD
    return n;
}
/* DIDEVICEOBJECTINSTANCE: dwSize, guidType, dwOfs, dwType, dwFlags, tszName[260], then the
 * DX5 tail: dwFFMaxForce, dwFFForceResolution, wCollectionNumber, wDesignatorIndex,
 * wUsagePage, wUsage, dwDimension, wExponent, wReportId. */
static uint32_t objinst_size(int wide) { return 4 + 16 + 4 + 4 + 4 + (wide ? 520u : 260u) + 4 + 4 + 2 + 2 + 2 + 2 + 4 + 2 + 2; }
static void fill_object(w32 *w, uint64_t at, const dobj *o, int wide, uint32_t size) {
    w32_write(w, at, 4, size);
    put_guid(w, at + 4, o->guid);
    w32_write(w, at + 20, 4, o->ofs);
    w32_write(w, at + 24, 4, o->type);
    w32_write(w, at + 28, 4, (o->type & DIDFT_AXIS_) ? 0x100u : 0);   /* DIDOI_ASPECTPOSITION */
    put_chars(w, at + 32, MAX_PATH_, wide, o->name ? o->name : dik_name((int)o->ofs));
    uint32_t tail = 32 + (wide ? 520u : 260u);
    if (size >= tail + 24) for (uint32_t k = 0; k < 24; k++) w32_write(w, at + tail + k, 1, 0);
}

/* --- IDirectInput8 / IDirectInput7 -------------------------------------------- */

static uint64_t make_device(w32 *w, int kind, int wide, int legacy) {
    int slot = -1;
    for (int i = 0; i < MAX_DEV; i++) if (!g_dev[i].used) { slot = i; break; }
    if (slot < 0) return 0;
    uint64_t o = w32_com_new(w, legacy ? &cls_dev7 : &cls_dev8, F_NFIELDS);
    if (!o) return 0;
    memset(&g_dev[slot], 0, sizeof g_dev[slot]); g_dev[slot].used = 1;
    w32_com_set(w, o, F_WIDE, (uint64_t)wide); w32_com_set(w, o, F_KIND, (uint64_t)kind);
    w32_com_set(w, o, F_SLOT, (uint64_t)slot + 1); w32_com_set(w, o, F_LEGACY, (uint64_t)legacy);
    w32_com_set(w, o, F_AXISMODE, kind == DEV_MOUSE ? 1 : 0);      /* DIPROPAXISMODE_REL for the mouse */
    w32_com_set(w, o, F_LMIN, 0); w32_com_set(w, o, F_LMAX, 65535);
    if (w->verbose) fprintf(stderr, "winrun: dinput: %s created (%s)\n", dev_instance_name(kind), wide ? "W" : "A");
    return o;
}
static int kind_of_guid(w32 *w, uint64_t g) {
    if (guid_eq(w, g, GUID_SysKeyboard_)) return DEV_KEYBOARD;
    if (guid_eq(w, g, GUID_SysMouse_)) return DEV_MOUSE;
    if (guid_eq(w, g, GUID_Joystick_)) return DEV_JOYSTICK;
    return 0;
}
/* CreateDevice(rguid, lplpDevice, pUnkOuter); CreateDeviceEx adds riid before the out pointer. */
static void create_device(w32 *w, uint64_t rguid, uint64_t out) {
    int wide = (int)w32_com_get(w, ARG(0), F_WIDE), legacy = w32_com_tag(w, ARG(0)) == TAG_DI && (int)w32_com_get(w, ARG(0), F_LEGACY);
    if (!out) { RET((uint64_t)(uint32_t)E_POINTER_); return; }
    int kind = kind_of_guid(w, rguid);
    if (!kind) { w32_write(w, out, (int)w32_ptrsize(w), 0); RET((uint64_t)(uint32_t)DIERR_DEVICENOTREG_); return; }
    uint64_t o = make_device(w, kind, wide, legacy);
    if (!o) { RET((uint64_t)(uint32_t)0x80004005u); return; }        /* E_FAIL */
    w32_write(w, out, (int)w32_ptrsize(w), o);
    RET(S_OK_);
}
static void di_CreateDevice(w32 *w) { create_device(w, ARG(1), ARG(2)); }
static void di_CreateDeviceEx(w32 *w) { create_device(w, ARG(1), ARG(3)); }

/* EnumDevices(dwDevType, callback, pvRef, dwFlags): each device the class asks
 * for, through the game's callback, which may stop the walk. */
static void di_EnumDevices(w32 *w) {
    uint32_t cls = (uint32_t)ARG(1) & 0xFF; uint64_t cb = ARG(2), ref = ARG(3);
    int wide = (int)w32_com_get(w, ARG(0), F_WIDE), legacy = (int)w32_com_get(w, ARG(0), F_LEGACY);
    if (!cb) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    uint32_t size = instance_size(wide);
    uint64_t buf = w32_heap_alloc(w, size);
    if (!buf) { RET((uint64_t)(uint32_t)0x80004005u); return; }
    static const int kinds[3] = { DEV_KEYBOARD, DEV_MOUSE, DEV_JOYSTICK };
    for (int i = 0; i < 3; i++) {
        int kind = kinds[i], want;
        if (legacy) want = cls == 0 || cls == (kind == DEV_KEYBOARD ? 3u : kind == DEV_MOUSE ? 2u : 4u);
        else        want = cls == 0 || cls == (kind == DEV_KEYBOARD ? 3u : kind == DEV_MOUSE ? 2u : 4u);   /* DI8DEVCLASS_KEYBOARD/POINTER/GAMECTRL happen to match */
        if (!want) continue;
        fill_instance(w, buf, kind, wide, legacy, size);
        uint64_t args[2] = { buf, ref };
        uint64_t r = w32_call_guest(w, cb, 2, args);
        if (w->exited || (uint32_t)r == DIENUM_STOP_) break;
    }
    w32_heap_free(w, buf);
    RET(S_OK_);
}
static void di_GetDeviceStatus(w32 *w) { RET(kind_of_guid(w, ARG(1)) ? (uint64_t)S_OK_ : (uint64_t)(uint32_t)DIERR_DEVICENOTREG_); }
static void di_RunControlPanel(w32 *w) { (void)w; RET(S_OK_); }
static void di_Initialize(w32 *w) { (void)w; RET(S_OK_); }
static void di_FindDevice(w32 *w) { (void)w; RET((uint64_t)(uint32_t)DIERR_DEVICENOTREG_); }
static void di_EnumDevicesBySemantics(w32 *w) { (void)w; RET(S_OK_); }       /* no action-mapped devices offered */
static void di_ConfigureDevices(w32 *w) { (void)w; RET((uint64_t)(uint32_t)E_NOTIMPL_); }

/* --- IDirectInputDevice8 / 7 ---------------------------------------------------- */

static void dv_GetCapabilities(w32 *w) {
    uint64_t f[F_NFIELDS]; load_fields(w, ARG(0), f);
    uint64_t c = ARG(1);
    if (!c || !w32_mem_ok(w, c, 4)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    uint32_t size = (uint32_t)w32_read(w, c, 4);
    if (size < 24 || !w32_mem_ok(w, c, size)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    int kind = (int)f[F_KIND];
    w32_write(w, c + 4, 4, 1u | 4u);                                  /* DIDC_ATTACHED | DIDC_EMULATED */
    w32_write(w, c + 8, 4, dev_type(kind, (int)f[F_LEGACY]));
    w32_write(w, c + 12, 4, kind == DEV_KEYBOARD ? 0 : kind == DEV_MOUSE ? 3 : 6);
    w32_write(w, c + 16, 4, kind == DEV_KEYBOARD ? 128 : kind == DEV_MOUSE ? 8 : 10);
    w32_write(w, c + 20, 4, kind == DEV_JOYSTICK ? 1 : 0);
    for (uint32_t k = 24; k + 4 <= size && k < 44; k += 4) w32_write(w, c + k, 4, 0);
    RET(S_OK_);
}
/* EnumObjects(callback, pvRef, dwFlags): DIDFT_ALL, or the classes named. */
static void dv_EnumObjects(w32 *w) {
    uint64_t f[F_NFIELDS]; load_fields(w, ARG(0), f);
    uint64_t cb = ARG(1), ref = ARG(2); uint32_t flags = (uint32_t)ARG(3);
    if (!cb) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    dobj objs[256]; int n = device_objects((int)f[F_KIND], objs, 256);
    uint32_t size = objinst_size((int)f[F_WIDE]);
    uint64_t buf = w32_heap_alloc(w, size);
    if (!buf) { RET((uint64_t)(uint32_t)0x80004005u); return; }
    for (int i = 0; i < n && i < 256; i++) {
        if (flags != DIDFT_ALL_ && !(objs[i].type & flags & 0xFF)) continue;
        fill_object(w, buf, &objs[i], (int)f[F_WIDE], size);
        uint64_t args[2] = { buf, ref };
        uint64_t r = w32_call_guest(w, cb, 2, args);
        if (w->exited || (uint32_t)r == DIENUM_STOP_) break;
    }
    w32_heap_free(w, buf);
    RET(S_OK_);
}
/* The property id is the "GUID pointer" MAKEDIPROP(n): a small integer. */
static void dv_GetProperty(w32 *w) {
    uint64_t f[F_NFIELDS]; load_fields(w, ARG(0), f);
    uint64_t prop = ARG(1), h = ARG(2);
    if (!h || !w32_mem_ok(w, h, 16)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    uint32_t size = (uint32_t)w32_read(w, h, 4);
    if (!w32_mem_ok(w, h, size)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    switch ((int)prop) {
    case DIPROP_BUFFERSIZE_: w32_write(w, h + 16, 4, (uint32_t)f[F_BUFSIZE]); RET(S_OK_); return;
    case DIPROP_AXISMODE_:   w32_write(w, h + 16, 4, (uint32_t)f[F_AXISMODE]); RET(S_OK_); return;
    case DIPROP_GRANULARITY_: w32_write(w, h + 16, 4, 1); RET(S_OK_); return;
    case DIPROP_RANGE_:      w32_write(w, h + 16, 4, (uint32_t)f[F_LMIN]); w32_write(w, h + 20, 4, (uint32_t)f[F_LMAX]); RET(S_OK_); return;
    case DIPROP_DEADZONE_:   w32_write(w, h + 16, 4, 0); RET(S_OK_); return;
    case DIPROP_SATURATION_: w32_write(w, h + 16, 4, 10000); RET(S_OK_); return;
    case DIPROP_FFGAIN_:     w32_write(w, h + 16, 4, 10000); RET(S_OK_); return;
    case DIPROP_VIDPID_:     w32_write(w, h + 16, 4, 0); RET(S_OK_); return;
    case DIPROP_INSTANCENAME_: case DIPROP_PRODUCTNAME_:
        if (size >= 16 + 2 * MAX_PATH_) put_chars(w, h + 16, MAX_PATH_, 1, (int)prop == DIPROP_INSTANCENAME_ ? dev_instance_name((int)f[F_KIND]) : dev_product_name((int)f[F_KIND]));
        RET(S_OK_); return;
    }
    RET((uint64_t)(uint32_t)E_NOTIMPL_);                       /* DIERR_UNSUPPORTED */
}
static void dv_SetProperty(w32 *w) {
    uint64_t f[F_NFIELDS]; load_fields(w, ARG(0), f);
    uint64_t prop = ARG(1), h = ARG(2);
    if (!h || !w32_mem_ok(w, h, 20)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    switch ((int)prop) {
    case DIPROP_BUFFERSIZE_: { uint32_t n = (uint32_t)w32_read(w, h + 16, 4); if (n > QLEN) n = QLEN; w32_com_set(w, ARG(0), F_BUFSIZE, n); RET(S_OK_); return; }
    case DIPROP_AXISMODE_:
        if (f[F_ACQUIRED]) { RET((uint64_t)(uint32_t)DIERR_ACQUIRED_); return; }
        w32_com_set(w, ARG(0), F_AXISMODE, (uint32_t)w32_read(w, h + 16, 4) ? 1 : 0); RET(S_OK_); return;
    case DIPROP_RANGE_:
        if (!w32_mem_ok(w, h, 24)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
        w32_com_set(w, ARG(0), F_LMIN, (uint32_t)w32_read(w, h + 16, 4)); w32_com_set(w, ARG(0), F_LMAX, (uint32_t)w32_read(w, h + 20, 4));
        RET(S_OK_); return;
    case DIPROP_DEADZONE_: case DIPROP_SATURATION_: case DIPROP_FFGAIN_: RET(S_OK_); return;
    }
    RET(S_FALSE_);                                              /* DI_PROPNOEFFECT: accepted, changes nothing */
}
static void dv_Acquire(w32 *w) {
    uint64_t f[F_NFIELDS]; load_fields(w, ARG(0), f);
    if (f[F_ACQUIRED]) { RET(S_FALSE_); return; }
    w32_com_set(w, ARG(0), F_ACQUIRED, 1);
    /* the state at acquisition is the baseline: what was already held down is
     * state, not an event */
    int slot = (int)f[F_SLOT] - 1;
    if (slot >= 0) {
        dev_state *d = &g_dev[slot];
        d->n = 0; d->head = 0; d->overflow = 0;
        uint64_t save = f[F_BUFSIZE]; f[F_BUFSIZE] = 0;          /* a poll that records nothing */
        poll_device(w, ARG(0), f);
        f[F_BUFSIZE] = save;
    }
    RET(S_OK_);
}
static void dv_Unacquire(w32 *w) {
    int was = (int)w32_com_get(w, ARG(0), F_ACQUIRED);
    w32_com_set(w, ARG(0), F_ACQUIRED, 0);
    RET(was ? S_OK_ : S_FALSE_);
}
/* GetDeviceState(cbData, lpvData): the shape depends on the device -- and on
 * what SetDataFormat asked for, which for every game that has ever shipped
 * is one of the standard formats, told apart here by their sizes. */
static void dv_GetDeviceState(w32 *w) {
    uint64_t f[F_NFIELDS]; load_fields(w, ARG(0), f);
    uint32_t cb = (uint32_t)ARG(1); uint64_t p = ARG(2);
    if (!f[F_ACQUIRED]) { RET((uint64_t)(uint32_t)DIERR_NOTACQUIRED_); return; }
    if (!p || !cb || !w32_mem_ok(w, p, cb)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    poll_device(w, ARG(0), f);
    int slot = (int)f[F_SLOT] - 1; dev_state *d = &g_dev[slot];
    for (uint32_t k = 0; k < cb; k++) w32_write(w, p + k, 1, 0);
    switch ((int)f[F_KIND]) {
    case DEV_KEYBOARD:
        for (uint32_t k = 0; k < cb && k < 256; k++) w32_write(w, p + k, 1, d->keys[k]);
        break;
    case DEV_MOUSE: {
        /* relative: motion since the last read; absolute: the running total */
        int32_t v[3] = { d->ax[0], d->ax[1], d->ax[2] };
        if (f[F_AXISMODE] == 1) { for (int i = 0; i < 3; i++) { int32_t t = v[i]; v[i] -= d->last_rel[i]; d->last_rel[i] = t; } }
        if (cb >= 12) for (int i = 0; i < 3; i++) w32_write(w, p + 4u * (uint32_t)i, 4, (uint32_t)v[i]);
        for (uint32_t i = 0; i < 8 && 12 + i < cb; i++) w32_write(w, p + 12 + i, 1, d->buttons[i]);
        break;
    }
    case DEV_JOYSTICK: {
        static const uint32_t ofs[8] = { 0, 4, 8, 12, 16, 20, 24, 32 };
        for (int i = 0; i < 8; i++) if (ofs[i] + 4 <= cb) w32_write(w, p + ofs[i], 4, (uint32_t)d->ax[i]);
        if (cb >= 32) { w32_write(w, p + 28, 4, (uint32_t)d->ax[6]); }                     /* the second slider, centred */
        for (uint32_t i = 1; i < 4 && 32 + 4 * i + 4 <= cb; i++) w32_write(w, p + 32 + 4 * i, 4, 0xFFFFFFFFu);   /* POVs 1-3: centred */
        for (uint32_t i = 0; i < 32 && 48 + i < cb; i++) w32_write(w, p + 48 + i, 1, d->buttons[i]);
        break;
    }
    }
    RET(S_OK_);
}
/* GetDeviceData(cbObjectData, rgdod, pdwInOut, dwFlags). DIDEVICEOBJECTDATA is
 * 16 bytes as DX3 defined it and with uAppData 16 (32-bit) or 24 (64-bit);
 * cbObjectData says which. */
static void dv_GetDeviceData(w32 *w) {
    uint64_t f[F_NFIELDS]; load_fields(w, ARG(0), f);
    uint32_t cb = (uint32_t)ARG(1); uint64_t arr = ARG(2), inout = ARG(3); uint32_t flags = (uint32_t)ARG(4);
    if (!f[F_ACQUIRED]) { RET((uint64_t)(uint32_t)DIERR_NOTACQUIRED_); return; }
    if (!f[F_BUFSIZE]) { RET((uint64_t)(uint32_t)DIERR_NOTBUFFERED_); return; }
    if (!inout || !w32_mem_ok(w, inout, 4) || (cb != 16 && cb != 20 && cb != 24)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    poll_device(w, ARG(0), f);
    int slot = (int)f[F_SLOT] - 1; dev_state *d = &g_dev[slot];
    uint32_t want = (uint32_t)w32_read(w, inout, 4), got = 0;
    if (arr && want && !w32_mem_ok(w, arr, (uint64_t)cb * want)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    int peek = (flags & 1) != 0;
    for (uint32_t i = 0; i < want && (int)i < d->n; i++) {
        int at = (d->head + (int)i) % QLEN;
        if (arr) {
            uint64_t e = arr + (uint64_t)cb * i;
            w32_write(w, e, 4, d->q[at].ofs); w32_write(w, e + 4, 4, d->q[at].data);
            w32_write(w, e + 8, 4, d->q[at].ts); w32_write(w, e + 12, 4, d->q[at].seq);
            if (cb >= 20) w32_write(w, e + 16, cb - 16, 0);
        }
        got++;
    }
    if (!arr && want == 0xFFFFFFFFu) got = (uint32_t)d->n;              /* "how many are there?" */
    if (!peek) { d->head = (d->head + (int)got) % QLEN; d->n -= (int)got; }
    w32_write(w, inout, 4, got);
    int overflow = d->overflow; if (!peek) d->overflow = 0;
    RET(overflow ? S_FALSE_ : S_OK_);                                  /* DI_BUFFEROVERFLOW is S_FALSE */
}
/* DIDATAFORMAT: dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs, rgodf. */
static void dv_SetDataFormat(w32 *w) {
    uint64_t f[F_NFIELDS]; load_fields(w, ARG(0), f);
    uint64_t p = ARG(1);
    if (f[F_ACQUIRED]) { RET((uint64_t)(uint32_t)DIERR_ACQUIRED_); return; }
    if (!p || !w32_mem_ok(w, p, 20)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    uint32_t flags = (uint32_t)w32_read(w, p + 8, 4), size = (uint32_t)w32_read(w, p + 12, 4);
    w32_com_set(w, ARG(0), F_FMTSIZE, size);
    if (flags & 1) w32_com_set(w, ARG(0), F_AXISMODE, 0);             /* DIDF_ABSAXIS */
    if (flags & 2) w32_com_set(w, ARG(0), F_AXISMODE, 1);             /* DIDF_RELAXIS */
    if (w->verbose) fprintf(stderr, "winrun: dinput: %s data format, %u bytes\n", dev_instance_name((int)f[F_KIND]), size);
    RET(S_OK_);
}
static void dv_SetEventNotification(w32 *w) { w32_com_set(w, ARG(0), F_EVENT, ARG(1)); RET(S_OK_); }
static void dv_SetCooperativeLevel(w32 *w) { w32_com_set(w, ARG(0), F_COOP, ARG(2)); RET(S_OK_); }
/* GetObjectInfo(pdidoi, dwObj, dwHow) */
static void dv_GetObjectInfo(w32 *w) {
    uint64_t f[F_NFIELDS]; load_fields(w, ARG(0), f);
    uint64_t p = ARG(1); uint32_t obj = (uint32_t)ARG(2), how = (uint32_t)ARG(3);
    if (!p || !w32_mem_ok(w, p, 4)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    uint32_t size = (uint32_t)w32_read(w, p, 4);
    if (!w32_mem_ok(w, p, size)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    dobj objs[256]; int n = device_objects((int)f[F_KIND], objs, 256);
    for (int i = 0; i < n && i < 256; i++) {
        int hit = how == DIPH_BYOFFSET_ ? objs[i].ofs == obj : how == DIPH_BYID_ ? objs[i].type == obj : 0;
        if (hit) { fill_object(w, p, &objs[i], (int)f[F_WIDE], size); RET(S_OK_); return; }
    }
    RET((uint64_t)(uint32_t)DIERR_OBJECTNOTFOUND_);
}
static void dv_GetDeviceInfo(w32 *w) {
    uint64_t f[F_NFIELDS]; load_fields(w, ARG(0), f);
    uint64_t p = ARG(1);
    if (!p || !w32_mem_ok(w, p, 4)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    uint32_t size = (uint32_t)w32_read(w, p, 4);
    if (size < 40 || !w32_mem_ok(w, p, size)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    fill_instance(w, p, (int)f[F_KIND], (int)f[F_WIDE], (int)f[F_LEGACY], size);
    RET(S_OK_);
}
static void dv_Poll(w32 *w) {
    uint64_t f[F_NFIELDS]; load_fields(w, ARG(0), f);
    if (!f[F_ACQUIRED]) { RET((uint64_t)(uint32_t)DIERR_NOTACQUIRED_); return; }
    poll_device(w, ARG(0), f);
    RET(f[F_KIND] == DEV_JOYSTICK ? S_OK_ : S_FALSE_);                 /* DI_NOEFFECT: nothing to poll */
}
static void dv_ok(w32 *w) { (void)w; RET(S_OK_); }
static void dv_unsupported(w32 *w) { (void)w; RET((uint64_t)(uint32_t)E_NOTIMPL_); }
static void dv_noeffect(w32 *w) { (void)w; RET(S_FALSE_); }
static void dv_ffstate(w32 *w) { if (ARG(1)) w32_write(w, ARG(1), 4, 0x80); RET(S_OK_); }   /* DIGFFS_ACTUATORSOFF */

/* --- the tables ------------------------------------------------------------------- */

/* Parameters named so they cannot collide with the fields they assign to. */
#define M(tab, slot, nm, na, f) do { (tab)[slot].name = (nm); (tab)[slot].nargs = (na); (tab)[slot].fn = (f); } while (0)
static void build_tables(void) {
    if (g_built) return;
    g_built = 1;
    build_vk2dik();
    for (int i = 0; i < DI8_NSLOTS; i++)  di8_methods[i]  = (w32_api){ 0, 0, 0, 0, 0 };
    for (int i = 0; i < DI7_NSLOTS; i++)  di7_methods[i]  = (w32_api){ 0, 0, 0, 0, 0 };
    for (int i = 0; i < DID8_NSLOTS; i++) dev8_methods[i] = (w32_api){ 0, 0, 0, 0, 0 };
    for (int i = 0; i < DID7_NSLOTS; i++) dev7_methods[i] = (w32_api){ 0, 0, 0, 0, 0 };

    M(di8_methods, DI8_QueryInterface, "QueryInterface", 3, w32_com_QueryInterface);
    M(di8_methods, DI8_AddRef, "AddRef", 1, w32_com_AddRef);
    M(di8_methods, DI8_Release, "Release", 1, w32_com_Release);
    M(di8_methods, DI8_CreateDevice, "CreateDevice", 4, di_CreateDevice);
    M(di8_methods, DI8_EnumDevices, "EnumDevices", 5, di_EnumDevices);
    M(di8_methods, DI8_GetDeviceStatus, "GetDeviceStatus", 2, di_GetDeviceStatus);
    M(di8_methods, DI8_RunControlPanel, "RunControlPanel", 3, di_RunControlPanel);
    M(di8_methods, DI8_Initialize, "Initialize", 3, di_Initialize);
    M(di8_methods, DI8_FindDevice, "FindDevice", 4, di_FindDevice);
    M(di8_methods, DI8_EnumDevicesBySemantics, "EnumDevicesBySemantics", 6, di_EnumDevicesBySemantics);
    M(di8_methods, DI8_ConfigureDevices, "ConfigureDevices", 5, di_ConfigureDevices);

    M(di7_methods, DI7_QueryInterface, "QueryInterface", 3, w32_com_QueryInterface);
    M(di7_methods, DI7_AddRef, "AddRef", 1, w32_com_AddRef);
    M(di7_methods, DI7_Release, "Release", 1, w32_com_Release);
    M(di7_methods, DI7_CreateDevice, "CreateDevice", 4, di_CreateDevice);
    M(di7_methods, DI7_EnumDevices, "EnumDevices", 5, di_EnumDevices);
    M(di7_methods, DI7_GetDeviceStatus, "GetDeviceStatus", 2, di_GetDeviceStatus);
    M(di7_methods, DI7_RunControlPanel, "RunControlPanel", 3, di_RunControlPanel);
    M(di7_methods, DI7_Initialize, "Initialize", 3, di_Initialize);
    M(di7_methods, DI7_FindDevice, "FindDevice", 4, di_FindDevice);
    M(di7_methods, DI7_CreateDeviceEx, "CreateDeviceEx", 5, di_CreateDeviceEx);

    w32_api *tabs[2] = { dev8_methods, dev7_methods };
    for (int t = 0; t < 2; t++) {
        w32_api *d = tabs[t];
        M(d, DID8_QueryInterface, "QueryInterface", 3, w32_com_QueryInterface);
        M(d, DID8_AddRef, "AddRef", 1, w32_com_AddRef);
        M(d, DID8_Release, "Release", 1, w32_com_Release);
        M(d, DID8_GetCapabilities, "GetCapabilities", 2, dv_GetCapabilities);
        M(d, DID8_EnumObjects, "EnumObjects", 4, dv_EnumObjects);
        M(d, DID8_GetProperty, "GetProperty", 3, dv_GetProperty);
        M(d, DID8_SetProperty, "SetProperty", 3, dv_SetProperty);
        M(d, DID8_Acquire, "Acquire", 1, dv_Acquire);
        M(d, DID8_Unacquire, "Unacquire", 1, dv_Unacquire);
        M(d, DID8_GetDeviceState, "GetDeviceState", 3, dv_GetDeviceState);
        M(d, DID8_GetDeviceData, "GetDeviceData", 5, dv_GetDeviceData);
        M(d, DID8_SetDataFormat, "SetDataFormat", 2, dv_SetDataFormat);
        M(d, DID8_SetEventNotification, "SetEventNotification", 2, dv_SetEventNotification);
        M(d, DID8_SetCooperativeLevel, "SetCooperativeLevel", 3, dv_SetCooperativeLevel);
        M(d, DID8_GetObjectInfo, "GetObjectInfo", 4, dv_GetObjectInfo);
        M(d, DID8_GetDeviceInfo, "GetDeviceInfo", 2, dv_GetDeviceInfo);
        M(d, DID8_RunControlPanel, "RunControlPanel", 3, dv_ok);
        M(d, DID8_Initialize, "Initialize", 4, dv_ok);
        M(d, DID8_CreateEffect, "CreateEffect", 5, dv_unsupported);
        M(d, DID8_EnumEffects, "EnumEffects", 4, dv_ok);
        M(d, DID8_GetEffectInfo, "GetEffectInfo", 3, dv_unsupported);
        M(d, DID8_GetForceFeedbackState, "GetForceFeedbackState", 2, dv_ffstate);
        M(d, DID8_SendForceFeedbackCommand, "SendForceFeedbackCommand", 2, dv_unsupported);
        M(d, DID8_EnumCreatedEffectObjects, "EnumCreatedEffectObjects", 4, dv_ok);
        M(d, DID8_Escape, "Escape", 2, dv_unsupported);
        M(d, DID8_Poll, "Poll", 1, dv_Poll);
        M(d, DID8_SendDeviceData, "SendDeviceData", 5, dv_noeffect);
        M(d, DID8_EnumEffectsInFile, "EnumEffectsInFile", 5, dv_unsupported);
        M(d, DID8_WriteEffectToFile, "WriteEffectToFile", 5, dv_unsupported);
        if (t == 0) {
            M(d, DID8_BuildActionMap, "BuildActionMap", 4, dv_noeffect);
            M(d, DID8_SetActionMap, "SetActionMap", 4, dv_noeffect);
            M(d, DID8_GetImageInfo, "GetImageInfo", 2, dv_unsupported);
        }
    }
    /* The 7 table is a prefix of the 8 table: the slots must agree up to where 7 ends. */
    _Static_assert(DID7_WriteEffectToFile == DID8_WriteEffectToFile, "IDirectInputDevice7 must be a prefix of IDirectInputDevice8");
    _Static_assert(DID7_NSLOTS == DID8_WriteEffectToFile + 1, "IDirectInputDevice7 ends at WriteEffectToFile");
    _Static_assert(DI7_FindDevice == DI8_FindDevice, "IDirectInput7 and 8 agree through FindDevice");

    cls_di8  = (w32_com_class){ "IDirectInput8",        di8_methods,  DI8_NSLOTS,  TAG_DI,    0, {0} };
    cls_di7  = (w32_com_class){ "IDirectInput7",        di7_methods,  DI7_NSLOTS,  TAG_DI,    0, {0} };
    cls_dev8 = (w32_com_class){ "IDirectInputDevice8",  dev8_methods, DID8_NSLOTS, TAG_DIDEV, 0, {0} };
    cls_dev7 = (w32_com_class){ "IDirectInputDevice7",  dev7_methods, DID7_NSLOTS, TAG_DIDEV, 0, {0} };
}

static uint64_t make_di(w32 *w, int wide, int legacy) {
    build_tables();
    uint64_t o = w32_com_new(w, legacy ? &cls_di7 : &cls_di8, F_NFIELDS);
    if (o) { w32_com_set(w, o, F_WIDE, (uint64_t)wide); w32_com_set(w, o, F_LEGACY, (uint64_t)legacy); }
    return o;
}
/* DirectInput8Create(hinst, dwVersion, riid, ppvOut, punkOuter) */
static void d_DirectInput8Create(w32 *w) {
    uint64_t out = ARG(3);
    if (!out) { RET((uint64_t)(uint32_t)E_POINTER_); return; }
    int wide = guid_eq(w, ARG(2), IID_IDirectInput8W_);
    uint64_t o = make_di(w, wide, 0);
    if (!o) { RET((uint64_t)(uint32_t)0x80004005u); return; }
    w32_write(w, out, (int)w32_ptrsize(w), o);
    if (w->verbose) fprintf(stderr, "winrun: dinput8: created (%s), version %#x\n", wide ? "W" : "A", (unsigned)ARG(1));
    RET(S_OK_);
}
/* DirectInputCreateA/W(hinst, dwVersion, lplpDirectInput, punkOuter) */
static void create_legacy(w32 *w, int wide, uint64_t out) {
    if (!out) { RET((uint64_t)(uint32_t)E_POINTER_); return; }
    uint64_t o = make_di(w, wide, 1);
    if (!o) { RET((uint64_t)(uint32_t)0x80004005u); return; }
    w32_write(w, out, (int)w32_ptrsize(w), o);
    if (w->verbose) fprintf(stderr, "winrun: dinput: created (%s), version %#x\n", wide ? "W" : "A", (unsigned)ARG(1));
    RET(S_OK_);
}
static void d_DirectInputCreateA(w32 *w) { create_legacy(w, 0, ARG(2)); }
static void d_DirectInputCreateW(w32 *w) { create_legacy(w, 1, ARG(2)); }
/* DirectInputCreateEx(hinst, dwVersion, riid, ppvOut, punkOuter) */
static void d_DirectInputCreateEx(w32 *w) {
    create_legacy(w, guid_eq(w, ARG(2), IID_IDirectInput7W_) || guid_eq(w, ARG(2), IID_IDirectInputW_), ARG(3));
}
static void d_DllGetClassObject(w32 *w) { (void)w; RET(0x80040111u); }   /* CLASS_E_CLASSNOTAVAILABLE */
static void d_DllCanUnloadNow(w32 *w) { (void)w; RET(1); }

/* CoCreateInstance(CLSID_DirectInput8) / (CLSID_DirectInput): the same objects. */
int w32_dinput_create_class(w32 *w, const uint8_t clsid[16], const uint8_t iid[16], uint64_t out) {
    int legacy;
    if (!memcmp(clsid, CLSID_DirectInput8_, 16)) legacy = 0;
    else if (!memcmp(clsid, CLSID_DirectInput_, 16)) legacy = 1;
    else return 0;
    int wide = iid && (!memcmp(iid, IID_IDirectInput8W_, 16) || !memcmp(iid, IID_IDirectInput7W_, 16) || !memcmp(iid, IID_IDirectInputW_, 16));
    uint64_t o = make_di(w, wide, legacy);
    if (!o) return 0;
    w32_write(w, out, (int)w32_ptrsize(w), o);
    return 1;
}

int w32_dinput_device_count(void) { int n = 0; for (int i = 0; i < MAX_DEV; i++) n += g_dev[i].used; return n; }
void w32_dinput_reset(void) { g_built = 0; memset(g_dev, 0, sizeof g_dev); }

#define F(n, a)  { #n, a, 0, d_##n, 0 }
const w32_api w32_dinput8[] = {
    F(DirectInput8Create, 5), F(DllGetClassObject, 3), F(DllCanUnloadNow, 0),
    { 0, 0, 0, 0, 0 },
};
const w32_api w32_dinput[] = {
    F(DirectInputCreateA, 4), F(DirectInputCreateW, 4), F(DirectInputCreateEx, 5),
    F(DllGetClassObject, 3), F(DllCanUnloadNow, 0),
    { 0, 0, 0, 0, 0 },
};
