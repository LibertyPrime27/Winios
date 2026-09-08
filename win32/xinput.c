/* xinput1_3.dll / xinput1_4.dll -- a gamepad.
 *
 * Eight functions, and it is the cheapest breadth on the list: every game
 * from about 2006 onwards reads a controller through XInput, and unlike
 * DirectInput there is no COM, no device enumeration and no data formats --
 * a struct of buttons and axes, polled.
 *
 * It reports a connected pad and feeds it from the same state user32.c keeps,
 * so the on-screen keys and a real keyboard drive it as well as a real
 * controller does. That is not a stopgap: on a tablet the keyboard *is* the
 * controller for a lot of people, and a game that only reads XInput would
 * otherwise be unplayable however good the touch controls are.
 *
 * The mapping is the conventional one a keyboard-to-pad layer uses -- WASD to
 * the left stick, arrows to the right, and the letters a game's own defaults
 * would have used for the face buttons. w32_pad_state() lets the host replace
 * all of it with a real controller's values (GCController on iOS), in which
 * case the keyboard contribution is simply absent.
 */
#include "w32.h"

#include <string.h>

enum { ERROR_SUCCESS_ = 0, ERROR_BAD_ARGUMENTS_ = 160,
       ERROR_DEVICE_NOT_CONNECTED_ = 1167, ERROR_EMPTY_ = 4306 };

/* XINPUT_GAMEPAD buttons */
enum {
    PAD_DPAD_UP = 0x0001, PAD_DPAD_DOWN = 0x0002, PAD_DPAD_LEFT = 0x0004, PAD_DPAD_RIGHT = 0x0008,
    PAD_START = 0x0010, PAD_BACK = 0x0020, PAD_LTHUMB = 0x0040, PAD_RTHUMB = 0x0080,
    PAD_LSHOULDER = 0x0100, PAD_RSHOULDER = 0x0200,
    PAD_A = 0x1000, PAD_B = 0x2000, PAD_X = 0x4000, PAD_Y = 0x8000,
};

/* What the host has to say, when it has a real controller. Set from the app;
 * `present` distinguishes "no pad attached" from "a pad reading all zeros",
 * which a game uses to decide whether to show controller prompts. */
static w32_pad g_pad;
static int g_pad_set;
static uint32_t g_packet;

void w32_pad_state(const w32_pad *p) {
    if (!p) { g_pad_set = 0; return; }
    g_pad = *p;
    g_pad_set = 1;
    g_packet++;
}

/* Virtual keys, so the keyboard can stand in for a pad. */
enum { VK_LEFT_ = 0x25, VK_UP_ = 0x26, VK_RIGHT_ = 0x27, VK_DOWN_ = 0x28,
       VK_RETURN_ = 0x0D, VK_ESCAPE_ = 0x1B, VK_SPACE_ = 0x20, VK_SHIFT_ = 0x10 };

static int down(int vk) { return w32_key_down(vk); }

/* The keyboard as a pad. Deliberately the conventional layout rather than an
 * invented one: someone who has used a keyboard-to-pad tool before should not
 * have to learn this, and a game's own default bindings tend to agree. */
static void pad_from_keyboard(w32_pad *p) {
    memset(p, 0, sizeof *p);
    int lx = (down('D') ? 1 : 0) - (down('A') ? 1 : 0);
    int ly = (down('W') ? 1 : 0) - (down('S') ? 1 : 0);
    int rx = (down(VK_RIGHT_) ? 1 : 0) - (down(VK_LEFT_) ? 1 : 0);
    int ry = (down(VK_UP_) ? 1 : 0) - (down(VK_DOWN_) ? 1 : 0);
    p->lx = (int16_t)(lx * 32767);
    p->ly = (int16_t)(ly * 32767);
    p->rx = (int16_t)(rx * 32767);
    p->ry = (int16_t)(ry * 32767);
    if (down(VK_SPACE_))  p->buttons |= PAD_A;
    if (down('E'))        p->buttons |= PAD_B;
    if (down('R'))        p->buttons |= PAD_X;
    if (down('F'))        p->buttons |= PAD_Y;
    if (down(VK_RETURN_)) p->buttons |= PAD_START;
    if (down(VK_ESCAPE_)) p->buttons |= PAD_BACK;
    if (down('Q'))        p->buttons |= PAD_LSHOULDER;
    if (down(VK_SHIFT_))  p->buttons |= PAD_RSHOULDER;
    /* the triggers are analogue and a key is not, so a key is all the way */
    if (down('Z')) p->lt = 255;
    if (down('C')) p->rt = 255;
    p->present = 1;
}

/* XINPUT_STATE: dwPacketNumber, then XINPUT_GAMEPAD
 * { WORD wButtons; BYTE bLeftTrigger, bRightTrigger;
 *   SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; }
 * -- 4 + 12, no padding to worry about in either bitness. */
static void x_XInputGetState(w32 *w) {
    uint32_t user = (uint32_t)ARG(0);
    uint64_t st = ARG(1);
    if (user != 0) { RET(ERROR_DEVICE_NOT_CONNECTED_); return; }   /* one pad, index 0 */

    w32_pad p;
    if (g_pad_set) p = g_pad;
    else pad_from_keyboard(&p);
    if (!p.present) { RET(ERROR_DEVICE_NOT_CONNECTED_); return; }

    if (!st) { RET(ERROR_SUCCESS_); return; }
    /* The packet number must change only when the state does, because a game
     * that polls at its frame rate uses it to skip work -- one that always
     * changes makes every frame look like new input, and one that never
     * changes makes a game ignore real input entirely. */
    static uint32_t last_packet;
    static w32_pad last;
    if (memcmp(&last, &p, sizeof p)) { last = p; last_packet++; }
    w32_write(w, st + 0, 4, last_packet);
    w32_write(w, st + 4, 2, p.buttons);
    w32_write(w, st + 6, 1, p.lt);
    w32_write(w, st + 7, 1, p.rt);
    w32_write(w, st + 8, 2, (uint16_t)p.lx);
    w32_write(w, st + 10, 2, (uint16_t)p.ly);
    w32_write(w, st + 12, 2, (uint16_t)p.rx);
    w32_write(w, st + 14, 2, (uint16_t)p.ry);
    RET(ERROR_SUCCESS_);
}
/* Rumble. Accepted and dropped -- the host can do it later through
 * CoreHaptics, and a game that sets it does not read it back. */
static void x_XInputSetState(w32 *w) {
    RET((uint32_t)ARG(0) == 0 ? ERROR_SUCCESS_ : ERROR_DEVICE_NOT_CONNECTED_);
}
/* XINPUT_CAPABILITIES: Type, SubType, Flags, then XINPUT_GAMEPAD and
 * XINPUT_VIBRATION { WORD left, right; }. */
static void x_XInputGetCapabilities(w32 *w) {
    uint32_t user = (uint32_t)ARG(0);
    uint64_t c = ARG(2);
    if (user != 0) { RET(ERROR_DEVICE_NOT_CONNECTED_); return; }
    if (!c) { RET(ERROR_SUCCESS_); return; }
    /* XInput has no error for "your structure is not there", so this borrows
     * the one it uses for an argument it cannot make sense of. */
    void *p = W32PN(w, c, 20);
    if (!p) { RET(ERROR_BAD_ARGUMENTS_); return; }
    memset(p, 0, 20);
    w32_write(w, c + 0, 1, 1);          /* XINPUT_DEVTYPE_GAMEPAD */
    w32_write(w, c + 1, 1, 1);          /* XINPUT_DEVSUBTYPE_GAMEPAD */
    w32_write(w, c + 2, 2, 0);          /* no wireless/voice flags */
    /* the gamepad field says which controls exist, as a mask of "all bits set
     * for what is supported" -- every button and both sticks */
    w32_write(w, c + 4, 2, 0xF3FF);
    w32_write(w, c + 6, 1, 0xFF);
    w32_write(w, c + 7, 1, 0xFF);
    for (int i = 0; i < 4; i++) w32_write(w, c + 8 + 2u * (unsigned)i, 2, 0xFFC0);
    RET(ERROR_SUCCESS_);
}
static void x_XInputEnable(w32 *w) { (void)w; }
static void x_XInputGetBatteryInformation(w32 *w) {
    uint64_t b = ARG(2);
    if (b) { w32_write(w, b + 0, 1, 1); w32_write(w, b + 1, 1, 3); }  /* wired, full */
    RET((uint32_t)ARG(0) == 0 ? ERROR_SUCCESS_ : ERROR_DEVICE_NOT_CONNECTED_);
}
/* Keystroke events rather than polled state. Nothing queues them here, so the
 * honest answer is "no events", which is what a game handles when the user is
 * not touching the pad. */
static void x_XInputGetKeystroke(w32 *w) { (void)w; RET(ERROR_EMPTY_); }
static void x_XInputGetAudioDeviceIds(w32 *w) {
    if (ARG(1)) w32_write(w, ARG(1), 2, 0);
    if (ARG(3)) w32_write(w, ARG(3), 2, 0);
    RET(ERROR_DEVICE_NOT_CONNECTED_);
}
static void x_XInputGetDSoundAudioDeviceGuids(w32 *w) {
    void *a = W32PN(w, ARG(1), 16), *b = W32PN(w, ARG(2), 16);
    if (a) memset(a, 0, 16);
    if (b) memset(b, 0, 16);
    RET(ERROR_DEVICE_NOT_CONNECTED_);
}
/* Ordinal 100, undocumented, and imported by ordinal by a good number of
 * games that want the Guide button. Same shape as XInputGetState. */
static void x_XInputGetStateEx(w32 *w) { x_XInputGetState(w); }

void w32_pad_current(w32_pad *p) { if (g_pad_set) *p = g_pad; else pad_from_keyboard(p); }
void w32_xinput_reset(void) { g_pad_set = 0; g_packet = 0; memset(&g_pad, 0, sizeof g_pad); }

#define F(n, a)  { #n, a, 0, x_##n, 0 }
const w32_api w32_xinput[] = {
    F(XInputGetState, 2), F(XInputSetState, 2), F(XInputGetCapabilities, 3),
    F(XInputEnable, 1), F(XInputGetBatteryInformation, 3), F(XInputGetKeystroke, 3),
    F(XInputGetAudioDeviceIds, 4), F(XInputGetDSoundAudioDeviceGuids, 3),
    F(XInputGetStateEx, 2),
    { 0, 0, 0, 0, 0 },
};
