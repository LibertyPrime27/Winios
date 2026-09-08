/* DirectInput, as a game from before XInput uses it.
 *
 * Keys and mouse motion are injected before the program starts (see
 * dinputtest.script), so what is checked is *state*: the keyboard in DIK
 * scancodes, the mouse as relative motion that is consumed by being read,
 * the keyboard-as-gamepad through a DIJOYSTATE2 with a range the program
 * chose, and the older dinput.dll entry points reaching the same devices.
 * Buffered data is asked for too: what was already held at Acquire is state,
 * not events, so the queue starts empty -- a game that drained a phantom
 * "key down" for every key the player was holding would act on it. */
#define DIRECTINPUT_VERSION 0x0800
#define INITGUID
#include <windows.h>
#include <dinput.h>
#include <stdio.h>

static void narrow(const WCHAR *w, char *out, size_t n) { size_t i = 0; for (; w[i] && i + 1 < n; i++) out[i] = (char)w[i]; out[i] = 0; }
static BOOL CALLBACK enum_cb(LPCDIDEVICEINSTANCEW d, LPVOID ref) {
    char name[64]; narrow(d->tszInstanceName, name, sizeof name);
    printf("  %s (type %#lx)\n", name, (unsigned long)d->dwDevType);
    (*(int *)ref)++;
    return DIENUM_CONTINUE;
}
static BOOL CALLBACK count_cb(LPCDIDEVICEOBJECTINSTANCEW o, LPVOID ref) { (void)o; (*(int *)ref)++; return DIENUM_CONTINUE; }

int main(void) {
    IDirectInput8W *di = NULL;
    HRESULT hr = DirectInput8Create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8W, (void **)&di, NULL);
    printf("DirectInput8Create: %s\n", SUCCEEDED(hr) && di ? "ok" : "FAILED");
    if (!di) return 1;
    int n = 0;
    IDirectInput8_EnumDevices(di, DI8DEVCLASS_ALL, enum_cb, &n, DIEDFL_ATTACHEDONLY);
    printf("EnumDevices: %d devices\n", n);

    IDirectInputDevice8W *kb = NULL;
    hr = IDirectInput8_CreateDevice(di, &GUID_SysKeyboard, &kb, NULL);
    printf("keyboard: %s\n", SUCCEEDED(hr) && kb ? "created" : "FAILED");
    if (!kb) return 1;
    IDirectInputDevice8_SetDataFormat(kb, &c_dfDIKeyboard);
    DIPROPDWORD dp; memset(&dp, 0, sizeof dp);
    dp.diph.dwSize = sizeof dp; dp.diph.dwHeaderSize = sizeof dp.diph; dp.diph.dwHow = DIPH_DEVICE; dp.dwData = 16;
    printf("buffer size: %s\n", SUCCEEDED(IDirectInputDevice8_SetProperty(kb, DIPROP_BUFFERSIZE, &dp.diph)) ? "ok" : "FAILED");
    IDirectInputDevice8_SetCooperativeLevel(kb, NULL, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    BYTE keys[256];
    hr = IDirectInputDevice8_GetDeviceState(kb, sizeof keys, keys);
    printf("GetDeviceState before Acquire: %s\n", hr == DIERR_NOTACQUIRED ? "DIERR_NOTACQUIRED, as it should be" : "WRONG");
    IDirectInputDevice8_Acquire(kb);
    hr = IDirectInputDevice8_GetDeviceState(kb, sizeof keys, keys);
    printf("W %s, D %s, Space %s, Q %s\n", keys[DIK_W] & 0x80 ? "down" : "up", keys[DIK_D] & 0x80 ? "down" : "up",
           keys[DIK_SPACE] & 0x80 ? "down" : "up", keys[DIK_Q] & 0x80 ? "down" : "up");
    DIDEVICEOBJECTDATA od[8]; DWORD cnt = 8;
    hr = IDirectInputDevice8_GetDeviceData(kb, sizeof od[0], od, &cnt, 0);
    printf("GetDeviceData: %s, %lu events (what was held at Acquire is state, not events)\n", SUCCEEDED(hr) ? "ok" : "FAILED", (unsigned long)cnt);

    IDirectInputDevice8W *ms = NULL;
    hr = IDirectInput8_CreateDevice(di, &GUID_SysMouse, &ms, NULL);
    IDirectInputDevice8_SetDataFormat(ms, &c_dfDIMouse2);
    IDirectInputDevice8_Acquire(ms);
    DIMOUSESTATE2 m; memset(&m, 0, sizeof m);
    hr = IDirectInputDevice8_GetDeviceState(ms, sizeof m, &m);
    printf("mouse: %s, dx %ld dy %ld wheel %ld, left %s, right %s\n", SUCCEEDED(hr) ? "created" : "FAILED",
           m.lX, m.lY, m.lZ, m.rgbButtons[0] & 0x80 ? "down" : "up", m.rgbButtons[1] & 0x80 ? "down" : "up");
    IDirectInputDevice8_GetDeviceState(ms, sizeof m, &m);
    printf("mouse again: dx %ld dy %ld (relative: consumed by the first read)\n", m.lX, m.lY);

    IDirectInputDevice8W *js = NULL;
    hr = IDirectInput8_CreateDevice(di, &GUID_Joystick, &js, NULL);
    IDirectInputDevice8_SetDataFormat(js, &c_dfDIJoystick2);
    DIPROPRANGE rg; memset(&rg, 0, sizeof rg);
    rg.diph.dwSize = sizeof rg; rg.diph.dwHeaderSize = sizeof rg.diph; rg.diph.dwHow = DIPH_DEVICE; rg.lMin = -1000; rg.lMax = 1000;
    HRESULT hr2 = IDirectInputDevice8_SetProperty(js, DIPROP_RANGE, &rg.diph);
    DIDEVCAPS caps; memset(&caps, 0, sizeof caps); caps.dwSize = sizeof caps;
    IDirectInputDevice8_GetCapabilities(js, &caps);
    int objs = 0;
    IDirectInputDevice8_EnumObjects(js, count_cb, &objs, DIDFT_ALL);
    printf("gamepad: %s, range %s, caps: %lu axes %lu buttons %lu POV, %d objects\n", SUCCEEDED(hr) && js ? "created" : "FAILED",
           SUCCEEDED(hr2) ? "set" : "REFUSED", (unsigned long)caps.dwAxes, (unsigned long)caps.dwButtons, (unsigned long)caps.dwPOVs, objs);
    IDirectInputDevice8_Acquire(js);
    IDirectInputDevice8_Poll(js);
    DIJOYSTATE2 j; memset(&j, 0, sizeof j);
    IDirectInputDevice8_GetDeviceState(js, sizeof j, &j);
    printf("gamepad state: lX %ld lY %ld button0 %s POV %s\n", j.lX, j.lY, j.rgbButtons[0] & 0x80 ? "down" : "up",
           j.rgdwPOV[0] == 0xFFFFFFFF ? "centred" : "MOVED");

    /* the older entry points: dinput.dll, IDirectInput7A, CreateDeviceEx */
    /* DirectInputCreateEx is not in mingw's import library, so it is fetched
     * the way a game with an optional DirectInput path fetches it -- which
     * also loads dinput.dll by name. */
    typedef HRESULT (WINAPI *dice_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
    dice_t dice = (dice_t)GetProcAddress(LoadLibraryA("dinput.dll"), "DirectInputCreateEx");
    IDirectInput7A *di7 = NULL;
    hr = dice ? dice(GetModuleHandleA(NULL), 0x0700, &IID_IDirectInput7A, (void **)&di7, NULL) : E_FAIL;
    IDirectInputDevice7A *kb7 = NULL;
    HRESULT hr3 = di7 ? IDirectInput7_CreateDeviceEx(di7, &GUID_SysKeyboard, &IID_IDirectInputDevice7A, (void **)&kb7, NULL) : E_FAIL;
    DIDEVICEINSTANCEA inst; memset(&inst, 0, sizeof inst); inst.dwSize = sizeof inst;
    if (kb7) IDirectInputDevice7_GetDeviceInfo(kb7, &inst);
    printf("dinput.dll: IDirectInput7A %s, keyboard via CreateDeviceEx %s, product %s, type %#lx\n",
           SUCCEEDED(hr) ? "ok" : "FAILED", SUCCEEDED(hr3) ? "ok" : "FAILED", inst.tszProductName, (unsigned long)inst.dwDevType);
    return 0;
}
