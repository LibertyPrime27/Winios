/* Host audio output: the one place guest sound reaches a speaker.
 *
 * DirectSound, PlaySound and XAudio2 all arrive here as *sources*: a block of
 * PCM (usually guest memory, read in place), a format, a play state, a volume
 * and a pan. A mixer sums whatever is playing into 44.1 kHz stereo 16-bit
 * frames as the device asks for them. The device is Apple's AudioQueue on iOS
 * and macOS, and nothing anywhere else -- where sources are still tracked, so
 * the guest-visible state is identical and only the speaker is missing.
 *
 * Integer arithmetic throughout the mix, as everywhere else in this project: a
 * rate is a 16.16 step per output frame, a volume is a Q15 gain fixed when it
 * is set, so the same sources give the same samples on every machine -- which
 * is what lets test_audio.c check the output to the sample. (A 32-bit float
 * source is converted with float arithmetic; there is no other way to read
 * one, and it is the only float here.)
 *
 * The one thing the mixer takes from a clock is where DirectSound says its
 * cursor is. The guest paces itself by GetCurrentPosition, which is
 * CLOCK_MONOTONIC arithmetic, and the device runs on its own crystal; when the
 * mixer's read position drifts more than a tenth of a second from the cursor
 * it jumps to it. Within that the difference is inaudible; beyond it, a game
 * would be writing where the mixer is not reading.
 *
 * Locking: the device thread and the guest thread both touch the source
 * table, under one mutex held only for the table walk. Guest memory itself is
 * read without a lock, as real hardware reads a buffer a program is writing:
 * a torn sample is a click, not a fault -- provided the memory is still
 * mapped, which is why w32_audio_close() runs before anything unmaps it.
 */
#define _GNU_SOURCE
#include "w32.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { OUT_RATE = 44100, MAX_SRC = 64, MAX_FRAMES = 4096 };

typedef struct {
    w32_audio_src s;
    uint64_t pos_fp;             /* read position in source frames, 16.16 */
    int32_t  gl, gr;             /* Q15 gains, left and right */
    int      used;
    w32_audio_src next;          /* the block behind this one, if has_next */
    int      has_next;
    int      ended;              /* blocks finished since the owner last asked */
    uint64_t frames_done;        /* source frames consumed, 16.16 */
} slot;

static slot g_src[MAX_SRC];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_device_on;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* DirectSound volume: hundredths of a decibel, 0 full, -10000 silent. pow()
 * once per SetVolume, never per sample. */
static int32_t gain_q15(int32_t mB) {
    if (mB >= 0) return 32768;
    if (mB <= -10000) return 0;
    return (int32_t)(32768.0 * pow(10.0, mB / 2000.0) + 0.5);
}
static void set_gains(slot *t) {
    int32_t v = gain_q15(t->s.vol_mB);
    int32_t pl = t->s.pan_mB > 0 ? gain_q15(-t->s.pan_mB) : 32768;    /* pan right: left fades */
    int32_t pr = t->s.pan_mB < 0 ? gain_q15(t->s.pan_mB) : 32768;
    t->gl = (int32_t)(((int64_t)v * pl) >> 15);
    t->gr = (int32_t)(((int64_t)v * pr) >> 15);
}
static uint32_t frame_bytes(const w32_audio_src *s) {
    uint32_t ch = s->channels ? s->channels : 1, by = s->bits ? s->bits / 8 : 2;
    return ch * by;
}

int w32_audio_src_add(const w32_audio_src *s) {
    pthread_mutex_lock(&g_lock);
    int id = -1;
    for (int i = 0; i < MAX_SRC; i++) if (!g_src[i].used) { id = i; break; }
    if (id >= 0) {
        slot *t = &g_src[id];
        memset(t, 0, sizeof *t);
        t->s = *s; t->used = 1;
        uint32_t fb = frame_bytes(&t->s);
        t->pos_fp = (uint64_t)(fb ? s->start_byte / fb : 0) << 16;
        set_gains(t);
    }
    pthread_mutex_unlock(&g_lock);
    return id;
}

/* Refresh a source. The read position survives unless play starts or the
 * start point moved -- a volume change mid-note must not restart the note. */
void w32_audio_src_set(int id, const w32_audio_src *s) {
    if (id < 0 || id >= MAX_SRC) return;
    pthread_mutex_lock(&g_lock);
    slot *t = &g_src[id];
    if (t->used) {
        int restart = (!t->s.playing && s->playing) || t->s.start_byte != s->start_byte
                      || t->s.mem != s->mem || t->s.size != s->size;
        void *owner = t->s.owner;
        t->s = *s;
        if (!t->s.owner) t->s.owner = owner;
        if (t->has_next) { t->next.playing = s->playing; t->next.vol_mB = s->vol_mB; t->next.pan_mB = s->pan_mB; t->next.freq = t->next.freq ? t->next.freq : s->freq; }
        if (restart) { uint32_t fb = frame_bytes(&t->s); t->pos_fp = (uint64_t)(fb ? s->start_byte / fb : 0) << 16; }
        set_gains(t);
    }
    pthread_mutex_unlock(&g_lock);
}

void w32_audio_src_remove(int id) {
    if (id < 0 || id >= MAX_SRC) return;
    pthread_mutex_lock(&g_lock);
    if (g_src[id].used) { free(g_src[id].s.owner); if (g_src[id].has_next) free(g_src[id].next.owner); memset(&g_src[id], 0, sizeof g_src[id]); }
    pthread_mutex_unlock(&g_lock);
}

int w32_audio_src_playing(int id) {
    if (id < 0 || id >= MAX_SRC) return 0;
    pthread_mutex_lock(&g_lock);
    int p = g_src[id].used && g_src[id].s.playing;
    pthread_mutex_unlock(&g_lock);
    return p;
}

/* One sample of one channel, as a signed 16-bit value. 8-bit WAV is unsigned;
 * 24 and 32-bit integer keep their top 16 bits; 32-bit float is clamped. */
static int32_t sample_at(const w32_audio_src *s, uint32_t frame, int ch) {
    uint32_t by = s->bits ? s->bits / 8 : 2, nch = s->channels ? s->channels : 1;
    if (!by) return 0;
    uint32_t idx = frame * nch * by + (nch > 1 ? (uint32_t)ch : 0) * by;
    if (idx + by > s->size) return 0;
    const uint8_t *p = (const uint8_t *)s->mem + idx;
    if (s->is_float && by == 4) {
        float f; memcpy(&f, p, 4);
        if (f > 1.0f) f = 1.0f; if (f < -1.0f) f = -1.0f;
        return (int32_t)(f * 32767.0f);
    }
    switch (by) {
    case 1:  return ((int32_t)p[0] - 128) << 8;
    case 2:  return (int16_t)(p[0] | p[1] << 8);
    case 3:  return (int16_t)(p[1] | p[2] << 8);
    default: return (int16_t)(p[by - 2] | p[by - 1] << 8);
    }
}

/* Where DirectSound's own clock says this source's cursor is, in frames --
 * the position the guest is writing ahead of. */
static uint32_t clock_frame(const w32_audio_src *s, uint32_t total_frames, int *ended) {
    uint32_t fb = frame_bytes(s);
    uint64_t adv = (now_ns() - s->play_ns) / 1000 * s->bps / 1000000ull;   /* bytes since Play */
    uint64_t byte = (uint64_t)s->start_byte + adv;
    *ended = 0;
    if (byte >= s->size) {
        if (!s->looping) { *ended = 1; return total_frames; }
        byte %= s->size;
    }
    return (uint32_t)(byte / fb);
}

void w32_audio_mix(int16_t *out, int frames) {
    static int32_t acc[2 * MAX_FRAMES];
    if (frames > MAX_FRAMES) frames = MAX_FRAMES;
    memset(acc, 0, sizeof(int32_t) * 2 * (size_t)frames);
    pthread_mutex_lock(&g_lock);
    for (int k = 0; k < MAX_SRC; k++) {
        slot *t = &g_src[k];
        if (!t->used || !t->s.playing || !t->s.mem) continue;
        w32_audio_src *s = &t->s;
        uint32_t fb = frame_bytes(s), total = fb ? s->size / fb : 0;
        uint32_t freq = s->freq ? s->freq : OUT_RATE;
        if (!total) continue;
        if (s->play_ns && s->bps) {
            int ended;
            uint32_t want = clock_frame(s, total, &ended);
            if (ended) { s->playing = 0; continue; }
            uint32_t have = (uint32_t)(t->pos_fp >> 16) % total;
            uint32_t d = want > have ? want - have : have - want;
            if (d > total / 2) d = total - d;                        /* the short way round */
            if (d > freq / 10) t->pos_fp = (uint64_t)want << 16;
        }
        uint64_t step = ((uint64_t)freq << 16) / OUT_RATE;
        int nch = s->channels > 1 ? 2 : 1;
        for (int i = 0; i < frames; i++) {
            uint32_t fr = (uint32_t)(t->pos_fp >> 16);
            if (fr >= total) {
                if (!s->looping) {
                    t->ended++;
                    if (!t->has_next) { s->playing = 0; break; }
                    /* the next block, from its start, without a gap: the
                     * fraction of a frame carried over is the resampler's */
                    void *owner = s->owner; int playing = s->playing;
                    t->s = t->next; t->has_next = 0;
                    t->s.playing = playing;                 /* the voice is playing; the block was queued before it started */
                    if (!t->s.owner) t->s.owner = owner; else free(owner);
                    s = &t->s;
                    fb = frame_bytes(s); total = fb ? s->size / fb : 0;
                    freq = s->freq ? s->freq : OUT_RATE;
                    step = ((uint64_t)freq << 16) / OUT_RATE;
                    nch = s->channels > 1 ? 2 : 1;
                    t->pos_fp = ((uint64_t)(fb ? s->start_byte / fb : 0) << 16) | (t->pos_fp & 0xFFFF);
                    set_gains(t);
                    if (!total) { s->playing = 0; break; }
                    fr = (uint32_t)(t->pos_fp >> 16);
                    if (fr >= total) { t->pos_fp = 0; fr = 0; }
                } else {
                    t->pos_fp -= (uint64_t)total << 16;
                    fr = (uint32_t)(t->pos_fp >> 16);
                    if (fr >= total) { t->pos_fp = 0; fr = 0; }
                }
            }
            int32_t l = sample_at(s, fr, 0), r = nch > 1 ? sample_at(s, fr, 1) : l;
            acc[2 * i]     += (int32_t)(((int64_t)l * t->gl) >> 15);
            acc[2 * i + 1] += (int32_t)(((int64_t)r * t->gr) >> 15);
            t->pos_fp += step;
            t->frames_done += step;
        }
    }
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < 2 * frames; i++)
        out[i] = (int16_t)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
}

/* RIFF/WAVE, PCM or IEEE float, plain or WAVE_FORMAT_EXTENSIBLE. `out->mem`
 * points into `p`; the caller owns the bytes. */
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
int w32_audio_parse_wav(const uint8_t *p, size_t n, w32_audio_src *out) {
    memset(out, 0, sizeof *out);
    if (n < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) return 0;
    size_t off = 12; int have_fmt = 0;
    while (off + 8 <= n) {
        uint32_t sz = rd32(p + off + 4);
        const uint8_t *body = p + off + 8;
        size_t avail = n - off - 8;
        if (!memcmp(p + off, "fmt ", 4) && sz >= 16 && avail >= 16) {
            uint16_t tag = rd16(body);
            out->channels = rd16(body + 2);
            out->freq = rd32(body + 4);
            out->bps = rd32(body + 8);
            out->bits = rd16(body + 14);
            if (tag == 0xFFFE && sz >= 40 && avail >= 40) tag = rd16(body + 24);   /* extensible: the subformat's first word */
            out->is_float = tag == 3;
            have_fmt = tag == 1 || tag == 3;
        } else if (!memcmp(p + off, "data", 4)) {
            if (sz > avail) sz = (uint32_t)avail;
            out->mem = body; out->size = sz;
        }
        off += 8 + sz + (sz & 1);
    }
    if (!have_fmt || !out->mem || !out->channels || !out->freq || !out->bits) return 0;
    if (!out->bps) out->bps = out->freq * frame_bytes(out);
    return 1;
}

/* ---- the device ------------------------------------------------------------ */
#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
static AudioQueueRef g_q;
enum { Q_BUFS = 3, Q_FRAMES = 512 };
static void q_fill(void *ud, AudioQueueRef q, AudioQueueBufferRef b) {
    (void)ud;
    int frames = (int)(b->mAudioDataBytesCapacity / 4);
    w32_audio_mix((int16_t *)b->mAudioData, frames);
    b->mAudioDataByteSize = (UInt32)frames * 4;
    AudioQueueEnqueueBuffer(q, b, 0, 0);
}
int w32_audio_open(void) {
    if (g_device_on) return 1;
    if (getenv("WINRUN_NO_AUDIO")) return 0;
    AudioStreamBasicDescription d;
    memset(&d, 0, sizeof d);
    d.mSampleRate = OUT_RATE; d.mFormatID = kAudioFormatLinearPCM;
    d.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    d.mBytesPerPacket = 4; d.mFramesPerPacket = 1; d.mBytesPerFrame = 4;
    d.mChannelsPerFrame = 2; d.mBitsPerChannel = 16;
    if (AudioQueueNewOutput(&d, q_fill, 0, 0, 0, 0, &g_q) != noErr || !g_q) {
        static int said; if (!said++) fprintf(stderr, "winrun: audio: no output device; sound stays silent\n");
        g_q = 0; return 0;
    }
    for (int i = 0; i < Q_BUFS; i++) {
        AudioQueueBufferRef b;
        if (AudioQueueAllocateBuffer(g_q, Q_FRAMES * 4, &b) != noErr) continue;
        q_fill(0, g_q, b);                                  /* primed with whatever is playing */
    }
    if (AudioQueueStart(g_q, 0) != noErr) { AudioQueueDispose(g_q, true); g_q = 0; return 0; }
    g_device_on = 1;
    return 1;
}
static void device_close(void) {
    if (g_q) { AudioQueueStop(g_q, true); AudioQueueDispose(g_q, true); g_q = 0; }
    g_device_on = 0;
}
#else
int w32_audio_open(void) { return 0; }
static void device_close(void) { g_device_on = 0; }
#endif

/* Everything off, before the guest memory the sources point into goes away.
 * The device is stopped synchronously first, so no callback is mid-mix when
 * the table is cleared. */
void w32_audio_close(void) {
    device_close();
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_SRC; i++) if (g_src[i].used) { free(g_src[i].s.owner); if (g_src[i].has_next) free(g_src[i].next.owner); memset(&g_src[i], 0, sizeof g_src[i]); }
    pthread_mutex_unlock(&g_lock);
}
int w32_audio_device_on(void) { return g_device_on; }

int w32_audio_src_queue_next(int id, const w32_audio_src *next) {
    if (id < 0 || id >= MAX_SRC) return 0;
    pthread_mutex_lock(&g_lock);
    slot *t = &g_src[id];
    int ok = t->used && !t->has_next;
    if (ok) {
        /* a source that has already run out takes the block as its current one */
        if (!t->s.playing && t->s.mem && !t->has_next && t->ended && t->frames_done) {
            void *owner = t->s.owner;
            t->s = *next; t->s.playing = 1; free(owner);
            uint32_t fb = frame_bytes(&t->s); t->pos_fp = (uint64_t)(fb ? next->start_byte / fb : 0) << 16;
            set_gains(t);
        } else { t->next = *next; t->has_next = 1; }
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}
int w32_audio_src_next_pending(int id) {
    if (id < 0 || id >= MAX_SRC) return 0;
    pthread_mutex_lock(&g_lock); int p = g_src[id].used && g_src[id].has_next; pthread_mutex_unlock(&g_lock);
    return p;
}
int w32_audio_src_take_ended(int id) {
    if (id < 0 || id >= MAX_SRC) return 0;
    pthread_mutex_lock(&g_lock); int n = g_src[id].used ? g_src[id].ended : 0; if (g_src[id].used) g_src[id].ended = 0; pthread_mutex_unlock(&g_lock);
    return n;
}
uint64_t w32_audio_src_frames_done(int id) {
    if (id < 0 || id >= MAX_SRC) return 0;
    pthread_mutex_lock(&g_lock); uint64_t n = g_src[id].used ? g_src[id].frames_done >> 16 : 0; pthread_mutex_unlock(&g_lock);
    return n;
}
int w32_audio_src_count(void) {
    int n = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_SRC; i++) n += g_src[i].used;
    pthread_mutex_unlock(&g_lock);
    return n;
}

void w32_audio_src_clear_next(int id) {
    if (id < 0 || id >= MAX_SRC) return;
    pthread_mutex_lock(&g_lock);
    if (g_src[id].used && g_src[id].has_next) { free(g_src[id].next.owner); memset(&g_src[id].next, 0, sizeof g_src[id].next); g_src[id].has_next = 0; }
    pthread_mutex_unlock(&g_lock);
}

/* No device: the mixer is driven by the wall clock instead, into nowhere.
 * Bounded per call so a long pause does not mix a minute of silence at once. */
void w32_audio_pump(void) {
    static uint64_t last_ns;
    static int16_t sink[2 * 1024];
    if (g_device_on) { last_ns = 0; return; }
    uint64_t now = now_ns();
    if (!last_ns) { last_ns = now; return; }
    uint64_t frames = (now - last_ns) * OUT_RATE / 1000000000ull;
    if (frames > 4 * 1024) frames = 4 * 1024;
    if (!frames) return;
    last_ns += frames * 1000000000ull / OUT_RATE;
    while (frames) { int n = frames > 1024 ? 1024 : (int)frames; w32_audio_mix(sink, n); frames -= (uint64_t)n; }
}
