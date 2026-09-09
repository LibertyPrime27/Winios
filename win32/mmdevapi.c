/* WASAPI: mmdevapi.dll's device enumerator and audioclient's stream.
 *
 * This is the sound API underneath everything since Vista, and the one a
 * modern engine calls directly: GameMaker's runner asks CoCreateInstance for
 * the MMDeviceEnumerator, takes the default render endpoint, activates an
 * IAudioClient on it, and streams into it from its own mixer thread -- no
 * DirectSound, no XAudio2. Until this file existed that first call answered
 * "no such class", the runner's audio thread never started, and the report
 * said "no device (silent)" for a game that never got the chance to ask.
 *
 * The stream model is WASAPI's own: a buffer of N frames the client fills a
 * piece at a time -- GetCurrentPadding says how much is still queued,
 * GetBuffer hands out room for the rest, ReleaseBuffer commits it -- and, in
 * event mode, an event the engine signals whenever a period's worth has
 * drained. Here each committed piece becomes one block in a queue in front
 * of audio_out's gapless two-block slot, the same arrangement xaudio2.c
 * uses, and the padding is frames committed minus frames the mixer has
 * consumed. The event is set from the tick, which runs on the guest thread
 * inside every wait and Sleep, so a thread blocked in WaitForSingleObject on
 * that event wakes at the rate the device drains.
 *
 * One endpoint: a render device called "Winios Audio", always active. There
 * is no capture, no exclusive mode that differs from shared, no session
 * volume beyond a master gain, and no notification ever fires -- the device
 * does not come and go. Each is answered with the documented code.
 *
 * Slot orders are from mmdeviceapi.h and audioclient.h, and the tables are
 * built at run time because IAudioClient::Initialize takes two 64-bit
 * REFERENCE_TIMEs: on x86 those are two stack slots each, so the stdcall
 * cleanup count depends on the bitness of the guest.
 */
#define _GNU_SOURCE
#include "w32.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { S_OK_ = 0, S_FALSE_ = 1, E_FAIL_ = (int)0x80004005, E_NOTIMPL_ = (int)0x80004001, E_POINTER_ = (int)0x80004003,
       E_INVALIDARG_ = (int)0x80070057, E_NOINTERFACE_ = (int)0x80004002, E_OUTOFMEMORY_ = (int)0x8007000E,
       E_NOTFOUND_ = (int)0x80070490 };
enum { AUDCLNT_E_NOT_INITIALIZED_ = (int)0x88890001, AUDCLNT_E_ALREADY_INITIALIZED_ = (int)0x88890002,
       AUDCLNT_E_WRONG_ENDPOINT_TYPE_ = (int)0x88890003, AUDCLNT_E_NOT_STOPPED_ = (int)0x88890005,
       AUDCLNT_E_BUFFER_TOO_LARGE_ = (int)0x88890006, AUDCLNT_E_OUT_OF_ORDER_ = (int)0x88890007,
       AUDCLNT_E_UNSUPPORTED_FORMAT_ = (int)0x88890008, AUDCLNT_E_INVALID_SIZE_ = (int)0x88890009,
       AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED_ = (int)0x88890011, AUDCLNT_E_EVENTHANDLE_NOT_SET_ = (int)0x88890014,
       AUDCLNT_E_BUFFER_ERROR_ = (int)0x88890018, AUDCLNT_S_BUFFER_EMPTY_ = 0x08890001 };
enum { AUDCLNT_STREAMFLAGS_LOOPBACK_ = 0x20000, AUDCLNT_STREAMFLAGS_EVENTCALLBACK_ = 0x40000,
       AUDCLNT_BUFFERFLAGS_SILENT_ = 2 };
enum { eRender = 0, eCapture = 1, eAll = 2 };
enum { DEVICE_STATE_ACTIVE_ = 1 };
enum { VT_EMPTY_ = 0, VT_LPWSTR_ = 31, VT_BLOB_ = 65 };

/* slot numbers, per the headers */
enum { EN_EnumAudioEndpoints = 3, EN_GetDefaultAudioEndpoint, EN_GetDevice, EN_RegisterEndpointNotificationCallback,
       EN_UnregisterEndpointNotificationCallback, EN_NSLOTS };
enum { COL_GetCount = 3, COL_Item, COL_NSLOTS };
enum { DEV_Activate = 3, DEV_OpenPropertyStore, DEV_GetId, DEV_GetState, DEV_NSLOTS };
enum { EP_GetDataFlow = 3, EP_NSLOTS };
enum { PS_GetCount = 3, PS_GetAt, PS_GetValue, PS_SetValue, PS_Commit, PS_NSLOTS };
enum { AC_Initialize = 3, AC_GetBufferSize, AC_GetStreamLatency, AC_GetCurrentPadding, AC_IsFormatSupported, AC_GetMixFormat,
       AC_GetDevicePeriod, AC_Start, AC_Stop, AC_Reset, AC_SetEventHandle, AC_GetService,
       AC2_IsOffloadCapable, AC2_SetClientProperties, AC2_GetBufferSizeLimits,
       AC3_GetSharedModeEnginePeriod, AC3_GetCurrentSharedModeEnginePeriod, AC3_InitializeSharedAudioStream, AC_NSLOTS };
enum { RC_GetBuffer = 3, RC_ReleaseBuffer, RC_NSLOTS };
enum { CK_GetFrequency = 3, CK_GetPosition, CK_GetCharacteristics, CK_NSLOTS };
enum { SV_SetMasterVolume = 3, SV_GetMasterVolume, SV_SetMute, SV_GetMute, SV_NSLOTS };
enum { STV_GetChannelCount = 3, STV_SetChannelVolume, STV_GetChannelVolume, STV_SetAllVolumes, STV_GetAllVolumes, STV_NSLOTS };
enum { SC_GetState = 3, SC_GetDisplayName, SC_SetDisplayName, SC_GetIconPath, SC_SetIconPath, SC_GetGroupingParam,
       SC_SetGroupingParam, SC_RegisterAudioSessionNotification, SC_UnregisterAudioSessionNotification, SC_NSLOTS };
_Static_assert(EN_NSLOTS == 8 && COL_NSLOTS == 5 && DEV_NSLOTS == 7 && EP_NSLOTS == 4 && PS_NSLOTS == 8, "mmdeviceapi.h slot counts");
_Static_assert(AC_NSLOTS == 21 && RC_NSLOTS == 5 && CK_NSLOTS == 6 && SV_NSLOTS == 7 && STV_NSLOTS == 8 && SC_NSLOTS == 12, "audioclient.h slot counts");

/* object fields */
enum { O_CLIENT = 0, O_COUNT = 1, O_NFIELDS = 2 };      /* the client a service belongs to; a collection's count */
enum { TAG_MMDEV = 0x4D4D0000, TAG_MMCLIENT };
enum { MAX_CLIENT = 16, QLEN = 256 };

static const uint8_t CLSID_MMDeviceEnumerator_[16] = { 0x95,0x03,0xde,0xbc, 0x2f,0xe5, 0x7c,0x46, 0x8e,0x3d, 0xc4,0x57,0x92,0x91,0x69,0x2e };
static const uint8_t IID_IAudioClient_[16]        = { 0x4c,0xad,0xb9,0x1c, 0xfa,0xdb, 0x32,0x4c, 0xb1,0x78, 0xc2,0xf5,0x68,0xa7,0x03,0xb2 };
static const uint8_t IID_IAudioClient2_[16]       = { 0xcd,0x78,0x67,0x72, 0x0a,0xf6, 0xda,0x4e, 0x82,0xde, 0xe4,0x76,0x10,0xcd,0x78,0xaa };
static const uint8_t IID_IAudioClient3_[16]       = { 0x07,0xee,0xd4,0x7e, 0x67,0x8e, 0xd4,0x4c, 0x8c,0x1a, 0x2b,0x7a,0x59,0x87,0xad,0x42 };
static const uint8_t IID_IAudioRenderClient_[16]  = { 0xfc,0xac,0x94,0xf2, 0x46,0x31, 0x83,0x44, 0xa7,0xbf, 0xad,0xdc,0xa7,0xc2,0x60,0xe2 };
static const uint8_t IID_IAudioClock_[16]         = { 0x4f,0x31,0x63,0xcd, 0xba,0x3f, 0x1b,0x4a, 0x81,0x2c, 0xef,0x96,0x35,0x87,0x28,0xe7 };
static const uint8_t IID_ISimpleAudioVolume_[16]  = { 0x98,0x54,0xce,0x87, 0xd6,0x68, 0xe5,0x44, 0x92,0x15, 0x6d,0xa4,0x7e,0xf8,0x83,0xd8 };
static const uint8_t IID_IAudioStreamVolume_[16]  = { 0x87,0x48,0x01,0x93, 0x2d,0x24, 0x68,0x40, 0x8a,0x15, 0xcf,0x5e,0x93,0xb9,0x0f,0xe3 };
static const uint8_t IID_IAudioSessionControl_[16] = { 0x80,0xb3,0xb3,0xf4, 0xfa,0x6c, 0xe3,0x4e, 0xb2,0x27, 0x16,0xd3,0xa4,0xc9,0x46,0x10 }; /* f4b1a599-7266-4319-a8ca-e70acb11e8cd is Control2 */
static const uint8_t IID_IMMEndpoint_[16]         = { 0x88,0x97,0xe0,0x1b, 0x94,0x68, 0x89,0x40, 0x85,0x86, 0x9a,0x2a,0x6c,0x26,0x5a,0xc5 };

/* property keys: fmtid then pid */
static const uint8_t PKEY_Device_[16]             = { 0x4e,0x25,0x5c,0xa4, 0x1c,0xdf, 0xfd,0x4e, 0x80,0x20, 0x67,0xd1,0x46,0xa8,0x50,0xe0 }; /* pid 14 FriendlyName, 2 DeviceDesc */
static const uint8_t PKEY_DeviceInterface_[16]    = { 0x6e,0x51,0x6e,0x02, 0x14,0xb8, 0x4b,0x41, 0x83,0xcd, 0x85,0x6d,0x6f,0xef,0x48,0x22 }; /* pid 2 FriendlyName */
static const uint8_t PKEY_AudioEngine_[16]        = { 0x4d,0x06,0x9f,0xf1, 0x2c,0x08, 0x27,0x4e, 0xbc,0x73, 0x68,0x82,0xa1,0xbb,0x8e,0x4c }; /* pid 0 DeviceFormat */
static const uint8_t KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_[16] = { 3,0,0,0, 0,0, 0x10,0, 0x80,0, 0,0xaa,0,0x38,0x9b,0x71 };
static const char DEVICE_ID[] = "{0.0.0.00000000}.{4d4d0000-0000-4000-8000-77696e696f73}";
static const char DEVICE_NAME[] = "Winios Audio";

typedef struct { void *mem; uint32_t frames; } blk;
typedef struct {
    int      used, inited, started, event_mode;
    uint64_t obj;
    uint32_t freq, channels, bits; int is_float;
    uint32_t buffer_frames, period_frames;
    uint64_t staging;                /* guest memory GetBuffer hands out; buffer_frames long */
    uint32_t pending;                /* frames handed out and not yet released */
    uint64_t released;               /* frames committed since Initialize or Reset */
    uint64_t event;                  /* SetEventHandle */
    int      src, in_mixer;          /* mixer source and how many front blocks it holds */
    blk      q[QLEN]; int head, n;
    float    volume; int mute;
} client;
static client g_c[MAX_CLIENT];
static w32_com_class cls_enum, cls_col, cls_dev, cls_ep, cls_ps, cls_ac, cls_rc, cls_ck, cls_sv, cls_stv, cls_sc;
static w32_api enum_m[EN_NSLOTS], col_m[COL_NSLOTS], dev_m[DEV_NSLOTS], ep_m[EP_NSLOTS], ps_m[PS_NSLOTS], ac_m[AC_NSLOTS],
               rc_m[RC_NSLOTS], ck_m[CK_NSLOTS], sv_m[SV_NSLOTS], stv_m[STV_NSLOTS], sc_m[SC_NSLOTS];
static int g_built, g_built32;
static int g_streams;                /* clients that reached Initialize, for the report */

static uint32_t frame_bytes(const client *c) { return (c->channels ? c->channels : 1) * ((c->bits ? c->bits : 16) / 8); }
static int32_t lin_to_mB(float v) {
    if (v <= 0.0f) return -10000;
    if (v >= 1.0f) return 0;
    double mB = 2000.0 * log10((double)v);
    return mB < -10000 ? -10000 : (int32_t)mB;
}
static uint64_t now_hns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100; }
static int guid_is(w32 *w, uint64_t p, const uint8_t g[16]) {
    if (!p || !w32_mem_ok(w, p, 16)) return 0;
    const uint8_t *m = W32PN(w, p, 16);
    return m && memcmp(m, g, 16) == 0;
}
static void out_ptr(w32 *w, uint64_t at, uint64_t v) { if (at) w32_write(w, at, (int)w32_ptrsize(w), v); }
static void out_u32(w32 *w, uint64_t at, uint32_t v) { if (at && w32_mem_ok(w, at, 4)) w32_write(w, at, 4, v); }
static void out_u64(w32 *w, uint64_t at, uint64_t v) { if (at && w32_mem_ok(w, at, 8)) w32_write(w, at, 8, v); }
/* a REFERENCE_TIME argument: one slot on x64, two on x86; *i moves past it */
static uint64_t arg64(w32 *w, int *i) {
    uint64_t v;
    if (w32_ptrsize(w) == 8) { v = ARG(*i); *i += 1; }
    else { v = (uint32_t)ARG(*i) | (uint64_t)(uint32_t)ARG(*i + 1) << 32; *i += 2; }
    return v;
}
/* The mix format: WAVEFORMATEXTENSIBLE, 44.1 kHz stereo 32-bit float, which is
 * what Windows reports for its shared-mode engine and what a program that
 * takes the mix format as given will hand back to Initialize. */
static uint64_t mix_format(w32 *w) {
    uint64_t f = w32_heap_alloc(w, 40);
    if (!f) return 0;
    w32_write(w, f, 2, 0xFFFE); w32_write(w, f + 2, 2, 2); w32_write(w, f + 4, 4, 44100); w32_write(w, f + 8, 4, 44100 * 8);
    w32_write(w, f + 12, 2, 8); w32_write(w, f + 14, 2, 32); w32_write(w, f + 16, 2, 22);
    w32_write(w, f + 18, 2, 32); w32_write(w, f + 20, 4, 3);
    for (int i = 0; i < 16; i++) w32_write(w, f + 24 + (unsigned)i, 1, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_[i]);
    return f;
}
/* Read a WAVEFORMATEX the program hands over. 1 if the mixer can play it. */
static int parse_format(w32 *w, uint64_t fmt, uint32_t *tag, uint32_t *ch, uint32_t *rate, uint32_t *bits) {
    if (!fmt || !w32_mem_ok(w, fmt, 16)) return 0;
    *tag = (uint32_t)w32_read(w, fmt, 2); *ch = (uint32_t)w32_read(w, fmt + 2, 2);
    *rate = (uint32_t)w32_read(w, fmt + 4, 4); *bits = (uint32_t)w32_read(w, fmt + 14, 2);
    if (*tag == 0xFFFE && w32_mem_ok(w, fmt, 40)) *tag = (uint32_t)w32_read(w, fmt + 24, 2);
    if (*tag != 1 && *tag != 3) return 0;
    if (!*ch || *ch > 8 || *rate < 8000 || *rate > 192000) return 0;
    if (*tag == 3 && *bits != 32) return 0;
    if (*tag == 1 && *bits != 8 && *bits != 16 && *bits != 24 && *bits != 32) return 0;
    return 1;
}

/* --- the queue and the mixer ------------------------------------------------ */
static void src_fill(const client *c, const blk *b, w32_audio_src *s) {
    memset(s, 0, sizeof *s);
    s->mem = b->mem; s->owner = b->mem; s->size = b->frames * frame_bytes(c);
    s->freq = c->freq; s->channels = c->channels; s->bits = c->bits; s->is_float = c->is_float;
    s->playing = c->started;
    s->vol_mB = c->mute ? -10000 : lin_to_mB(c->volume);
}
static void refill(client *c) {
    while (c->in_mixer < 2 && c->in_mixer < c->n) {
        blk *b = &c->q[(c->head + c->in_mixer) % QLEN];
        w32_audio_src s; src_fill(c, b, &s);
        if (c->src < 0) { c->src = w32_audio_src_add(&s); if (c->src < 0) return; }
        else if (c->in_mixer == 0) w32_audio_src_set(c->src, &s);
        else if (!w32_audio_src_queue_next(c->src, &s)) return;
        c->in_mixer++;
    }
    if (c->started && c->src >= 0) w32_audio_open();
}
static void sync_gain(client *c) {
    if (c->src < 0 || c->in_mixer == 0) return;
    w32_audio_src s; src_fill(c, &c->q[c->head % QLEN], &s);
    s.owner = 0;                                        /* the mixer already holds this block */
    w32_audio_src_set(c->src, &s);
}
static uint64_t played(const client *c) { return c->src >= 0 ? w32_audio_src_frames_done(c->src) : 0; }
static uint32_t padding_of(const client *c) {
    uint64_t p = played(c);
    return c->released > p ? (uint32_t)(c->released - p) : 0;
}
/* Drop the whole queue: what Reset does, and what a released client leaves.
 * Blocks the mixer holds are its to free; the rest are ours. */
static void flush(client *c) {
    if (c->src >= 0) w32_audio_src_remove(c->src);
    c->src = -1;
    for (int i = c->in_mixer; i < c->n; i++) free(c->q[(c->head + i) % QLEN].mem);
    c->head = c->n = c->in_mixer = 0;
    c->released = 0; c->pending = 0;
}
static void client_tick(w32 *w, client *c) {
    if (c->src >= 0) {
        int ended = w32_audio_src_take_ended(c->src);
        while (ended-- > 0 && c->n > 0) { c->head++; c->n--; if (c->in_mixer > 0) c->in_mixer--; }
        refill(c);
    }
    /* Event mode: the program is woken whenever a period's worth of room is
     * free. Set, not pulsed -- an auto-reset event is consumed by the wait
     * that sees it, a manual-reset one the program resets itself. */
    if (c->event_mode && c->event && c->started && c->inited) {
        uint32_t pad = padding_of(c);
        if (pad + c->period_frames <= c->buffer_frames) w32_event_set(w, c->event);
    }
}
static int g_in_tick;
void w32_mmdevapi_tick(w32 *w) {
    if (!g_built || g_in_tick) return;
    g_in_tick = 1;
    w32_audio_pump();
    for (int i = 0; i < MAX_CLIENT && !w->exited; i++) if (g_c[i].used) client_tick(w, &g_c[i]);
    g_in_tick = 0;
}
int w32_mmdevapi_stream_count(void) { return g_streams; }

/* --- IAudioClient --------------------------------------------------------- */
static client *client_of(w32 *w, uint64_t obj) {
    int i = (int)w32_com_get(w, obj, O_CLIENT) - 1;
    return i >= 0 && i < MAX_CLIENT && g_c[i].used ? &g_c[i] : 0;
}
static void ac_Release(w32 *w) {
    /* IUnknown::Release; when the last reference goes the stream stops and
     * its blocks are freed, which is what a program that closes its audio
     * and opens it again expects to hear: silence, then the new stream. */
    uint64_t self = ARG(0);
    client *c = client_of(w, self);
    uint32_t r = (uint32_t)w32_read(w, self + w32_ptrsize(w), 4);
    if (r) r--;
    w32_write(w, self + w32_ptrsize(w), 4, r);
    if (!r && c) { flush(c); memset(c, 0, sizeof *c); w32_com_set(w, self, O_CLIENT, 0); }
    RET(r);
}
static void init_client(w32 *w, client *c, uint32_t flags, uint64_t dur_hns, uint64_t period_hns, uint64_t fmt) {
    uint32_t tag, ch, rate, bits;
    if (c->inited) { RET((uint64_t)(uint32_t)AUDCLNT_E_ALREADY_INITIALIZED_); return; }
    if (flags & AUDCLNT_STREAMFLAGS_LOOPBACK_) { RET((uint64_t)(uint32_t)AUDCLNT_E_WRONG_ENDPOINT_TYPE_); return; }
    if (!parse_format(w, fmt, &tag, &ch, &rate, &bits)) {
        static char note[96]; snprintf(note, sizeof note, "audioclient!Initialize (format tag %u, %u bits: only PCM and float are played here)", tag, bits);
        w32_note_refused(w, note);
        RET((uint64_t)(uint32_t)AUDCLNT_E_UNSUPPORTED_FORMAT_); return;
    }
    c->freq = rate; c->channels = ch; c->bits = bits; c->is_float = tag == 3;
    /* The period is 10 ms unless exclusive mode asked for another; the
     * buffer is what was asked for, and at least two periods so that a
     * program filling it one period at a time is never told it is full. */
    if (!period_hns) period_hns = 100000;
    c->period_frames = (uint32_t)((period_hns * rate + 9999999) / 10000000);
    if (c->period_frames < 32) c->period_frames = 32;
    c->buffer_frames = (uint32_t)((dur_hns * rate + 9999999) / 10000000);
    if (c->buffer_frames < 2 * c->period_frames) c->buffer_frames = 2 * c->period_frames;
    if (c->buffer_frames > rate * 8) c->buffer_frames = rate * 8;                 /* eight seconds is a mistake, not a request */
    c->staging = w32_alloc(w, (uint64_t)c->buffer_frames * frame_bytes(c), 0);
    if (!c->staging) { RET((uint64_t)(uint32_t)E_OUTOFMEMORY_); return; }
    c->event_mode = (flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK_) != 0;
    c->inited = 1; c->volume = 1.0f; c->src = -1;
    g_streams++;
    if (w->verbose) fprintf(stderr, "winrun: wasapi: stream %u Hz, %u ch, %u bit%s, buffer %u frames, period %u%s\n",
                            rate, ch, bits, c->is_float ? " float" : "", c->buffer_frames, c->period_frames, c->event_mode ? ", event driven" : "");
    RET(S_OK_);
}
/* Initialize(ShareMode, StreamFlags, hnsBufferDuration, hnsPeriodicity, pFormat, AudioSessionGuid) */
static void ac_Initialize(w32 *w) {
    client *c = client_of(w, ARG(0));
    if (!c) { RET((uint64_t)(uint32_t)E_FAIL_); return; }
    int i = 3;
    uint32_t flags = (uint32_t)ARG(2);
    uint64_t dur = arg64(w, &i), period = arg64(w, &i);
    uint64_t fmt = ARG(i);
    init_client(w, c, flags, dur, period, fmt);
}
/* IAudioClient3::InitializeSharedAudioStream(StreamFlags, PeriodInFrames, pFormat, AudioSessionGuid) */
static void ac3_InitializeSharedAudioStream(w32 *w) {
    client *c = client_of(w, ARG(0));
    if (!c) { RET((uint64_t)(uint32_t)E_FAIL_); return; }
    uint32_t tag, ch, rate, bits, pf = (uint32_t)ARG(2);
    if (!parse_format(w, ARG(3), &tag, &ch, &rate, &bits)) { RET((uint64_t)(uint32_t)AUDCLNT_E_UNSUPPORTED_FORMAT_); return; }
    uint64_t period = pf ? ((uint64_t)pf * 10000000 + rate - 1) / rate : 0;
    init_client(w, c, (uint32_t)ARG(1), period * 2, period, ARG(3));
}
#define NEED_INIT(c) do { if (!(c) || !(c)->inited) { RET((uint64_t)(uint32_t)((c) ? AUDCLNT_E_NOT_INITIALIZED_ : E_FAIL_)); return; } } while (0)
static void ac_GetBufferSize(w32 *w) { client *c = client_of(w, ARG(0)); NEED_INIT(c); out_u32(w, ARG(1), c->buffer_frames); RET(S_OK_); }
static void ac_GetStreamLatency(w32 *w) { client *c = client_of(w, ARG(0)); NEED_INIT(c); out_u64(w, ARG(1), (uint64_t)c->period_frames * 10000000 / c->freq); RET(S_OK_); }
static void ac_GetCurrentPadding(w32 *w) {
    client *c = client_of(w, ARG(0)); NEED_INIT(c);
    w32_mmdevapi_tick(w);
    out_u32(w, ARG(1), padding_of(c));
    RET(S_OK_);
}
/* IsFormatSupported(ShareMode, pFormat, ppClosestMatch) */
static void ac_IsFormatSupported(w32 *w) {
    uint32_t tag, ch, rate, bits, mode = (uint32_t)ARG(1);
    uint64_t closest = ARG(3);
    if (!ARG(2)) { RET((uint64_t)(uint32_t)E_POINTER_); return; }
    if (parse_format(w, ARG(2), &tag, &ch, &rate, &bits)) { if (mode == 0) out_ptr(w, closest, 0); RET(S_OK_); return; }
    if (mode == 0 && closest) { out_ptr(w, closest, mix_format(w)); RET(S_FALSE_); return; }
    RET((uint64_t)(uint32_t)AUDCLNT_E_UNSUPPORTED_FORMAT_);
}
static void ac_GetMixFormat(w32 *w) {
    if (!ARG(1)) { RET((uint64_t)(uint32_t)E_POINTER_); return; }
    uint64_t f = mix_format(w);
    out_ptr(w, ARG(1), f);
    RET(f ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void ac_GetDevicePeriod(w32 *w) { out_u64(w, ARG(1), 100000); out_u64(w, ARG(2), 30000); RET(S_OK_); }
static void ac_Start(w32 *w) {
    client *c = client_of(w, ARG(0)); NEED_INIT(c);
    if (c->started) { RET((uint64_t)(uint32_t)AUDCLNT_E_NOT_STOPPED_); return; }
    if (c->event_mode && !c->event) { RET((uint64_t)(uint32_t)AUDCLNT_E_EVENTHANDLE_NOT_SET_); return; }
    c->started = 1;
    sync_gain(c); refill(c);
    w32_audio_open();
    if (w->verbose) fprintf(stderr, "winrun: wasapi: stream started, %s\n", w32_audio_device_on() ? "device open" : "no device");
    RET(S_OK_);
}
static void ac_Stop(w32 *w) {
    client *c = client_of(w, ARG(0)); NEED_INIT(c);
    int was = c->started;
    c->started = 0; sync_gain(c);
    RET(was ? S_OK_ : S_FALSE_);
}
static void ac_Reset(w32 *w) {
    client *c = client_of(w, ARG(0)); NEED_INIT(c);
    if (c->started) { RET((uint64_t)(uint32_t)AUDCLNT_E_NOT_STOPPED_); return; }
    flush(c);
    RET(S_OK_);
}
static void ac_SetEventHandle(w32 *w) {
    client *c = client_of(w, ARG(0)); NEED_INIT(c);
    if (!c->event_mode) { RET((uint64_t)(uint32_t)AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED_); return; }
    if (!ARG(1)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    c->event = ARG(1);
    RET(S_OK_);
}
static uint64_t service_new(w32 *w, w32_com_class *cls, uint64_t owner) {
    uint64_t o = w32_com_new(w, cls, O_NFIELDS);
    if (o) w32_com_set(w, o, O_CLIENT, w32_com_get(w, owner, O_CLIENT));
    return o;
}
/* GetService(riid, out): the other interfaces of the stream, each its own
 * object with its own vtable and a pointer back to the client. */
static void ac_GetService(w32 *w) {
    client *c = client_of(w, ARG(0)); NEED_INIT(c);
    uint64_t iid = ARG(1), out = ARG(2);
    if (!out) { RET((uint64_t)(uint32_t)E_POINTER_); return; }
    w32_com_class *cls = 0;
    if (guid_is(w, iid, IID_IAudioRenderClient_)) cls = &cls_rc;
    else if (guid_is(w, iid, IID_IAudioClock_)) cls = &cls_ck;
    else if (guid_is(w, iid, IID_ISimpleAudioVolume_)) cls = &cls_sv;
    else if (guid_is(w, iid, IID_IAudioStreamVolume_)) cls = &cls_stv;
    else if (guid_is(w, iid, IID_IAudioSessionControl_)) cls = &cls_sc;
    if (!cls) {
        out_ptr(w, out, 0);
        w32_note_refused(w, "audioclient!GetService (an interface other than the render client, clock, volumes and session control)");
        RET((uint64_t)(uint32_t)E_NOINTERFACE_); return;
    }
    uint64_t o = service_new(w, cls, ARG(0));
    out_ptr(w, out, o);
    RET(o ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void ac2_IsOffloadCapable(w32 *w) { out_u32(w, ARG(2), 0); RET(S_OK_); }
static void ac2_SetClientProperties(w32 *w) { (void)w; RET(S_OK_); }
static void ac2_GetBufferSizeLimits(w32 *w) { out_u64(w, ARG(3), 100000); out_u64(w, ARG(4), 20000000); RET(S_OK_); }
/* IAudioClient3: the engine period in frames of the format given. 10 ms at
 * the format's rate, adjustable down to 3 ms and up to 100. */
static void ac3_GetSharedModeEnginePeriod(w32 *w) {
    uint32_t tag, ch, rate = 44100, bits;
    if (ARG(1) && !parse_format(w, ARG(1), &tag, &ch, &rate, &bits)) { RET((uint64_t)(uint32_t)AUDCLNT_E_UNSUPPORTED_FORMAT_); return; }
    out_u32(w, ARG(2), rate / 100); out_u32(w, ARG(3), 1); out_u32(w, ARG(4), rate * 3 / 1000); out_u32(w, ARG(5), rate / 10);
    RET(S_OK_);
}
static void ac3_GetCurrentSharedModeEnginePeriod(w32 *w) {
    out_ptr(w, ARG(1), mix_format(w)); out_u32(w, ARG(2), 441);
    RET(S_OK_);
}

/* --- IAudioRenderClient --------------------------------------------------- */
/* GetBuffer(NumFramesRequested, ppData): room to write, in the staging area.
 * A program may hold one buffer at a time, and never more than the space
 * the padding leaves. */
static void rc_GetBuffer(w32 *w) {
    client *c = client_of(w, ARG(0)); NEED_INIT(c);
    uint32_t n = (uint32_t)ARG(1);
    uint64_t out = ARG(2);
    if (!out) { RET((uint64_t)(uint32_t)E_POINTER_); return; }
    if (c->pending) { RET((uint64_t)(uint32_t)AUDCLNT_E_OUT_OF_ORDER_); return; }
    w32_mmdevapi_tick(w);
    if (n > c->buffer_frames - padding_of(c) || n > c->buffer_frames) { out_ptr(w, out, 0); RET((uint64_t)(uint32_t)AUDCLNT_E_BUFFER_TOO_LARGE_); return; }
    c->pending = n;
    out_ptr(w, out, n ? c->staging : 0);
    RET(S_OK_);
}
/* ReleaseBuffer(NumFramesWritten, Flags): the frames become a block behind
 * whatever is playing. SILENT means the program did not bother writing
 * them and wants zeros. */
static void rc_ReleaseBuffer(w32 *w) {
    client *c = client_of(w, ARG(0)); NEED_INIT(c);
    uint32_t n = (uint32_t)ARG(1), flags = (uint32_t)ARG(2);
    if (n > c->pending) { RET((uint64_t)(uint32_t)AUDCLNT_E_INVALID_SIZE_); return; }
    if (!n) { c->pending = 0; RET(S_OK_); return; }
    if (c->n >= QLEN) { RET((uint64_t)(uint32_t)AUDCLNT_E_BUFFER_ERROR_); return; }
    uint32_t bytes = n * frame_bytes(c);
    uint8_t *mem = malloc(bytes);
    if (!mem) { RET((uint64_t)(uint32_t)E_OUTOFMEMORY_); return; }
    const void *src = W32PN(w, c->staging, bytes);
    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT_) || !src) memset(mem, c->bits == 8 ? 0x80 : 0, bytes);
    else memcpy(mem, src, bytes);
    blk *b = &c->q[(c->head + c->n) % QLEN];
    b->mem = mem; b->frames = n;
    c->n++;
    c->released += n; c->pending = 0;
    refill(c);
    RET(S_OK_);
}

/* --- IAudioClock, the volumes, the session ---------------------------------- */
static void ck_GetFrequency(w32 *w) { client *c = client_of(w, ARG(0)); NEED_INIT(c); out_u64(w, ARG(1), c->freq); RET(S_OK_); }
static void ck_GetPosition(w32 *w) {
    client *c = client_of(w, ARG(0)); NEED_INIT(c);
    w32_mmdevapi_tick(w);
    out_u64(w, ARG(1), played(c));
    out_u64(w, ARG(2), now_hns());
    RET(S_OK_);
}
static void ck_GetCharacteristics(w32 *w) { out_u32(w, ARG(1), 0); RET(S_OK_); }
static void sv_SetMasterVolume(w32 *w) {
    client *c = client_of(w, ARG(0)); NEED_INIT(c);
    float f = w32_fargf(w, 1);
    if (f < 0.0f || f > 1.0f) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    c->volume = f; sync_gain(c);
    RET(S_OK_);
}
static void sv_GetMasterVolume(w32 *w) {
    client *c = client_of(w, ARG(0)); NEED_INIT(c);
    uint32_t bits; memcpy(&bits, &c->volume, 4);
    out_u32(w, ARG(1), bits);
    RET(S_OK_);
}
static void sv_SetMute(w32 *w) { client *c = client_of(w, ARG(0)); NEED_INIT(c); c->mute = ARG(1) != 0; sync_gain(c); RET(S_OK_); }
static void sv_GetMute(w32 *w) { client *c = client_of(w, ARG(0)); NEED_INIT(c); out_u32(w, ARG(1), (uint32_t)c->mute); RET(S_OK_); }
static void stv_GetChannelCount(w32 *w) { client *c = client_of(w, ARG(0)); NEED_INIT(c); out_u32(w, ARG(1), c->channels); RET(S_OK_); }
static void stv_SetChannelVolume(w32 *w) {
    /* one gain for the stream: the channels move together */
    client *c = client_of(w, ARG(0)); NEED_INIT(c);
    if ((uint32_t)ARG(1) >= c->channels) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    float f = w32_fargf(w, 2);
    if (f < 0.0f || f > 1.0f) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    c->volume = f; sync_gain(c);
    RET(S_OK_);
}
static void stv_GetChannelVolume(w32 *w) {
    client *c = client_of(w, ARG(0)); NEED_INIT(c);
    if ((uint32_t)ARG(1) >= c->channels) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    uint32_t bits; memcpy(&bits, &c->volume, 4);
    out_u32(w, ARG(2), bits);
    RET(S_OK_);
}
static void stv_SetAllVolumes(w32 *w) {
    client *c = client_of(w, ARG(0)); NEED_INIT(c);
    uint32_t n = (uint32_t)ARG(1);
    if (n != c->channels || !ARG(2) || !w32_mem_ok(w, ARG(2), 4ull * n)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    uint32_t bits = (uint32_t)w32_read(w, ARG(2), 4); float f; memcpy(&f, &bits, 4);
    if (f < 0.0f || f > 1.0f) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    c->volume = f; sync_gain(c);
    RET(S_OK_);
}
static void stv_GetAllVolumes(w32 *w) {
    client *c = client_of(w, ARG(0)); NEED_INIT(c);
    uint32_t n = (uint32_t)ARG(1);
    if (n != c->channels || !ARG(2) || !w32_mem_ok(w, ARG(2), 4ull * n)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    uint32_t bits; memcpy(&bits, &c->volume, 4);
    for (uint32_t i = 0; i < n; i++) w32_write(w, ARG(2) + 4ull * i, 4, bits);
    RET(S_OK_);
}
/* the session: active while the stream runs, named nothing, grouped alone */
static void sc_GetState(w32 *w) { client *c = client_of(w, ARG(0)); out_u32(w, ARG(1), c && c->started ? 1 : 0); RET(S_OK_); }
static void sc_GetString(w32 *w) { out_ptr(w, ARG(1), w32_wstrdup(w, "")); RET(S_OK_); }
static void sc_SetString(w32 *w) { (void)w; RET(S_OK_); }
static void sc_GetGroupingParam(w32 *w) { if (ARG(1) && w32_mem_ok(w, ARG(1), 16)) for (int i = 0; i < 16; i++) w32_write(w, ARG(1) + (unsigned)i, 1, 0); RET(S_OK_); }
static void sc_ok(w32 *w) { (void)w; RET(S_OK_); }

/* --- IMMDeviceEnumerator, IMMDevice, the collection and the property store --- */
static uint64_t device_new(w32 *w) { return w32_com_new(w, &cls_dev, O_NFIELDS); }
static void en_EnumAudioEndpoints(w32 *w) {
    /* (dataFlow, stateMask, out): one render device when asked for render
     * or all, and it is active */
    uint32_t flow = (uint32_t)ARG(1), mask = (uint32_t)ARG(2);
    if (!ARG(3)) { RET((uint64_t)(uint32_t)E_POINTER_); return; }
    uint64_t col = w32_com_new(w, &cls_col, O_NFIELDS);
    if (!col) { RET((uint64_t)(uint32_t)E_OUTOFMEMORY_); return; }
    w32_com_set(w, col, O_COUNT, (flow == eRender || flow == eAll) && (mask & DEVICE_STATE_ACTIVE_) ? 1 : 0);
    out_ptr(w, ARG(3), col);
    RET(S_OK_);
}
static void en_GetDefaultAudioEndpoint(w32 *w) {
    /* (dataFlow, role, out) */
    uint32_t flow = (uint32_t)ARG(1);
    if (!ARG(3)) { RET((uint64_t)(uint32_t)E_POINTER_); return; }
    if (flow != eRender) { out_ptr(w, ARG(3), 0); RET((uint64_t)(uint32_t)E_NOTFOUND_); return; }
    uint64_t d = device_new(w);
    out_ptr(w, ARG(3), d);
    RET(d ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void en_GetDevice(w32 *w) {
    /* (id, out): the one id there is */
    char id[128];
    if (!ARG(2)) { RET((uint64_t)(uint32_t)E_POINTER_); return; }
    if (!ARG(1)) { RET((uint64_t)(uint32_t)E_POINTER_); return; }
    w32_wtoa(w, ARG(1), id, sizeof id);
    if (strcmp(id, DEVICE_ID) != 0) { out_ptr(w, ARG(2), 0); RET((uint64_t)(uint32_t)E_NOTFOUND_); return; }
    uint64_t d = device_new(w);
    out_ptr(w, ARG(2), d);
    RET(d ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void en_ok(w32 *w) { (void)w; RET(S_OK_); }
static void col_GetCount(w32 *w) { out_u32(w, ARG(1), (uint32_t)w32_com_get(w, ARG(0), O_COUNT)); RET(S_OK_); }
static void col_Item(w32 *w) {
    if (!ARG(2)) { RET((uint64_t)(uint32_t)E_POINTER_); return; }
    if ((uint32_t)ARG(1) >= (uint32_t)w32_com_get(w, ARG(0), O_COUNT)) { out_ptr(w, ARG(2), 0); RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    uint64_t d = device_new(w);
    out_ptr(w, ARG(2), d);
    RET(d ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
/* QueryInterface on the device: IMMEndpoint has a different vtable. */
static void dev_QueryInterface(w32 *w) {
    uint64_t self = ARG(0), iid = ARG(1), out = ARG(2);
    if (!out) { RET((uint64_t)(uint32_t)E_POINTER_); return; }
    if (guid_is(w, iid, IID_IMMEndpoint_)) {
        uint64_t ep = w32_com_new(w, &cls_ep, O_NFIELDS);
        out_ptr(w, out, ep);
        RET(ep ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_); return;
    }
    out_ptr(w, out, self);
    w32_com_AddRef(w);
    RET(S_OK_);
}
/* Activate(iid, clsctx, params, out): the stream, in any of its three
 * revisions -- one vtable serves all, the later ones extend the earlier. */
static void dev_Activate(w32 *w) {
    uint64_t iid = ARG(1), out = ARG(4);
    if (!out) { RET((uint64_t)(uint32_t)E_POINTER_); return; }
    if (!(guid_is(w, iid, IID_IAudioClient_) || guid_is(w, iid, IID_IAudioClient2_) || guid_is(w, iid, IID_IAudioClient3_))) {
        out_ptr(w, out, 0);
        w32_note_refused(w, "mmdevapi!IMMDevice::Activate (an interface other than IAudioClient: endpoint volume, session manager, spatial audio)");
        RET((uint64_t)(uint32_t)E_NOINTERFACE_); return;
    }
    int slot = -1;
    for (int i = 0; i < MAX_CLIENT; i++) if (!g_c[i].used) { slot = i; break; }
    if (slot < 0) { RET((uint64_t)(uint32_t)E_OUTOFMEMORY_); return; }
    uint64_t o = w32_com_new(w, &cls_ac, O_NFIELDS);
    if (!o) { RET((uint64_t)(uint32_t)E_OUTOFMEMORY_); return; }
    client *c = &g_c[slot];
    memset(c, 0, sizeof *c);
    c->used = 1; c->obj = o; c->src = -1; c->volume = 1.0f;
    w32_com_set(w, o, O_CLIENT, (uint64_t)slot + 1);
    out_ptr(w, out, o);
    RET(S_OK_);
}
static void dev_OpenPropertyStore(w32 *w) {
    if (!ARG(2)) { RET((uint64_t)(uint32_t)E_POINTER_); return; }
    uint64_t ps = w32_com_new(w, &cls_ps, O_NFIELDS);
    out_ptr(w, ARG(2), ps);
    RET(ps ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void dev_GetId(w32 *w) {
    if (!ARG(1)) { RET((uint64_t)(uint32_t)E_POINTER_); return; }
    uint64_t s = w32_wstrdup(w, DEVICE_ID);              /* the caller frees it with CoTaskMemFree, which is the same heap */
    out_ptr(w, ARG(1), s);
    RET(s ? S_OK_ : (uint64_t)(uint32_t)E_OUTOFMEMORY_);
}
static void dev_GetState(w32 *w) { out_u32(w, ARG(1), DEVICE_STATE_ACTIVE_); RET(S_OK_); }
static void ep_GetDataFlow(w32 *w) { out_u32(w, ARG(1), eRender); RET(S_OK_); }
/* The property store: the device's names, and the engine's format. */
static void write_key(w32 *w, uint64_t at, const uint8_t fmtid[16], uint32_t pid) {
    if (!at || !w32_mem_ok(w, at, 20)) return;
    for (int i = 0; i < 16; i++) w32_write(w, at + (unsigned)i, 1, fmtid[i]);
    w32_write(w, at + 16, 4, pid);
}
static void ps_GetCount(w32 *w) { out_u32(w, ARG(1), 4); RET(S_OK_); }
static void ps_GetAt(w32 *w) {
    switch ((uint32_t)ARG(1)) {
    case 0: write_key(w, ARG(2), PKEY_Device_, 14); break;
    case 1: write_key(w, ARG(2), PKEY_Device_, 2); break;
    case 2: write_key(w, ARG(2), PKEY_DeviceInterface_, 2); break;
    case 3: write_key(w, ARG(2), PKEY_AudioEngine_, 0); break;
    default: RET((uint64_t)(uint32_t)E_INVALIDARG_); return;
    }
    RET(S_OK_);
}
static void ps_GetValue(w32 *w) {
    /* (key*, PROPVARIANT*): vt at 0, the value at 8 */
    uint64_t key = ARG(1), pv = ARG(2);
    int psz = (int)w32_ptrsize(w);
    if (!pv || !w32_mem_ok(w, pv, 8 + 2 * (unsigned)psz) || !key || !w32_mem_ok(w, key, 20)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    for (int i = 0; i < 8 + 2 * psz; i++) w32_write(w, pv + (unsigned)i, 1, 0);
    const uint8_t *fmtid = W32PN(w, key, 16);
    uint32_t pid = (uint32_t)w32_read(w, key + 16, 4);
    if (fmtid && ((!memcmp(fmtid, PKEY_Device_, 16) && (pid == 14 || pid == 2)) || (!memcmp(fmtid, PKEY_DeviceInterface_, 16) && pid == 2))) {
        w32_write(w, pv, 2, VT_LPWSTR_);
        w32_write(w, pv + 8, psz, w32_wstrdup(w, DEVICE_NAME));
    } else if (fmtid && !memcmp(fmtid, PKEY_AudioEngine_, 16) && pid == 0) {
        w32_write(w, pv, 2, VT_BLOB_);
        w32_write(w, pv + 8, 4, 40);
        w32_write(w, pv + 8 + (unsigned)psz, psz, mix_format(w));   /* BLOB { cbSize; pBlobData } -- the pointer is aligned to its size */
    }
    RET(S_OK_);                                            /* VT_EMPTY for the rest: "no such property", not an error */
}
static void ps_SetValue(w32 *w) { (void)w; RET((uint64_t)(uint32_t)0x80070005); }   /* E_ACCESSDENIED: the store was opened for reading */
static void ps_Commit(w32 *w) { (void)w; RET(S_OK_); }

/* --- the tables -------------------------------------------------------------------- */
#define M(tab, slot, nm, na, f) do { (tab)[slot].name = (nm); (tab)[slot].nargs = (na); (tab)[slot].fn = (f); } while (0)
static void iunknown(w32_api *t, void (*release)(w32 *), void (*qi)(w32 *)) {
    M(t, 0, "QueryInterface", 3, qi ? qi : w32_com_QueryInterface);
    M(t, 1, "AddRef", 1, w32_com_AddRef);
    M(t, 2, "Release", 1, release ? release : w32_com_Release);
}
static void build_tables(int is32) {
    if (g_built && g_built32 == is32) return;
    memset(enum_m, 0, sizeof enum_m); memset(col_m, 0, sizeof col_m); memset(dev_m, 0, sizeof dev_m); memset(ep_m, 0, sizeof ep_m);
    memset(ps_m, 0, sizeof ps_m); memset(ac_m, 0, sizeof ac_m); memset(rc_m, 0, sizeof rc_m); memset(ck_m, 0, sizeof ck_m);
    memset(sv_m, 0, sizeof sv_m); memset(stv_m, 0, sizeof stv_m); memset(sc_m, 0, sizeof sc_m);
    iunknown(enum_m, 0, 0);
    M(enum_m, EN_EnumAudioEndpoints, "EnumAudioEndpoints", 4, en_EnumAudioEndpoints);
    M(enum_m, EN_GetDefaultAudioEndpoint, "GetDefaultAudioEndpoint", 4, en_GetDefaultAudioEndpoint);
    M(enum_m, EN_GetDevice, "GetDevice", 3, en_GetDevice);
    M(enum_m, EN_RegisterEndpointNotificationCallback, "RegisterEndpointNotificationCallback", 2, en_ok);
    M(enum_m, EN_UnregisterEndpointNotificationCallback, "UnregisterEndpointNotificationCallback", 2, en_ok);
    iunknown(col_m, 0, 0);
    M(col_m, COL_GetCount, "GetCount", 2, col_GetCount);
    M(col_m, COL_Item, "Item", 3, col_Item);
    iunknown(dev_m, 0, dev_QueryInterface);
    M(dev_m, DEV_Activate, "Activate", 5, dev_Activate);
    M(dev_m, DEV_OpenPropertyStore, "OpenPropertyStore", 3, dev_OpenPropertyStore);
    M(dev_m, DEV_GetId, "GetId", 2, dev_GetId);
    M(dev_m, DEV_GetState, "GetState", 2, dev_GetState);
    iunknown(ep_m, 0, 0);
    M(ep_m, EP_GetDataFlow, "GetDataFlow", 2, ep_GetDataFlow);
    iunknown(ps_m, 0, 0);
    M(ps_m, PS_GetCount, "GetCount", 2, ps_GetCount);
    M(ps_m, PS_GetAt, "GetAt", 3, ps_GetAt);
    M(ps_m, PS_GetValue, "GetValue", 3, ps_GetValue);
    M(ps_m, PS_SetValue, "SetValue", 3, ps_SetValue);
    M(ps_m, PS_Commit, "Commit", 1, ps_Commit);
    iunknown(ac_m, ac_Release, 0);
    M(ac_m, AC_Initialize, "Initialize", is32 ? 9 : 7, ac_Initialize);
    M(ac_m, AC_GetBufferSize, "GetBufferSize", 2, ac_GetBufferSize);
    M(ac_m, AC_GetStreamLatency, "GetStreamLatency", 2, ac_GetStreamLatency);
    M(ac_m, AC_GetCurrentPadding, "GetCurrentPadding", 2, ac_GetCurrentPadding);
    M(ac_m, AC_IsFormatSupported, "IsFormatSupported", 4, ac_IsFormatSupported);
    M(ac_m, AC_GetMixFormat, "GetMixFormat", 2, ac_GetMixFormat);
    M(ac_m, AC_GetDevicePeriod, "GetDevicePeriod", 3, ac_GetDevicePeriod);
    M(ac_m, AC_Start, "Start", 1, ac_Start);
    M(ac_m, AC_Stop, "Stop", 1, ac_Stop);
    M(ac_m, AC_Reset, "Reset", 1, ac_Reset);
    M(ac_m, AC_SetEventHandle, "SetEventHandle", 2, ac_SetEventHandle);
    M(ac_m, AC_GetService, "GetService", 3, ac_GetService);
    M(ac_m, AC2_IsOffloadCapable, "IsOffloadCapable", 3, ac2_IsOffloadCapable);
    M(ac_m, AC2_SetClientProperties, "SetClientProperties", 2, ac2_SetClientProperties);
    M(ac_m, AC2_GetBufferSizeLimits, "GetBufferSizeLimits", 5, ac2_GetBufferSizeLimits);
    M(ac_m, AC3_GetSharedModeEnginePeriod, "GetSharedModeEnginePeriod", 6, ac3_GetSharedModeEnginePeriod);
    M(ac_m, AC3_GetCurrentSharedModeEnginePeriod, "GetCurrentSharedModeEnginePeriod", 3, ac3_GetCurrentSharedModeEnginePeriod);
    M(ac_m, AC3_InitializeSharedAudioStream, "InitializeSharedAudioStream", 5, ac3_InitializeSharedAudioStream);
    iunknown(rc_m, 0, 0);
    M(rc_m, RC_GetBuffer, "GetBuffer", 3, rc_GetBuffer);
    M(rc_m, RC_ReleaseBuffer, "ReleaseBuffer", 3, rc_ReleaseBuffer);
    iunknown(ck_m, 0, 0);
    M(ck_m, CK_GetFrequency, "GetFrequency", 2, ck_GetFrequency);
    M(ck_m, CK_GetPosition, "GetPosition", 3, ck_GetPosition);
    M(ck_m, CK_GetCharacteristics, "GetCharacteristics", 2, ck_GetCharacteristics);
    iunknown(sv_m, 0, 0);
    M(sv_m, SV_SetMasterVolume, "SetMasterVolume", 3, sv_SetMasterVolume);
    M(sv_m, SV_GetMasterVolume, "GetMasterVolume", 2, sv_GetMasterVolume);
    M(sv_m, SV_SetMute, "SetMute", 3, sv_SetMute);
    M(sv_m, SV_GetMute, "GetMute", 2, sv_GetMute);
    iunknown(stv_m, 0, 0);
    M(stv_m, STV_GetChannelCount, "GetChannelCount", 2, stv_GetChannelCount);
    M(stv_m, STV_SetChannelVolume, "SetChannelVolume", 3, stv_SetChannelVolume);
    M(stv_m, STV_GetChannelVolume, "GetChannelVolume", 3, stv_GetChannelVolume);
    M(stv_m, STV_SetAllVolumes, "SetAllVolumes", 3, stv_SetAllVolumes);
    M(stv_m, STV_GetAllVolumes, "GetAllVolumes", 3, stv_GetAllVolumes);
    iunknown(sc_m, 0, 0);
    M(sc_m, SC_GetState, "GetState", 2, sc_GetState);
    M(sc_m, SC_GetDisplayName, "GetDisplayName", 2, sc_GetString);
    M(sc_m, SC_SetDisplayName, "SetDisplayName", 3, sc_SetString);
    M(sc_m, SC_GetIconPath, "GetIconPath", 2, sc_GetString);
    M(sc_m, SC_SetIconPath, "SetIconPath", 3, sc_SetString);
    M(sc_m, SC_GetGroupingParam, "GetGroupingParam", 2, sc_GetGroupingParam);
    M(sc_m, SC_SetGroupingParam, "SetGroupingParam", 3, sc_ok);
    M(sc_m, SC_RegisterAudioSessionNotification, "RegisterAudioSessionNotification", 2, sc_ok);
    M(sc_m, SC_UnregisterAudioSessionNotification, "UnregisterAudioSessionNotification", 2, sc_ok);
    #define CLS(c, nm, tab, tg) do { (c).name = (nm); (c).methods = (tab); (c).nmethods = (int)(sizeof (tab) / sizeof (tab)[0]); (c).tag = (tg); (c).vtable = 0; } while (0)
    CLS(cls_enum, "IMMDeviceEnumerator", enum_m, TAG_MMDEV); CLS(cls_col, "IMMDeviceCollection", col_m, TAG_MMDEV);
    CLS(cls_dev, "IMMDevice", dev_m, TAG_MMDEV); CLS(cls_ep, "IMMEndpoint", ep_m, TAG_MMDEV); CLS(cls_ps, "IPropertyStore", ps_m, TAG_MMDEV);
    CLS(cls_ac, "IAudioClient", ac_m, TAG_MMCLIENT); CLS(cls_rc, "IAudioRenderClient", rc_m, TAG_MMCLIENT); CLS(cls_ck, "IAudioClock", ck_m, TAG_MMCLIENT);
    CLS(cls_sv, "ISimpleAudioVolume", sv_m, TAG_MMCLIENT); CLS(cls_stv, "IAudioStreamVolume", stv_m, TAG_MMCLIENT); CLS(cls_sc, "IAudioSessionControl", sc_m, TAG_MMCLIENT);
    #undef CLS
    g_built = 1; g_built32 = is32;
}

int w32_mmdevapi_create_class(w32 *w, const uint8_t clsid[16], const uint8_t iid[16], uint64_t out) {
    (void)iid;
    if (memcmp(clsid, CLSID_MMDeviceEnumerator_, 16)) return 0;
    build_tables(w->is32);
    uint64_t e = w32_com_new(w, &cls_enum, O_NFIELDS);
    if (!e) return 0;
    w32_write(w, out, (int)w32_ptrsize(w), e);
    if (w->verbose) fprintf(stderr, "winrun: wasapi: device enumerator created\n");
    return 1;
}
void w32_mmdevapi_reset(void) {
    for (int i = 0; i < MAX_CLIENT; i++) if (g_c[i].used) {
        /* the mixer's sources are already gone (w32_audio_close ran first); the queued blocks are ours */
        for (int k = g_c[i].in_mixer; k < g_c[i].n; k++) free(g_c[i].q[(g_c[i].head + k) % QLEN].mem);
    }
    memset(g_c, 0, sizeof g_c);
    g_built = 0; g_streams = 0; g_in_tick = 0;
    cls_enum.vtable = cls_col.vtable = cls_dev.vtable = cls_ep.vtable = cls_ps.vtable = cls_ac.vtable = 0;
    cls_rc.vtable = cls_ck.vtable = cls_sv.vtable = cls_stv.vtable = cls_sc.vtable = 0;
}

/* mmdevapi.dll has no plain exports a game calls -- everything is reached
 * through CoCreateInstance -- but LoadLibrary("mmdevapi.dll") must succeed. */
const w32_api w32_mmdevapi[] = {
    { 0, 0, 0, 0, 0 },
};
