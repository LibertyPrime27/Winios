/* dsound.dll -- audio that initialises.
 *
 * There is no sound here yet. That is deliberate and it is still worth
 * having, because of how games treat a failed audio init: a great many of
 * them do not degrade to silence, they abort. `DirectSoundCreate8` returning
 * an error is the end of the program, so "no audio" and "no audio subsystem"
 * are very different amounts of broken. This is the second: every call
 * succeeds, buffers can be created, locked, written to, played and stopped,
 * and the program gets on with the rest of its startup.
 *
 * A buffer really is guest memory of the size asked for, and Lock hands back
 * a pointer into it. That matters more than it sounds: a game writes its
 * mixed audio there and then reads its own play cursor to decide how much
 * more to write. So the cursor advances at the rate the buffer's format
 * implies, from a clock, and a program waiting to be told "you may write the
 * next 4 KB" is told so on time. Get that wrong and a game that would have
 * been silent instead hangs in its audio thread.
 *
 * What is missing is only the last step: handing those bytes to CoreAudio.
 * The buffer contents are correct by the time Play is called, so that step is
 * a consumer, not a redesign.
 *
 * Vtable slot numbers come from win32/vtables_gen.h, generated from
 * mingw-w64's dsound.h -- see tools/gen/vtables.sh for why they are not
 * counted by hand.
 */
#include "w32.h"
#include "vtables_gen.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

enum { S_OK_ = 0, E_FAIL_ = 0x80004005u, E_NOINTERFACE_ = 0x80004002u,
       DSERR_INVALIDPARAM = 0x80070057u };
/* DSBCAPS */
enum { DSBCAPS_PRIMARYBUFFER = 0x00000001, DSBCAPS_CTRLVOLUME = 0x00000080,
       DSBCAPS_GETCURRENTPOSITION2 = 0x00010000 };
/* DSBSTATUS */
enum { DSBSTATUS_PLAYING = 1, DSBSTATUS_BUFFERLOST = 2, DSBSTATUS_LOOPING = 4 };
enum { DSSCL_NORMAL = 1, DSSCL_PRIORITY = 2, DSSCL_EXCLUSIVE = 3, DSSCL_WRITEPRIMARY = 4 };

/* Object fields (w32_com_new gives 64-bit slots). */
enum { DS_NBUF = 0, DS_COOP = 1, DS_NFIELDS = 4 };
enum { B_MEM = 0, B_SIZE = 1, B_FLAGS = 2, B_FREQ = 3, B_BYTES_PER_SEC = 4,
       B_STATUS = 5, B_PLAY_NS = 6, B_WRITE = 7, B_VOL = 8, B_PAN = 9,
       B_LOCKED = 10, B_NFIELDS = 12 };
enum { TAG_DS = 0x44530000, TAG_DSBUF, TAG_DS3L, TAG_DS3B, TAG_DSNOTIFY };

static w32_com_class cls_ds, cls_buf, cls_3dl, cls_3db, cls_notify;
static void b_QueryInterface(w32 *w);   /* needs the classes above; defined below */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* --- IDirectSoundBuffer8 ------------------------------------------------- */

/* Where the hardware would be reading from by now. A game polls this to pace
 * its writes, so it has to move, and it has to move at the buffer's own rate
 * rather than at some arbitrary speed -- a cursor that runs fast starves the
 * game's mixer and one that runs slow makes it spin. */
static uint32_t play_cursor(w32 *w, uint64_t self) {
    uint32_t size = (uint32_t)w32_com_get(w, self, B_SIZE);
    uint32_t bps = (uint32_t)w32_com_get(w, self, B_BYTES_PER_SEC);
    if (!size || !bps) return 0;
    if (!(w32_com_get(w, self, B_STATUS) & DSBSTATUS_PLAYING)) return (uint32_t)w32_com_get(w, self, B_WRITE);
    uint64_t started = w32_com_get(w, self, B_PLAY_NS);
    uint64_t elapsed = now_ns() - started;
    return (uint32_t)((elapsed * bps / 1000000000ull) % size);
}

static void b_GetCaps(w32 *w) {
    uint64_t c = ARG(1);
    if (!c) { RET(DSERR_INVALIDPARAM); return; }
    /* DSBCAPS: dwSize, dwFlags, dwBufferBytes, dwUnlockTransferRate, dwPlayCpuOverhead */
    w32_write(w, c + 0, 4, 20);
    w32_write(w, c + 4, 4, (uint32_t)w32_com_get(w, ARG(0), B_FLAGS));
    w32_write(w, c + 8, 4, (uint32_t)w32_com_get(w, ARG(0), B_SIZE));
    w32_write(w, c + 12, 4, 0);
    w32_write(w, c + 16, 4, 0);
    RET(S_OK_);
}
/* GetCurrentPosition(play, write). The write cursor leads the play cursor --
 * that gap is the region the hardware has not committed to yet, and a game
 * writes ahead of it. Reporting them equal tells a game it may overwrite what
 * is about to play. */
static void b_GetCurrentPosition(w32 *w) {
    uint64_t self = ARG(0);
    uint32_t size = (uint32_t)w32_com_get(w, self, B_SIZE);
    uint32_t play = play_cursor(w, self);
    uint32_t bps = (uint32_t)w32_com_get(w, self, B_BYTES_PER_SEC);
    uint32_t lead = bps ? bps / 100 : 0;                 /* 10 ms, as real hardware roughly is */
    if (ARG(1)) w32_write(w, ARG(1), 4, play);
    if (ARG(2)) w32_write(w, ARG(2), 4, size ? (play + lead) % size : 0);
    RET(S_OK_);
}
static void b_SetCurrentPosition(w32 *w) {
    w32_com_set(w, ARG(0), B_WRITE, ARG(1));
    w32_com_set(w, ARG(0), B_PLAY_NS, now_ns());
    RET(S_OK_);
}
/* GetFormat(pwfx, size, written): a WAVEFORMATEX describing what we accepted. */
static void b_GetFormat(w32 *w) {
    uint64_t p = ARG(1);
    uint32_t cap = (uint32_t)ARG(2);
    uint32_t freq = (uint32_t)w32_com_get(w, ARG(0), B_FREQ);
    if (!freq) freq = 44100;
    if (ARG(3)) w32_write(w, ARG(3), 4, 18);
    if (!p) { RET(S_OK_); return; }
    if (cap < 18) { RET(DSERR_INVALIDPARAM); return; }
    w32_write(w, p + 0, 2, 1);                  /* WAVE_FORMAT_PCM */
    w32_write(w, p + 2, 2, 2);                  /* stereo */
    w32_write(w, p + 4, 4, freq);
    w32_write(w, p + 8, 4, freq * 4);           /* nAvgBytesPerSec */
    w32_write(w, p + 12, 2, 4);                 /* nBlockAlign */
    w32_write(w, p + 14, 2, 16);                /* wBitsPerSample */
    w32_write(w, p + 16, 2, 0);                 /* cbSize */
    RET(S_OK_);
}
static void b_SetFormat(w32 *w) {
    uint64_t p = ARG(1);
    if (p) {
        uint32_t freq = (uint32_t)w32_read(w, p + 4, 4);
        uint32_t bps = (uint32_t)w32_read(w, p + 8, 4);
        if (freq) w32_com_set(w, ARG(0), B_FREQ, freq);
        if (bps) w32_com_set(w, ARG(0), B_BYTES_PER_SEC, bps);
    }
    RET(S_OK_);
}
static void b_GetVolume(w32 *w) { if (ARG(1)) w32_write(w, ARG(1), 4, (uint32_t)w32_com_get(w, ARG(0), B_VOL)); RET(S_OK_); }
static void b_SetVolume(w32 *w) { w32_com_set(w, ARG(0), B_VOL, ARG(1)); RET(S_OK_); }
static void b_GetPan(w32 *w) { if (ARG(1)) w32_write(w, ARG(1), 4, (uint32_t)w32_com_get(w, ARG(0), B_PAN)); RET(S_OK_); }
static void b_SetPan(w32 *w) { w32_com_set(w, ARG(0), B_PAN, ARG(1)); RET(S_OK_); }
static void b_GetFrequency(w32 *w) { if (ARG(1)) w32_write(w, ARG(1), 4, (uint32_t)w32_com_get(w, ARG(0), B_FREQ)); RET(S_OK_); }
static void b_SetFrequency(w32 *w) {
    if (ARG(1)) { w32_com_set(w, ARG(0), B_FREQ, ARG(1)); w32_com_set(w, ARG(0), B_BYTES_PER_SEC, ARG(1) * 4); }
    RET(S_OK_);
}
static void b_GetStatus(w32 *w) { if (ARG(1)) w32_write(w, ARG(1), 4, (uint32_t)w32_com_get(w, ARG(0), B_STATUS)); RET(S_OK_); }

/* Lock(offset, bytes, pp1, pn1, pp2, pn2, flags). A lock can wrap the end of
 * a circular buffer, which is why there are two pointers; a game that asks
 * for a region crossing the end and gets one pointer back writes past it.
 * DSBLOCK_ENTIREBUFFER is 2. */
static void b_Lock(w32 *w) {
    uint64_t self = ARG(0);
    uint32_t off = (uint32_t)ARG(1), want = (uint32_t)ARG(2);
    uint32_t flags = (uint32_t)ARG(7);
    uint32_t size = (uint32_t)w32_com_get(w, self, B_SIZE);
    uint64_t mem = w32_com_get(w, self, B_MEM);
    if (!mem || !size) { RET(E_FAIL_); return; }
    if (flags & 2) { off = 0; want = size; }
    if (off >= size) { RET(DSERR_INVALIDPARAM); return; }
    if (want > size) want = size;
    uint32_t first = want, second = 0;
    if (off + want > size) { first = size - off; second = want - first; }
    if (ARG(3)) w32_write(w, ARG(3), (int)w32_ptrsize(w), mem + off);
    if (ARG(4)) w32_write(w, ARG(4), 4, first);
    if (ARG(5)) w32_write(w, ARG(5), (int)w32_ptrsize(w), second ? mem : 0);
    if (ARG(6)) w32_write(w, ARG(6), 4, second);
    w32_com_set(w, self, B_LOCKED, 1);
    RET(S_OK_);
}
/* Unlock(p1, n1, p2, n2) */
static void b_Unlock(w32 *w) { w32_com_set(w, ARG(0), B_LOCKED, 0); RET(S_OK_); }
static void b_Play(w32 *w) {
    uint32_t flags = (uint32_t)ARG(3);
    uint32_t st = DSBSTATUS_PLAYING | ((flags & 1) ? DSBSTATUS_LOOPING : 0);
    w32_com_set(w, ARG(0), B_STATUS, st);
    w32_com_set(w, ARG(0), B_PLAY_NS, now_ns());
    RET(S_OK_);
}
static void b_Stop(w32 *w) {
    w32_com_set(w, ARG(0), B_WRITE, play_cursor(w, ARG(0)));
    w32_com_set(w, ARG(0), B_STATUS, 0);
    RET(S_OK_);
}
/* A buffer is never lost here: there is no device to lose it to. */
static void b_Restore(w32 *w) { w32_com_set(w, ARG(0), B_STATUS, (uint32_t)w32_com_get(w, ARG(0), B_STATUS) & ~(uint64_t)DSBSTATUS_BUFFERLOST); RET(S_OK_); }
static void b_Initialize(w32 *w) { (void)w; RET(S_OK_); }
/* SetFX(count, pDSFXDesc, pdwResultCodes): one result per effect asked for,
 * and DSFXR_UNKNOWN (0) for each, because none were located. A game that
 * reads only the HRESULT is satisfied either way; one that walks the array
 * would read whatever was there. */
static void b_SetFX(w32 *w) {
    uint32_t n = (uint32_t)ARG(1);
    if (ARG(3)) for (uint32_t i = 0; i < n && i < 64; i++) w32_write(w, ARG(3) + 4u * i, 4, 0);
    RET(S_OK_);
}
static void b_AcquireResources(w32 *w) { (void)w; RET(S_OK_); }
static void b_GetObjectInPath(w32 *w) { if (ARG(4)) w32_write(w, ARG(4), (int)w32_ptrsize(w), 0); RET(E_NOINTERFACE_); }

/* --- the 3D interfaces, which games query for and then mostly set ------- */

static void ok0(w32 *w) { RET(S_OK_); }
static void ok_get_f(w32 *w) { if (ARG(1)) w32_write(w, ARG(1), 4, 0); RET(S_OK_); }

/* --- IDirectSound8 ------------------------------------------------------ */

/* CreateSoundBuffer(desc, ppBuffer, unkOuter). DSBUFFERDESC is
 * dwSize, dwFlags, dwBufferBytes, dwReserved, lpwfxFormat, guid3DAlgorithm --
 * so the fields wanted are at fixed offsets in both bitnesses up to the
 * format pointer, which is where the pointer size first matters. */
static void ds_CreateSoundBuffer(w32 *w) {
    uint64_t desc = ARG(1), out = ARG(2);
    if (!desc || !out) { RET(DSERR_INVALIDPARAM); return; }
    uint32_t flags = (uint32_t)w32_read(w, desc + 4, 4);
    uint32_t bytes = (uint32_t)w32_read(w, desc + 8, 4);
    uint64_t fmt = w32_read(w, desc + 16, (int)w32_ptrsize(w));

    uint32_t freq = 44100, bps = 44100 * 4;
    if (fmt) {
        uint32_t f = (uint32_t)w32_read(w, fmt + 4, 4);
        uint32_t a = (uint32_t)w32_read(w, fmt + 8, 4);
        if (f) freq = f;
        if (a) bps = a;
    }
    /* A primary buffer has no bytes of its own; a game asking for one wants
     * the mixer's format, not storage. Give it something to hold anyway so a
     * WRITEPRIMARY game's Lock has somewhere to write. */
    if (!bytes) bytes = bps / 10;                /* 100 ms */
    if (bytes > (16u << 20)) bytes = 16u << 20;

    uint64_t mem = w32_alloc(w, bytes, 0);
    if (!mem) { fprintf(stderr, "winrun: dsound: no memory for a %u-byte buffer\n", bytes); RET(E_FAIL_); return; }
    /* Silence, so a game that plays without writing does not play garbage --
     * and so a test can tell "wrote nothing" from "wrote something". */
    memset(W32P(w, mem), 0, bytes);

    uint64_t b = w32_com_new(w, &cls_buf, B_NFIELDS);
    if (!b) { RET(E_FAIL_); return; }
    w32_com_set(w, b, B_MEM, mem);
    w32_com_set(w, b, B_SIZE, bytes);
    w32_com_set(w, b, B_FLAGS, flags);
    w32_com_set(w, b, B_FREQ, freq);
    w32_com_set(w, b, B_BYTES_PER_SEC, bps);
    w32_com_set(w, b, B_VOL, 0);                 /* DSBVOLUME_MAX */
    w32_com_set(w, ARG(0), DS_NBUF, w32_com_get(w, ARG(0), DS_NBUF) + 1);
    w32_write(w, out, (int)w32_ptrsize(w), b);
    if (w->verbose)
        fprintf(stderr, "winrun: dsound: buffer %u bytes, %u Hz, flags %#x%s\n",
                bytes, freq, flags, (flags & DSBCAPS_PRIMARYBUFFER) ? " (primary)" : "");
    RET(S_OK_);
}

static void ds_GetCaps(w32 *w) {
    uint64_t c = ARG(1);
    if (!c) { RET(DSERR_INVALIDPARAM); return; }
    /* DSCAPS is large and mostly counters; the fields a game branches on are
     * dwFlags and the buffer limits. Zeroed first so nothing is left to
     * whatever the guest had there. */
    void *p = W32PN(w, c, 96);
    if (!p) { RET(DSERR_INVALIDPARAM); return; }
    memset(p, 0, 96);
    w32_write(w, c + 0, 4, 96);
    w32_write(w, c + 4, 4, 0x00000E3F);          /* primary/secondary, all formats */
    w32_write(w, c + 8, 4, 200000);              /* dwMinSecondarySampleRate */
    w32_write(w, c + 12, 4, 200000);             /* dwMaxSecondarySampleRate */
    w32_write(w, c + 16, 4, 1);                  /* dwPrimaryBuffers */
    w32_write(w, c + 20, 4, 64);                 /* dwMaxHwMixingAllBuffers */
    RET(S_OK_);
}
static void ds_DuplicateSoundBuffer(w32 *w) {
    /* Sharing the original's memory is what duplication means -- two cursors
     * over one set of samples. */
    uint64_t src = ARG(1), out = ARG(2);
    if (!src || !out) { RET(DSERR_INVALIDPARAM); return; }
    uint64_t b = w32_com_new(w, &cls_buf, B_NFIELDS);
    if (!b) { RET(E_FAIL_); return; }
    for (int i = 0; i < B_NFIELDS; i++) w32_com_set(w, b, i, w32_com_get(w, src, i));
    w32_com_set(w, b, B_STATUS, 0);
    w32_write(w, out, (int)w32_ptrsize(w), b);
    RET(S_OK_);
}
static void ds_SetCooperativeLevel(w32 *w) { w32_com_set(w, ARG(0), DS_COOP, ARG(2)); RET(S_OK_); }
static void ds_GetSpeakerConfig(w32 *w) { if (ARG(1)) w32_write(w, ARG(1), 4, 6); RET(S_OK_); }   /* DSSPEAKER_STEREO */
static void ds_SetSpeakerConfig(w32 *w) { (void)w; RET(S_OK_); }
static void ds_VerifyCertification(w32 *w) { if (ARG(1)) w32_write(w, ARG(1), 4, 0); RET(S_OK_); }

/* --- the vtables, indexed by generated slot name ------------------------ */

static w32_api ds_methods[DS8_NSLOTS];
static w32_api buf_methods[DSB8_NSLOTS];
static w32_api l3d_methods[DS3L_NSLOTS];
static w32_api b3d_methods[DS3B_NSLOTS];
static w32_api notify_methods[DSN_NSLOTS];

/* Parameters named so they cannot collide with the fields they assign to:
 * `name` as a macro parameter turns `tab[slot].name` into `tab[slot]."..."`. */
#define M(tab, slot, nm, na, f) \
    do { (tab)[slot].name = (nm); (tab)[slot].nargs = (na); (tab)[slot].fn = (f); } while (0)

/* Built at first use rather than as initialisers, because designated
 * initialisers with generated indices read worse than this does and a hole
 * left by a typo would be silent either way. Anything not named here stays a
 * null entry, which the stub mechanism reports as "unimplemented slot N". */
static int g_built;
static void build_tables(void) {
    if (g_built) return;
    g_built = 1;
    for (int i = 0; i < DS8_NSLOTS; i++)  ds_methods[i]  = (w32_api){ 0, 0, 0, 0, 0 };
    for (int i = 0; i < DSB8_NSLOTS; i++) buf_methods[i] = (w32_api){ 0, 0, 0, 0, 0 };

    M(ds_methods, DS8_QueryInterface, "QueryInterface", 3, w32_com_QueryInterface);
    M(ds_methods, DS8_AddRef, "AddRef", 1, w32_com_AddRef);
    M(ds_methods, DS8_Release, "Release", 1, w32_com_Release);
    M(ds_methods, DS8_CreateSoundBuffer, "CreateSoundBuffer", 4, ds_CreateSoundBuffer);
    M(ds_methods, DS8_GetCaps, "GetCaps", 2, ds_GetCaps);
    M(ds_methods, DS8_DuplicateSoundBuffer, "DuplicateSoundBuffer", 3, ds_DuplicateSoundBuffer);
    M(ds_methods, DS8_SetCooperativeLevel, "SetCooperativeLevel", 3, ds_SetCooperativeLevel);
    M(ds_methods, DS8_Compact, "Compact", 1, ok0);
    M(ds_methods, DS8_GetSpeakerConfig, "GetSpeakerConfig", 2, ds_GetSpeakerConfig);
    M(ds_methods, DS8_SetSpeakerConfig, "SetSpeakerConfig", 2, ds_SetSpeakerConfig);
    M(ds_methods, DS8_Initialize, "Initialize", 2, ok0);
    M(ds_methods, DS8_VerifyCertification, "VerifyCertification", 2, ds_VerifyCertification);

    M(buf_methods, DSB8_QueryInterface, "QueryInterface", 3, b_QueryInterface);
    M(buf_methods, DSB8_AddRef, "AddRef", 1, w32_com_AddRef);
    M(buf_methods, DSB8_Release, "Release", 1, w32_com_Release);
    M(buf_methods, DSB8_GetCaps, "GetCaps", 2, b_GetCaps);
    M(buf_methods, DSB8_GetCurrentPosition, "GetCurrentPosition", 3, b_GetCurrentPosition);
    M(buf_methods, DSB8_GetFormat, "GetFormat", 4, b_GetFormat);
    M(buf_methods, DSB8_GetVolume, "GetVolume", 2, b_GetVolume);
    M(buf_methods, DSB8_GetPan, "GetPan", 2, b_GetPan);
    M(buf_methods, DSB8_GetFrequency, "GetFrequency", 2, b_GetFrequency);
    M(buf_methods, DSB8_GetStatus, "GetStatus", 2, b_GetStatus);
    M(buf_methods, DSB8_Initialize, "Initialize", 3, b_Initialize);
    M(buf_methods, DSB8_Lock, "Lock", 8, b_Lock);
    M(buf_methods, DSB8_Play, "Play", 4, b_Play);
    M(buf_methods, DSB8_SetCurrentPosition, "SetCurrentPosition", 2, b_SetCurrentPosition);
    M(buf_methods, DSB8_SetFormat, "SetFormat", 2, b_SetFormat);
    M(buf_methods, DSB8_SetVolume, "SetVolume", 2, b_SetVolume);
    M(buf_methods, DSB8_SetPan, "SetPan", 2, b_SetPan);
    M(buf_methods, DSB8_SetFrequency, "SetFrequency", 2, b_SetFrequency);
    M(buf_methods, DSB8_Stop, "Stop", 1, b_Stop);
    M(buf_methods, DSB8_Unlock, "Unlock", 5, b_Unlock);
    M(buf_methods, DSB8_Restore, "Restore", 1, b_Restore);
    M(buf_methods, DSB8_SetFX, "SetFX", 4, b_SetFX);
    M(buf_methods, DSB8_AcquireResources, "AcquireResources", 4, b_AcquireResources);
    M(buf_methods, DSB8_GetObjectInPath, "GetObjectInPath", 5, b_GetObjectInPath);

    /* The 3D interfaces: a game sets a listener and per-buffer positions and
     * never reads them back for anything but its own bookkeeping. Accepting
     * them and returning zeros is the whole of it until there is a mixer. */
    for (int i = 3; i < DS3L_NSLOTS; i++) M(l3d_methods, i, "IDirectSound3DListener method", 2, ok_get_f);
    M(l3d_methods, DS3L_QueryInterface, "QueryInterface", 3, w32_com_QueryInterface);
    M(l3d_methods, DS3L_AddRef, "AddRef", 1, w32_com_AddRef);
    M(l3d_methods, DS3L_Release, "Release", 1, w32_com_Release);
    M(l3d_methods, DS3L_CommitDeferredSettings, "CommitDeferredSettings", 1, ok0);
    M(l3d_methods, DS3L_SetPosition, "SetPosition", 5, ok0);
    M(l3d_methods, DS3L_SetOrientation, "SetOrientation", 8, ok0);
    M(l3d_methods, DS3L_SetVelocity, "SetVelocity", 5, ok0);
    M(l3d_methods, DS3L_SetAllParameters, "SetAllParameters", 3, ok0);

    for (int i = 3; i < DS3B_NSLOTS; i++) M(b3d_methods, i, "IDirectSound3DBuffer method", 2, ok_get_f);
    M(b3d_methods, DS3B_QueryInterface, "QueryInterface", 3, w32_com_QueryInterface);
    M(b3d_methods, DS3B_AddRef, "AddRef", 1, w32_com_AddRef);
    M(b3d_methods, DS3B_Release, "Release", 1, w32_com_Release);
    M(b3d_methods, DS3B_SetPosition, "SetPosition", 5, ok0);
    M(b3d_methods, DS3B_SetVelocity, "SetVelocity", 5, ok0);
    M(b3d_methods, DS3B_SetMode, "SetMode", 3, ok0);
    M(b3d_methods, DS3B_SetAllParameters, "SetAllParameters", 3, ok0);
    M(b3d_methods, DS3B_SetConeOrientation, "SetConeOrientation", 5, ok0);

    M(notify_methods, DSN_QueryInterface, "QueryInterface", 3, w32_com_QueryInterface);
    M(notify_methods, DSN_AddRef, "AddRef", 1, w32_com_AddRef);
    M(notify_methods, DSN_Release, "Release", 1, w32_com_Release);
    M(notify_methods, DSN_SetNotificationPositions, "SetNotificationPositions", 3, ok0);

    cls_ds     = (w32_com_class){ "IDirectSound8",          ds_methods,     DS8_NSLOTS,  TAG_DS,       0, {0} };
    cls_buf    = (w32_com_class){ "IDirectSoundBuffer8",    buf_methods,    DSB8_NSLOTS, TAG_DSBUF,    0, {0} };
    cls_3dl    = (w32_com_class){ "IDirectSound3DListener", l3d_methods,    DS3L_NSLOTS, TAG_DS3L,     0, {0} };
    cls_3db    = (w32_com_class){ "IDirectSound3DBuffer",   b3d_methods,    DS3B_NSLOTS, TAG_DS3B,     0, {0} };
    cls_notify = (w32_com_class){ "IDirectSoundNotify",     notify_methods, DSN_NSLOTS,  TAG_DSNOTIFY, 0, {0} };
}

/* QueryInterface, for real, on a sound buffer.
 *
 * The shared w32_com_QueryInterface hands back the same object for any IID,
 * which is harmless where every interface a guest asks for has the same
 * vtable. Not here: a game asks a buffer for IDirectSound3DBuffer and then
 * calls SetPosition, which is slot 17 there and GetFrequency on the buffer.
 * Returning self would be a call into a differently shaped vtable -- exactly
 * the failure the generated slot numbers in this file exist to prevent, so
 * the IID is matched properly instead.
 *
 * A GUID is Data1/Data2/Data3 little-endian then eight bytes, so the whole
 * sixteen are compared. Matching on Data1 alone would usually work and would
 * be a loose compare guarding the one thing worth being strict about.
 */
static const struct { uint8_t iid[16]; int which; } BUF_IIDS[] = {
    /* IID_IDirectSound3DBuffer   279AFA86-4981-11CE-A521-0020AF0BE560 */
    { { 0x86,0xFA,0x9A,0x27, 0x81,0x49, 0xCE,0x11, 0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60 }, 1 },
    /* IID_IDirectSoundNotify     B0210783-89CD-11D0-AF08-00A0C925CD16 */
    { { 0x83,0x07,0x21,0xB0, 0xCD,0x89, 0xD0,0x11, 0xAF,0x08,0x00,0xA0,0xC9,0x25,0xCD,0x16 }, 2 },
    /* IID_IDirectSound3DListener 279AFA84-4981-11CE-A521-0020AF0BE560 */
    { { 0x84,0xFA,0x9A,0x27, 0x81,0x49, 0xCE,0x11, 0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60 }, 3 },
};

static void b_QueryInterface(w32 *w) {
    uint64_t iid = ARG(1), out = ARG(2);
    if (!out) { RET(E_NOINTERFACE_); return; }
    if (iid) {
        const uint8_t *g = W32PN(w, iid, 16);
        for (size_t i = 0; g && i < sizeof BUF_IIDS / sizeof BUF_IIDS[0]; i++) {
            if (memcmp(g, BUF_IIDS[i].iid, 16)) continue;
            w32_com_class *cls = BUF_IIDS[i].which == 1 ? &cls_3db
                               : BUF_IIDS[i].which == 2 ? &cls_notify : &cls_3dl;
            uint64_t o = w32_com_new(w, cls, 2);
            if (!o) { RET(E_FAIL_); return; }
            w32_write(w, out, (int)w32_ptrsize(w), o);
            if (w->verbose) fprintf(stderr, "winrun: dsound: buffer asked for %s\n", cls->name);
            RET(S_OK_);
            return;
        }
    }
    /* anything else: the buffer itself, which is what IDirectSoundBuffer and
     * IDirectSoundBuffer8 both are here */
    w32_com_QueryInterface(w);
}

/* --- the exports -------------------------------------------------------- */

static void d_DirectSoundCreate8(w32 *w) {
    build_tables();
    uint64_t out = ARG(1);
    if (!out) { RET(DSERR_INVALIDPARAM); return; }
    uint64_t ds = w32_com_new(w, &cls_ds, DS_NFIELDS);
    if (!ds) { RET(E_FAIL_); return; }
    w32_write(w, out, (int)w32_ptrsize(w), ds);
    if (w->verbose) fprintf(stderr, "winrun: dsound: device created (silent)\n");
    RET(S_OK_);
}
static void d_DirectSoundCreate(w32 *w) { d_DirectSoundCreate8(w); }

/* CLSID_DirectSound and CLSID_DirectSound8, for a program that goes through
 * CoCreateInstance rather than DirectSoundCreate8 -- which is what one that
 * loads dsound lazily does. Same object either way. */
static const uint8_t CLSID_DirectSound_[16]  = W32_GUID(0x47d4d946, 0x62e8, 0x11cf, 0x93,0xbc,0x44,0x45,0x53,0x54,0x00,0x00);
static const uint8_t CLSID_DirectSound8_[16] = W32_GUID(0x3901cc3f, 0x84b5, 0x4fa4, 0xba,0x35,0xaa,0x81,0x72,0xb8,0xa0,0x9b);
int w32_dsound_create_class(w32 *w, const uint8_t clsid[16], const uint8_t iid[16], uint64_t out) {
    (void)iid;
    if (memcmp(clsid, CLSID_DirectSound_, 16) && memcmp(clsid, CLSID_DirectSound8_, 16)) return 0;
    build_tables();
    uint64_t ds = w32_com_new(w, &cls_ds, DS_NFIELDS);
    if (!ds) return 0;
    w32_write(w, out, (int)w32_ptrsize(w), ds);
    if (w->verbose) fprintf(stderr, "winrun: dsound: device created through CoCreateInstance (silent)\n");
    return 1;
}

/* DirectSoundEnumerate(callback, context): one device, so the callback is
 * called once. It runs on the guest's side, so this goes through
 * w32_call_guest -- and a callback returning FALSE means "stop", which with
 * one device is the same as returning at all. */
static void enumerate(w32 *w, int wide) {
    uint64_t cb = ARG(0);
    if (!cb) { RET(DSERR_INVALIDPARAM); return; }
    uint64_t desc = wide ? w32_wstrdup(w, "Primary Sound Driver") : w32_strdup(w, "Primary Sound Driver");
    uint64_t mod = wide ? w32_wstrdup(w, "") : w32_strdup(w, "");
    uint64_t args[4] = { 0, desc, mod, ARG(1) };     /* NULL GUID = the default device */
    w32_call_guest(w, cb, 4, args);
    RET(S_OK_);
}
static void d_DirectSoundEnumerateA(w32 *w) { enumerate(w, 0); }
static void d_DirectSoundEnumerateW(w32 *w) { enumerate(w, 1); }
static void d_DirectSoundCaptureCreate8(w32 *w) { (void)w; RET(E_FAIL_); }   /* no microphone */
static void d_DirectSoundCaptureEnumerateA(w32 *w) { (void)w; RET(S_OK_); }
static void d_GetDeviceID(w32 *w) {
    void *d = W32PN(w, ARG(1), 16);
    const void *s = W32PN(w, ARG(0), 16);
    if (d && s) memcpy(d, s, 16);
    RET(S_OK_);
}
/* dsound exports DllGetClassObject/DllCanUnloadNow for CoCreateInstance; a
 * game that goes that way gets told to use the create function instead. */
static void d_DllGetClassObject(w32 *w) { (void)w; RET(0x80040111u); }    /* CLASS_E_CLASSNOTAVAILABLE */
static void d_DllCanUnloadNow(w32 *w) { (void)w; RET(1); }

void w32_dsound_reset(void) { g_built = 0; }

#define F(n, a)  { #n, a, 0, d_##n, 0 }
const w32_api w32_dsound[] = {
    F(DirectSoundCreate, 3), F(DirectSoundCreate8, 3),
    F(DirectSoundEnumerateA, 2), F(DirectSoundEnumerateW, 2),
    F(DirectSoundCaptureCreate8, 3), F(DirectSoundCaptureEnumerateA, 2),
    F(GetDeviceID, 2), F(DllGetClassObject, 3), F(DllCanUnloadNow, 0),
    { 0, 0, 0, 0, 0 },
};
