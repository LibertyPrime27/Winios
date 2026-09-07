/* Audio that initialises, and a gamepad.
 *
 * Neither makes a sound or needs a controller plugged in, which is the point:
 * the failure this guards against is a game *aborting* because audio init
 * returned an error, or ignoring input because XInput said no pad was
 * attached. So what is checked is that every call succeeds, that a buffer is
 * real memory the guest can write to and read back, that the play cursor
 * moves at the rate the format implies, and that a keyboard reaches the pad.
 *
 * The Lock wrap case is worth its own check: a circular buffer means a lock
 * near the end returns two pointers, and a program handed only the first
 * writes past the end of the buffer.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmreg.h>
#include <mmsystem.h>
#define INITGUID
#include <initguid.h>
#include <dsound.h>
#include <xinput.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    printf("%d-bit\n", (int)(8 * sizeof(void *)));

    /* --- DirectSound --- */
    LPDIRECTSOUND8 ds = NULL;
    HRESULT hr = DirectSoundCreate8(NULL, &ds, NULL);
    printf("\nDirectSoundCreate8: %s\n", SUCCEEDED(hr) && ds ? "ok" : "FAILED");
    if (!ds) return 1;
    printf("SetCooperativeLevel: %s\n", SUCCEEDED(IDirectSound8_SetCooperativeLevel(ds, NULL, DSSCL_PRIORITY)) ? "ok" : "FAILED");

    DSCAPS caps; memset(&caps, 0, sizeof caps); caps.dwSize = sizeof caps;
    /* Two statements, not one printf: C does not order the arguments of a
     * call, so a field read in the same printf as the call that fills it can
     * legally be read first -- and was. */
    hr = IDirectSound8_GetCaps(ds, &caps);
    printf("GetCaps: %s, primary buffers %lu\n",
           SUCCEEDED(hr) ? "ok" : "FAILED", (unsigned long)caps.dwPrimaryBuffers);

    WAVEFORMATEX wf; memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 44100;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = 44100 * 4;

    DSBUFFERDESC bd; memset(&bd, 0, sizeof bd);
    bd.dwSize = sizeof bd;
    bd.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_CTRLVOLUME;
    bd.dwBufferBytes = 44100 * 4;               /* one second */
    bd.lpwfxFormat = &wf;

    LPDIRECTSOUNDBUFFER buf = NULL;
    hr = IDirectSound8_CreateSoundBuffer(ds, &bd, &buf, NULL);
    printf("CreateSoundBuffer(1 s): %s\n", SUCCEEDED(hr) && buf ? "ok" : "FAILED");
    if (!buf) return 1;

    DSBCAPS bc; memset(&bc, 0, sizeof bc); bc.dwSize = sizeof bc;
    hr = IDirectSoundBuffer_GetCaps(buf, &bc);
    printf("buffer GetCaps: %s, %lu bytes\n", SUCCEEDED(hr) ? "ok" : "FAILED",
           (unsigned long)bc.dwBufferBytes);

    /* the format we got back has to be the format we asked for */
    WAVEFORMATEX got; DWORD wrote = 0;
    memset(&got, 0, sizeof got);
    IDirectSoundBuffer_GetFormat(buf, &got, sizeof got, &wrote);
    printf("GetFormat: %lu Hz, %u channels, %u bits\n",
           (unsigned long)got.nSamplesPerSec, got.nChannels, got.wBitsPerSample);

    /* write a pattern, read it back: the buffer is memory, not a sink */
    void *p1 = NULL, *p2 = NULL; DWORD n1 = 0, n2 = 0;
    hr = IDirectSoundBuffer_Lock(buf, 0, 1024, &p1, &n1, &p2, &n2, 0);
    printf("Lock(0, 1024): %s, %lu + %lu bytes\n",
           SUCCEEDED(hr) ? "ok" : "FAILED", (unsigned long)n1, (unsigned long)n2);
    if (SUCCEEDED(hr) && p1) {
        unsigned char *b = (unsigned char *)p1;
        for (int i = 0; i < 256; i++) b[i] = (unsigned char)(i ^ 0x5A);
        IDirectSoundBuffer_Unlock(buf, p1, n1, p2, n2);
    }
    p1 = p2 = NULL; n1 = n2 = 0;
    hr = IDirectSoundBuffer_Lock(buf, 0, 256, &p1, &n1, &p2, &n2, 0);
    int same = 1;
    if (SUCCEEDED(hr) && p1) {
        const unsigned char *b = (const unsigned char *)p1;
        for (int i = 0; i < 256; i++) if (b[i] != (unsigned char)(i ^ 0x5A)) same = 0;
        IDirectSoundBuffer_Unlock(buf, p1, n1, p2, n2);
    } else same = 0;
    printf("what was written is what is there: %s\n", same ? "yes" : "NO");

    /* a lock that crosses the end must come back as two pieces */
    p1 = p2 = NULL; n1 = n2 = 0;
    hr = IDirectSoundBuffer_Lock(buf, bd.dwBufferBytes - 512, 1024, &p1, &n1, &p2, &n2, 0);
    printf("Lock across the end: %s, %lu + %lu bytes (want 512 + 512), second pointer %s\n",
           SUCCEEDED(hr) ? "ok" : "FAILED", (unsigned long)n1, (unsigned long)n2,
           p2 ? "given" : "MISSING");
    if (SUCCEEDED(hr)) IDirectSoundBuffer_Unlock(buf, p1, n1, p2, n2);

    DWORD status = 0;
    IDirectSoundBuffer_GetStatus(buf, &status);
    printf("status before Play: %#lx\n", (unsigned long)status);
    printf("Play: %s\n", SUCCEEDED(IDirectSoundBuffer_Play(buf, 0, 0, DSBPLAY_LOOPING)) ? "ok" : "FAILED");
    IDirectSoundBuffer_GetStatus(buf, &status);
    printf("status while playing: %#lx (1 = playing, 4 = looping)\n", (unsigned long)status);

    /* The cursor has to move, and at roughly the buffer's own rate: a game
     * paces its writes by it, so one that runs fast starves the mixer and one
     * that stands still makes it spin. Checked as a range, because it is a
     * clock and this is not a real-time system. */
    DWORD play0 = 0, play1 = 0, write0 = 0;
    IDirectSoundBuffer_GetCurrentPosition(buf, &play0, &write0);
    Sleep(100);
    IDirectSoundBuffer_GetCurrentPosition(buf, &play1, NULL);
    long moved = (long)play1 - (long)play0;
    if (moved < 0) moved += (long)bd.dwBufferBytes;
    /* A wide window on purpose: it is a clock, not a real-time guarantee, and
     * the recording must not contain a timing measurement. What is being
     * tested is the order of magnitude -- that it moves, and at the rate the
     * format implies rather than some other one. */
    long want = (long)wf.nAvgBytesPerSec / 10;
    printf("cursor moved %s in 100 ms (wanted roughly %ld bytes)\n",
           moved > want / 2 && moved < want * 2 ? "about right"
           : moved == 0 ? "NOT AT ALL" : "BY THE WRONG AMOUNT", want);
    printf("write cursor leads the play cursor: %s\n", write0 != play0 ? "yes" : "NO");

    printf("Stop: %s\n", SUCCEEDED(IDirectSoundBuffer_Stop(buf)) ? "ok" : "FAILED");
    IDirectSoundBuffer_GetStatus(buf, &status);
    printf("status after Stop: %#lx\n", (unsigned long)status);

    /* QueryInterface for a 3D buffer must give a different object: a game
     * calls SetPosition on it, which is a different slot in that vtable. */
    LPDIRECTSOUND3DBUFFER b3d = NULL;
    hr = IDirectSoundBuffer_QueryInterface(buf, &IID_IDirectSound3DBuffer, (void **)&b3d);
    printf("QueryInterface(IDirectSound3DBuffer): %s, %s object\n",
           SUCCEEDED(hr) ? "ok" : "FAILED",
           b3d == NULL ? "no" : (void *)b3d != (void *)buf ? "a different" : "THE SAME");
    if (b3d) {
        printf("  SetPosition on it: %s\n",
               SUCCEEDED(IDirectSound3DBuffer_SetPosition(b3d, 1.0f, 2.0f, 3.0f, DS3D_IMMEDIATE)) ? "ok" : "FAILED");
        IDirectSound3DBuffer_Release(b3d);
    }

    IDirectSoundBuffer_Release(buf);
    IDirectSound8_Release(ds);

    /* --- XInput --- */
    printf("\nXInput:\n");
    XINPUT_STATE xs; memset(&xs, 0, sizeof xs);
    DWORD r = XInputGetState(0, &xs);
    printf("  XInputGetState(0): %lu (0 = a pad is there, 1167 = none)\n", (unsigned long)r);
    printf("  XInputGetState(1): %lu (want 1167: only one pad)\n", (unsigned long)XInputGetState(1, &xs));

    XINPUT_CAPABILITIES xc; memset(&xc, 0, sizeof xc);
    r = XInputGetCapabilities(0, XINPUT_FLAG_GAMEPAD, &xc);
    printf("  GetCapabilities: %lu, type %u subtype %u\n", (unsigned long)r, xc.Type, xc.SubType);

    XINPUT_VIBRATION vib; vib.wLeftMotorSpeed = vib.wRightMotorSpeed = 32000;
    printf("  SetState (rumble): %lu\n", (unsigned long)XInputSetState(0, &vib));

    /* The keyboard stands in for the pad, so injected keys show up here. That
     * is the property worth testing: a game that only reads XInput is
     * playable from a keyboard or the on-screen keys. */
    memset(&xs, 0, sizeof xs);
    XInputGetState(0, &xs);
    printf("  buttons %#x, left stick %d,%d, right stick %d,%d, triggers %u/%u\n",
           xs.Gamepad.wButtons, xs.Gamepad.sThumbLX, xs.Gamepad.sThumbLY,
           xs.Gamepad.sThumbRX, xs.Gamepad.sThumbRY,
           xs.Gamepad.bLeftTrigger, xs.Gamepad.bRightTrigger);
    return 0;
}
