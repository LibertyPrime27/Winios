/* WASAPI: the sound API a modern engine calls directly.
 *
 * CoCreateInstance for the device enumerator, the default render endpoint,
 * an IAudioClient activated on it, initialised in event-driven shared mode,
 * fed through IAudioRenderClient and paced by the event -- which is what
 * GameMaker's runner does on its audio thread, and what stopped it before
 * this existed ("no such COM class").
 *
 * Nothing here needs a speaker. What is checked is that every call succeeds
 * with the documented answer, that the padding is exactly what was written
 * and then drains, that a full buffer refuses more, that the event fires
 * while the stream runs, and that the clock moves. Timing is judged as a
 * range, because it is a clock and this is not a real-time system.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void ok(int c, const char *what) { printf("%s %s\n", c ? "ok  " : "FAIL", what); if (!c) fails++; }

int main(void) {
    printf("%d-bit\n", (int)(8 * sizeof(void *)));
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ok(SUCCEEDED(hr), "CoInitializeEx");

    IMMDeviceEnumerator *en = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en);
    ok(SUCCEEDED(hr) && en, "CoCreateInstance(MMDeviceEnumerator)");
    if (!en) { printf("%d failures\n", fails); return 1; }

    IMMDevice *dev = NULL;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    ok(SUCCEEDED(hr) && dev, "GetDefaultAudioEndpoint(eRender)");
    IMMDevice *cap = NULL;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eCapture, eConsole, &cap);
    ok(FAILED(hr) && !cap, "GetDefaultAudioEndpoint(eCapture) says there is none");
    if (!dev) { printf("%d failures\n", fails); return 1; }

    IMMDeviceCollection *col = NULL;
    hr = IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col);
    UINT count = 0;
    if (SUCCEEDED(hr) && col) IMMDeviceCollection_GetCount(col, &count);
    ok(SUCCEEDED(hr) && count == 1, "EnumAudioEndpoints: one active render device");
    if (col) {
        IMMDevice *item = NULL;
        ok(SUCCEEDED(IMMDeviceCollection_Item(col, 0, &item)) && item, "  Item(0)");
        if (item) IMMDevice_Release(item);
        IMMDeviceCollection_Release(col);
    }

    LPWSTR id = NULL;
    hr = IMMDevice_GetId(dev, &id);
    ok(SUCCEEDED(hr) && id && wcslen(id) > 10 && id[0] == L'{', "GetId gives an endpoint id");
    if (id) {
        IMMDevice *again = NULL;
        ok(SUCCEEDED(IMMDeviceEnumerator_GetDevice(en, id, &again)) && again, "  GetDevice by that id finds it");
        if (again) IMMDevice_Release(again);
        CoTaskMemFree(id);
    }
    DWORD state = 0;
    ok(SUCCEEDED(IMMDevice_GetState(dev, &state)) && state == DEVICE_STATE_ACTIVE, "GetState: active");

    IPropertyStore *ps = NULL;
    hr = IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps);
    ok(SUCCEEDED(hr) && ps, "OpenPropertyStore");
    if (ps) {
        PROPVARIANT pv; PropVariantInit(&pv);
        hr = IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &pv);
        ok(SUCCEEDED(hr) && pv.vt == VT_LPWSTR && pv.pwszVal && wcslen(pv.pwszVal) > 0, "  PKEY_Device_FriendlyName is a string");
        if (SUCCEEDED(hr) && pv.vt == VT_LPWSTR) printf("  device: %ls\n", pv.pwszVal);
        PropVariantClear(&pv);
        IPropertyStore_Release(ps);
    }

    IAudioClient *ac = NULL;
    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    ok(SUCCEEDED(hr) && ac, "Activate(IAudioClient)");
    if (!ac) { printf("%d failures\n", fails); return 1; }

    WAVEFORMATEX *mix = NULL;
    hr = IAudioClient_GetMixFormat(ac, &mix);
    ok(SUCCEEDED(hr) && mix && mix->nChannels == 2 && mix->nSamplesPerSec >= 44100, "GetMixFormat: stereo, 44.1 kHz or better");
    if (mix) { printf("  mix format: %u Hz, %u ch, %u bits, tag %#x\n", (unsigned)mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample, mix->wFormatTag); CoTaskMemFree(mix); }
    REFERENCE_TIME def = 0, min = 0;
    ok(SUCCEEDED(IAudioClient_GetDevicePeriod(ac, &def, &min)) && def > 0 && min > 0 && min <= def, "GetDevicePeriod");

    WAVEFORMATEX wf; memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = 44100; wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4; wf.nAvgBytesPerSec = 44100 * 4;
    WAVEFORMATEX *closest = NULL;
    hr = IAudioClient_IsFormatSupported(ac, AUDCLNT_SHAREMODE_SHARED, &wf, &closest);
    ok(hr == S_OK, "IsFormatSupported(16-bit stereo PCM): yes");
    if (closest) CoTaskMemFree(closest);
    WAVEFORMATEX bad = wf; bad.wFormatTag = 0x55;             /* MP3 */
    closest = NULL;
    hr = IAudioClient_IsFormatSupported(ac, AUDCLNT_SHAREMODE_SHARED, &bad, &closest);
    ok(hr == S_FALSE && closest != NULL, "IsFormatSupported(MP3): no, with the closest match offered");
    if (closest) CoTaskMemFree(closest);

    UINT32 frames = 0;
    hr = IAudioClient_GetBufferSize(ac, &frames);
    ok(hr == AUDCLNT_E_NOT_INITIALIZED, "GetBufferSize before Initialize: AUDCLNT_E_NOT_INITIALIZED");

    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000 /* 100 ms */, 0, &wf, NULL);
    ok(SUCCEEDED(hr), "Initialize(shared, event-driven, 100 ms)");
    hr = IAudioClient_GetBufferSize(ac, &frames);
    ok(SUCCEEDED(hr) && frames >= 4410 && frames <= 44100, "GetBufferSize: at least 100 ms, at most a second");
    UINT32 pad = 99;
    ok(SUCCEEDED(IAudioClient_GetCurrentPadding(ac, &pad)) && pad == 0, "GetCurrentPadding: nothing queued yet");

    IAudioRenderClient *rc = NULL;
    hr = IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc);
    ok(SUCCEEDED(hr) && rc, "GetService(IAudioRenderClient)");
    IAudioClock *clock = NULL;
    ok(SUCCEEDED(IAudioClient_GetService(ac, &IID_IAudioClock, (void **)&clock)) && clock, "GetService(IAudioClock)");
    ISimpleAudioVolume *vol = NULL;
    ok(SUCCEEDED(IAudioClient_GetService(ac, &IID_ISimpleAudioVolume, (void **)&vol)) && vol, "GetService(ISimpleAudioVolume)");
    if (!rc) { printf("%d failures\n", fails); return 1; }

    hr = IAudioClient_Start(ac);
    ok(hr == AUDCLNT_E_EVENTHANDLE_NOT_SET, "Start before SetEventHandle: AUDCLNT_E_EVENTHANDLE_NOT_SET");
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(SUCCEEDED(IAudioClient_SetEventHandle(ac, ev)), "SetEventHandle");

    /* fill the whole buffer with a tone */
    BYTE *data = NULL;
    hr = IAudioRenderClient_GetBuffer(rc, frames, &data);
    ok(SUCCEEDED(hr) && data, "GetBuffer(the whole buffer)");
    if (data) {
        short *s = (short *)data;
        for (UINT32 i = 0; i < frames; i++) { short v = (short)(8000.0 * sin(i * 6.2831853 * 440.0 / 44100.0)); s[2 * i] = v; s[2 * i + 1] = v; }
        hr = IAudioRenderClient_ReleaseBuffer(rc, frames, 0);
        ok(SUCCEEDED(hr), "ReleaseBuffer");
    }
    ok(SUCCEEDED(IAudioClient_GetCurrentPadding(ac, &pad)) && pad == frames, "padding equals what was written: yes");
    hr = IAudioRenderClient_GetBuffer(rc, 1, &data);
    ok(hr == AUDCLNT_E_BUFFER_TOO_LARGE, "a full buffer refuses one more frame: AUDCLNT_E_BUFFER_TOO_LARGE");

    LARGE_INTEGER freq, t0, t1; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
    ok(SUCCEEDED(IAudioClient_Start(ac)), "Start");
    DWORD wr = WaitForSingleObject(ev, 1000);
    QueryPerformanceCounter(&t1);
    long ms = (long)((t1.QuadPart - t0.QuadPart) * 1000 / freq.QuadPart);
    ok(wr == WAIT_OBJECT_0, "the event fires while the stream runs");
    printf("  first event after %s\n", ms < 5 ? "less than 5 ms" : ms < 500 ? "5 to 500 ms" : "MORE THAN 500 ms");
    UINT32 pad2 = 0;
    IAudioClient_GetCurrentPadding(ac, &pad2);
    ok(pad2 < frames, "padding drains once started: yes");

    /* keep it fed for a while, the way a mixer thread does */
    int refills = 0, events = 0;
    for (int i = 0; i < 20; i++) {
        if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) break;
        events++;
        UINT32 p = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(ac, &p))) break;
        UINT32 room = frames - p;
        if (!room) continue;
        if (FAILED(IAudioRenderClient_GetBuffer(rc, room, &data)) || !data) break;
        memset(data, 0, (size_t)room * 4);
        if (FAILED(IAudioRenderClient_ReleaseBuffer(rc, room, AUDCLNT_BUFFERFLAGS_SILENT))) break;
        refills++;
    }
    ok(events >= 10 && refills >= 5, "twenty periods of events, and the buffer was refilled through them");

    UINT64 pos = 0, qpc = 0, cfreq = 0;
    if (clock) {
        ok(SUCCEEDED(IAudioClock_GetFrequency(clock, &cfreq)) && cfreq > 0, "IAudioClock::GetFrequency");
        ok(SUCCEEDED(IAudioClock_GetPosition(clock, &pos, &qpc)) && pos > 0 && qpc > 0, "IAudioClock::GetPosition has moved");
    }
    if (vol) {
        float f = 0;
        ok(SUCCEEDED(ISimpleAudioVolume_SetMasterVolume(vol, 0.5f, NULL)) && SUCCEEDED(ISimpleAudioVolume_GetMasterVolume(vol, &f)) && f == 0.5f, "ISimpleAudioVolume round trip: 0.5");
        BOOL mute = TRUE;
        ok(SUCCEEDED(ISimpleAudioVolume_GetMute(vol, &mute)) && !mute, "  not muted");
    }

    ok(IAudioClient_Reset(ac) == AUDCLNT_E_NOT_STOPPED, "Reset while running: AUDCLNT_E_NOT_STOPPED");
    ok(IAudioClient_Stop(ac) == S_OK, "Stop");
    ok(IAudioClient_Stop(ac) == S_FALSE, "Stop again: S_FALSE");
    ok(IAudioClient_Reset(ac) == S_OK, "Reset");
    ok(SUCCEEDED(IAudioClient_GetCurrentPadding(ac, &pad)) && pad == 0, "padding after Reset: 0");

    if (vol) ISimpleAudioVolume_Release(vol);
    if (clock) IAudioClock_Release(clock);
    IAudioRenderClient_Release(rc);
    IAudioClient_Release(ac);
    IMMDevice_Release(dev);
    IMMDeviceEnumerator_Release(en);
    CloseHandle(ev);
    CoUninitialize();
    printf("%d failures\n", fails);
    return fails ? 1 : 0;
}
