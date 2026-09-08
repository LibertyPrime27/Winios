/* XAudio2, the way a game uses it: an engine, a mastering voice, a source
 * voice with a callback, buffers submitted ahead and reported back one by
 * one as they finish, in order, with the stream's end after the last.
 *
 * No xaudio2.h: the vtables are declared here from the slot order the
 * implementation is checked against, and the DLL is loaded by name -- which
 * is how a game with an optional XAudio2 path does it, and what makes this a
 * test of the whole route including LoadLibrary and GetProcAddress. The 2.7
 * layout is reached the way 2.7 is: CoCreateInstance on its CLSID.
 *
 * Nothing here needs a speaker. Without a device the mixer runs on the wall
 * clock, so the buffers still end; the loop below waits for that, bounded. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>

typedef struct XA2 XA2; typedef struct XAV XAV; typedef struct CB CB;
typedef struct { UINT32 Flags, AudioBytes; const BYTE *pAudioData; UINT32 PlayBegin, PlayLength, LoopBegin, LoopLength, LoopCount; void *pContext; } XBUF;
typedef struct { void *pCurrentBufferContext; UINT32 BuffersQueued; UINT64 SamplesPlayed; } XSTATE;
typedef struct {
    HRESULT (WINAPI *QI)(XA2 *, const GUID *, void **); ULONG (WINAPI *AddRef)(XA2 *); ULONG (WINAPI *Release)(XA2 *);
    HRESULT (WINAPI *RegisterForCallbacks)(XA2 *, void *); void (WINAPI *UnregisterForCallbacks)(XA2 *, void *);
    HRESULT (WINAPI *CreateSourceVoice)(XA2 *, XAV **, const WAVEFORMATEX *, UINT32, float, CB *, const void *, const void *);
    HRESULT (WINAPI *CreateSubmixVoice)(XA2 *, XAV **, UINT32, UINT32, UINT32, UINT32, const void *, const void *);
    HRESULT (WINAPI *CreateMasteringVoice)(XA2 *, XAV **, UINT32, UINT32, UINT32, LPCWSTR, const void *, int);
    HRESULT (WINAPI *StartEngine)(XA2 *); void (WINAPI *StopEngine)(XA2 *); HRESULT (WINAPI *CommitChanges)(XA2 *, UINT32);
    void (WINAPI *GetPerformanceData)(XA2 *, void *); void (WINAPI *SetDebugConfiguration)(XA2 *, const void *, void *);
} XA9Vtbl;
typedef struct {
    HRESULT (WINAPI *QI)(XA2 *, const GUID *, void **); ULONG (WINAPI *AddRef)(XA2 *); ULONG (WINAPI *Release)(XA2 *);
    HRESULT (WINAPI *GetDeviceCount)(XA2 *, UINT32 *); HRESULT (WINAPI *GetDeviceDetails)(XA2 *, UINT32, void *); HRESULT (WINAPI *Initialize)(XA2 *, UINT32, UINT32);
    HRESULT (WINAPI *RegisterForCallbacks)(XA2 *, void *); void (WINAPI *UnregisterForCallbacks)(XA2 *, void *);
    HRESULT (WINAPI *CreateSourceVoice)(XA2 *, XAV **, const WAVEFORMATEX *, UINT32, float, CB *, const void *, const void *);
    HRESULT (WINAPI *CreateSubmixVoice)(XA2 *, XAV **, UINT32, UINT32, UINT32, UINT32, const void *, const void *);
    HRESULT (WINAPI *CreateMasteringVoice)(XA2 *, XAV **, UINT32, UINT32, UINT32, UINT32, const void *);
    HRESULT (WINAPI *StartEngine)(XA2 *); void (WINAPI *StopEngine)(XA2 *); HRESULT (WINAPI *CommitChanges)(XA2 *, UINT32);
    void (WINAPI *GetPerformanceData)(XA2 *, void *); void (WINAPI *SetDebugConfiguration)(XA2 *, const void *, void *);
} XA7Vtbl;
struct XA2 { const void *v; };
typedef struct {
    void (WINAPI *GetVoiceDetails)(XAV *, void *); HRESULT (WINAPI *SetOutputVoices)(XAV *, const void *); HRESULT (WINAPI *SetEffectChain)(XAV *, const void *);
    HRESULT (WINAPI *EnableEffect)(XAV *, UINT32, UINT32); HRESULT (WINAPI *DisableEffect)(XAV *, UINT32, UINT32); void (WINAPI *GetEffectState)(XAV *, UINT32, BOOL *);
    HRESULT (WINAPI *SetEffectParameters)(XAV *, UINT32, const void *, UINT32, UINT32); HRESULT (WINAPI *GetEffectParameters)(XAV *, UINT32, void *, UINT32);
    HRESULT (WINAPI *SetFilterParameters)(XAV *, const void *, UINT32); void (WINAPI *GetFilterParameters)(XAV *, void *);
    HRESULT (WINAPI *SetOutputFilterParameters)(XAV *, XAV *, const void *, UINT32); void (WINAPI *GetOutputFilterParameters)(XAV *, XAV *, void *);
    HRESULT (WINAPI *SetVolume)(XAV *, float, UINT32); void (WINAPI *GetVolume)(XAV *, float *);
    HRESULT (WINAPI *SetChannelVolumes)(XAV *, UINT32, const float *, UINT32); void (WINAPI *GetChannelVolumes)(XAV *, UINT32, float *);
    HRESULT (WINAPI *SetOutputMatrix)(XAV *, XAV *, UINT32, UINT32, const float *, UINT32); void (WINAPI *GetOutputMatrix)(XAV *, XAV *, UINT32, UINT32, float *);
    void (WINAPI *DestroyVoice)(XAV *);
    HRESULT (WINAPI *Start)(XAV *, UINT32, UINT32); HRESULT (WINAPI *Stop)(XAV *, UINT32, UINT32);
    HRESULT (WINAPI *SubmitSourceBuffer)(XAV *, const XBUF *, const void *); HRESULT (WINAPI *FlushSourceBuffers)(XAV *); HRESULT (WINAPI *Discontinuity)(XAV *);
    HRESULT (WINAPI *ExitLoop)(XAV *, UINT32); void (WINAPI *GetState)(XAV *, XSTATE *, UINT32); HRESULT (WINAPI *SetFrequencyRatio)(XAV *, float, UINT32);
    void (WINAPI *GetFrequencyRatio)(XAV *, float *); HRESULT (WINAPI *SetSourceSampleRate)(XAV *, UINT32);
} XAVVtbl;
struct XAV { const XAVVtbl *v; };
typedef struct {
    void (WINAPI *PassStart)(CB *, UINT32); void (WINAPI *PassEnd)(CB *); void (WINAPI *StreamEnd)(CB *);
    void (WINAPI *BufferStart)(CB *, void *); void (WINAPI *BufferEnd)(CB *, void *); void (WINAPI *LoopEnd)(CB *, void *);
    void (WINAPI *VoiceError)(CB *, void *, HRESULT);
} CBVtbl;
struct CB { const CBVtbl *v; };

static char g_events[512];
static void add(const char *fmt, void *ctx) { char t[32]; snprintf(t, sizeof t, fmt, (int)(INT_PTR)ctx); strcat(g_events, t); }
static void WINAPI cb_pass_start(CB *c, UINT32 n) { (void)c; (void)n; }
static void WINAPI cb_pass_end(CB *c) { (void)c; }
static void WINAPI cb_stream_end(CB *c) { (void)c; strcat(g_events, "X "); }
static void WINAPI cb_buffer_start(CB *c, void *ctx) { (void)c; add("S%d ", ctx); }
static void WINAPI cb_buffer_end(CB *c, void *ctx) { (void)c; add("E%d ", ctx); }
static void WINAPI cb_loop_end(CB *c, void *ctx) { (void)c; add("L%d ", ctx); }
static void WINAPI cb_error(CB *c, void *ctx, HRESULT hr) { (void)c; (void)ctx; (void)hr; strcat(g_events, "ERR "); }
static const CBVtbl cb_vtbl = { cb_pass_start, cb_pass_end, cb_stream_end, cb_buffer_start, cb_buffer_end, cb_loop_end, cb_error };
static CB g_cb = { &cb_vtbl };

static short g_pcm[3][2205];   /* three 50 ms blocks at 44100 Hz, mono, 16-bit */

int main(void) {
    for (int b = 0; b < 3; b++) for (int i = 0; i < 2205; i++) g_pcm[b][i] = (short)(((i * 440 * 2 / 441) % 200) * 100 - 10000);   /* a sawtooth; no libm */
    typedef HRESULT (WINAPI *create_t)(XA2 **, UINT32, UINT32);
    HMODULE m = LoadLibraryA("xaudio2_9.dll");
    create_t create = (create_t)GetProcAddress(m, "XAudio2Create");
    XA2 *xa = NULL;
    HRESULT hr = create ? create(&xa, 0, 1) : E_FAIL;
    printf("XAudio2Create: %s\n", SUCCEEDED(hr) && xa ? "ok" : "FAILED");
    if (!xa) return 1;
    const XA9Vtbl *E = xa->v;
    XAV *master = NULL, *src = NULL;
    hr = E->CreateMasteringVoice(xa, &master, 0, 0, 0, NULL, NULL, 0);
    printf("mastering voice: %s\n", SUCCEEDED(hr) && master ? "ok" : "FAILED");
    WAVEFORMATEX wf; memset(&wf, 0, sizeof wf);
    wf.wFormatTag = 1; wf.nChannels = 1; wf.nSamplesPerSec = 44100; wf.wBitsPerSample = 16; wf.nBlockAlign = 2; wf.nAvgBytesPerSec = 88200;
    hr = E->CreateSourceVoice(xa, &src, &wf, 0, 2.0f, &g_cb, NULL, NULL);
    printf("source voice: %s\n", SUCCEEDED(hr) && src ? "ok" : "FAILED");
    if (!src) return 1;
    const XAVVtbl *V = src->v;
    float vol = 0; V->SetVolume(src, 0.5f, 0); V->GetVolume(src, &vol);
    float ratio = 0; V->SetFrequencyRatio(src, 1.5f, 0); V->GetFrequencyRatio(src, &ratio);
    printf("volume %.2f, frequency ratio %.2f\n", vol, ratio);

    for (int b = 0; b < 3; b++) {
        XBUF buf; memset(&buf, 0, sizeof buf);
        buf.AudioBytes = sizeof g_pcm[b]; buf.pAudioData = (const BYTE *)g_pcm[b]; buf.pContext = (void *)(INT_PTR)(b + 1);
        if (b == 2) buf.Flags = 0x40;                                /* XAUDIO2_END_OF_STREAM */
        hr = V->SubmitSourceBuffer(src, &buf, NULL);
        if (FAILED(hr)) printf("submit %d FAILED\n", b + 1);
    }
    XSTATE st; memset(&st, 0, sizeof st);
    V->GetState(src, &st, 0);
    printf("before Start: %lu queued, current context %d, %llu samples played\n", (unsigned long)st.BuffersQueued, (int)(INT_PTR)st.pCurrentBufferContext, (unsigned long long)st.SamplesPlayed);
    V->Start(src, 0, 0);
    int waited = 0;
    for (; waited < 300; waited++) { V->GetState(src, &st, 0); if (st.BuffersQueued == 0) break; Sleep(10); }
    long long played = (long long)st.SamplesPlayed;
    printf("after playing: %lu queued, samples played %s 6615, events: %s\n", (unsigned long)st.BuffersQueued,
           played >= 6612 && played <= 6618 ? "about" : "NOT NEAR", g_events);

    /* flush while stopped: everything goes, and each buffer is reported ended */
    g_events[0] = 0;
    V->Stop(src, 0, 0);
    for (int b = 0; b < 2; b++) {
        XBUF buf; memset(&buf, 0, sizeof buf);
        buf.AudioBytes = sizeof g_pcm[0]; buf.pAudioData = (const BYTE *)g_pcm[0]; buf.pContext = (void *)(INT_PTR)(b + 4);
        V->SubmitSourceBuffer(src, &buf, NULL);
    }
    V->GetState(src, &st, 0);
    unsigned long before = st.BuffersQueued;
    V->FlushSourceBuffers(src);
    V->GetState(src, &st, 0);
    printf("flush: %lu queued before, %lu after, events: %s\n", before, (unsigned long)st.BuffersQueued, g_events);
    V->DestroyVoice(src);
    master->v->DestroyVoice(master);
    E->Release(xa);

    /* 2.7: the class, through COM */
    CoInitialize(NULL);
    GUID clsid = { 0x5a508685, 0xa254, 0x4fba, { 0x9b,0x82,0x9a,0x24,0xb0,0x03,0x06,0xaf } };
    GUID iid   = { 0x8bcf1f58, 0x9fe7, 0x4583, { 0x8a,0xc6,0xe2,0xad,0xc4,0x65,0xc8,0xbb } };
    XA2 *xa7 = NULL;
    hr = CoCreateInstance(&clsid, NULL, 1, &iid, (void **)&xa7);
    UINT32 devices = 0;
    if (xa7) { const XA7Vtbl *E7 = xa7->v; E7->Initialize(xa7, 0, 1); E7->GetDeviceCount(xa7, &devices); E7->Release(xa7); }
    printf("2.7 through CoCreateInstance: %s, %lu device\n", SUCCEEDED(hr) && xa7 ? "ok" : "FAILED", (unsigned long)devices);
    return 0;
}
