/* 7z, decoded here rather than by a library.
 *
 * The format is a container of *folders* -- solid blocks -- each produced by
 * a small graph of coders: LZMA or LZMA2 for the compression, in front of it
 * often a branch filter (BCJ for x86, or BCJ2 with its four streams) that
 * turns relative call targets into absolute ones so they compress, sometimes
 * Delta. Files are substreams of a folder's output, back to back. A header
 * may itself be a folder (an "encoded header"), and a self-extractor is a
 * program with the archive appended.
 *
 * Everything decodes as a pull: a source is asked for the next n bytes and
 * asks its own inputs for what it needs. An LZMA decoder keeps only its
 * dictionary window, so a solid block of a gigabyte is streamed to disk
 * through a window of tens of megabytes, which is the difference between
 * working and not working on a phone. What is not decoded is said by name:
 * PPMd, the ARM/PowerPC/SPARC branch filters, and AES -- an encrypted
 * archive needs a password nobody here can ask for.
 *
 * LZMA follows the specification's reference decoder step for step, and the
 * x86 filter is the SDK's algorithm; both are checked to the byte against
 * archives produced by liblzma (tests/test_un7z.c).
 */
#include "un7z.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#define FAIL(...) do { if (err && err_len) snprintf(err, err_len, __VA_ARGS__); rc = -1; goto done; } while (0)

/* ---- sources ------------------------------------------------------------------- */
typedef struct src src;
struct src {
    int (*read)(src *s, uint8_t *out, size_t n);   /* bytes produced, 0 at end, -1 on error */
    void (*free_)(src *s);
    char err[128];
};
static int src_read_all(src *s, uint8_t *out, size_t n) {
    size_t got = 0;
    while (got < n) { int r = s->read(s, out + got, n - got); if (r <= 0) return r < 0 ? -1 : (int)got; got += (size_t)r; }
    return (int)got;
}
static void src_free(src *s) { if (s) s->free_(s); }

/* a region of the archive file */
typedef struct { src b; FILE *f; uint64_t off, left; uint8_t buf[1 << 16]; size_t bpos, blen; } file_src;
static int file_read(src *s, uint8_t *out, size_t n) {
    file_src *fs = (file_src *)s;
    size_t got = 0;
    while (got < n && (fs->left || fs->bpos < fs->blen)) {
        if (fs->bpos == fs->blen) {
            size_t want = fs->left < sizeof fs->buf ? (size_t)fs->left : sizeof fs->buf;
            if (fseeko(fs->f, (off_t)fs->off, SEEK_SET)) { snprintf(s->err, sizeof s->err, "seek failed"); return -1; }
            size_t r = fread(fs->buf, 1, want, fs->f);
            if (!r) { snprintf(s->err, sizeof s->err, "archive is truncated"); return -1; }
            fs->off += r; fs->left -= r; fs->bpos = 0; fs->blen = r;
        }
        size_t take = fs->blen - fs->bpos; if (take > n - got) take = n - got;
        memcpy(out + got, fs->buf + fs->bpos, take); fs->bpos += take; got += take;
    }
    return (int)got;
}
static void file_free(src *s) { free(s); }
static src *file_src_new(FILE *f, uint64_t off, uint64_t size) {
    file_src *fs = calloc(1, sizeof *fs); if (!fs) return 0;
    fs->b.read = file_read; fs->b.free_ = file_free; fs->f = f; fs->off = off; fs->left = size;
    return &fs->b;
}
/* ---- LZMA ------------------------------------------------------------------------ */
enum { kNumBitModelTotalBits = 11, kBitModelTotal = 1 << 11, kNumMoveBits = 5, kTopValue = 1u << 24 };
enum { kNumStates = 12, kNumPosBitsMax = 4, kNumLenToPosStates = 4, kNumAlignBits = 4, kEndPosModelIndex = 14,
       kNumFullDistances = 1 << (kEndPosModelIndex >> 1), kMatchMinLen = 2 };
typedef struct {
    /* the range coder, over an inner source with an optional byte limit */
    src *in; uint64_t in_left; int in_limited;
    uint8_t ibuf[1 << 14]; size_t ipos, ilen; int in_eof;
    uint32_t range, code;
    /* the model */
    int lc, lp, pb; uint16_t *probs; size_t nprobs;
    uint32_t state, rep[4];
    /* the window */
    uint8_t *dict; size_t dict_size, dict_pos; int dict_full;
    uint64_t processed;
    /* a match in progress across read() calls */
    uint32_t rem_len, rem_dist;
    int end_marker;
} lzma;
static int lz_byte(lzma *z) {
    if (z->ipos == z->ilen) {
        if (z->in_eof || (z->in_limited && !z->in_left)) { z->in_eof = 1; return 0; }   /* padding: the SDK does the same */
        size_t want = sizeof z->ibuf;
        if (z->in_limited && want > z->in_left) want = (size_t)z->in_left;
        int r = z->in->read(z->in, z->ibuf, want);
        if (r <= 0) { z->in_eof = 1; return 0; }
        z->ipos = 0; z->ilen = (size_t)r;
        if (z->in_limited) z->in_left -= (uint64_t)r;
    }
    return z->ibuf[z->ipos++];
}
static void rc_init(lzma *z) { z->code = 0; z->range = 0xFFFFFFFFu; for (int i = 0; i < 5; i++) z->code = (z->code << 8) | (uint8_t)lz_byte(z); }
static inline void rc_norm(lzma *z) { if (z->range < kTopValue) { z->range <<= 8; z->code = (z->code << 8) | (uint8_t)lz_byte(z); } }
static inline int rc_bit(lzma *z, uint16_t *p) {
    uint32_t bound = (z->range >> kNumBitModelTotalBits) * *p;
    int b;
    if (z->code < bound) { z->range = bound; *p += (kBitModelTotal - *p) >> kNumMoveBits; b = 0; }
    else { z->range -= bound; z->code -= bound; *p -= *p >> kNumMoveBits; b = 1; }
    rc_norm(z);
    return b;
}
static inline uint32_t rc_direct(lzma *z, int nbits) {
    uint32_t res = 0;
    while (nbits--) { z->range >>= 1; z->code -= z->range; uint32_t t = 0 - (z->code >> 31); z->code += z->range & t; res = (res << 1) + (t + 1); rc_norm(z); }
    return res;
}
static uint32_t rc_tree(lzma *z, uint16_t *probs, int nbits) { uint32_t m = 1; for (int i = 0; i < nbits; i++) m = (m << 1) + (uint32_t)rc_bit(z, &probs[m]); return m - (1u << nbits); }
static uint32_t rc_tree_rev(lzma *z, uint16_t *probs, int nbits) { uint32_t m = 1, sym = 0; for (int i = 0; i < nbits; i++) { int b = rc_bit(z, &probs[m]); m = (m << 1) + (uint32_t)b; sym |= (uint32_t)b << i; } return sym; }
/* probability layout, as in the specification */
enum { kLenChoice = 0, kLenChoice2 = 1, kLenLow = 2, kLenMid = kLenLow + (1 << kNumPosBitsMax) * 8, kLenHigh = kLenMid + (1 << kNumPosBitsMax) * 8, kNumLenProbs = kLenHigh + 256 };
enum { IsMatch = 0, IsRep = IsMatch + (kNumStates << kNumPosBitsMax), IsRepG0 = IsRep + kNumStates, IsRepG1 = IsRepG0 + kNumStates,
       IsRepG2 = IsRepG1 + kNumStates, IsRep0Long = IsRepG2 + kNumStates, PosSlot = IsRep0Long + (kNumStates << kNumPosBitsMax),
       SpecPos = PosSlot + (kNumLenToPosStates << 6), Align = SpecPos + kNumFullDistances - kEndPosModelIndex,
       LenCoder = Align + (1 << kNumAlignBits), RepLenCoder = LenCoder + kNumLenProbs, Literal = RepLenCoder + kNumLenProbs };
static int lz_set_props(lzma *z, int lc, int lp, int pb) {
    if (lc > 8 || lp > 4 || pb > 4) return -1;
    z->lc = lc; z->lp = lp; z->pb = pb;
    size_t n = Literal + ((size_t)0x300 << (lc + lp));
    if (n != z->nprobs) { free(z->probs); z->probs = malloc(n * sizeof *z->probs); if (!z->probs) return -1; z->nprobs = n; }
    return 0;
}
static void lz_reset_state(lzma *z) { for (size_t i = 0; i < z->nprobs; i++) z->probs[i] = kBitModelTotal >> 1; z->state = 0; z->rep[0] = z->rep[1] = z->rep[2] = z->rep[3] = 0; z->rem_len = 0; }
static void lz_reset_dict(lzma *z) { z->dict_pos = 0; z->dict_full = 0; z->processed = 0; }
static inline uint8_t dict_at(const lzma *z, uint32_t dist) { size_t p = z->dict_pos + z->dict_size - dist - 1; return z->dict[p % z->dict_size]; }
static inline void dict_put(lzma *z, uint8_t b) { z->dict[z->dict_pos++] = b; if (z->dict_pos == z->dict_size) { z->dict_pos = 0; z->dict_full = 1; } z->processed++; }
static uint32_t len_decode(lzma *z, uint16_t *lp, uint32_t pos_state) {
    if (!rc_bit(z, &lp[kLenChoice])) return rc_tree(z, lp + kLenLow + (pos_state << 3), 3);
    if (!rc_bit(z, &lp[kLenChoice2])) return 8 + rc_tree(z, lp + kLenMid + (pos_state << 3), 3);
    return 16 + rc_tree(z, lp + kLenHigh, 8);
}
/* Decode up to n bytes of the current stream into out (and the window). Stops
 * at the end marker or when `limit` bytes have been produced in this stream. */
static int lz_decode(lzma *z, uint8_t *out, size_t n, uint64_t limit) {
    size_t made = 0;
    while (made < n && z->processed < limit && !z->end_marker) {
        if (z->rem_len) {                                  /* a match, byte by byte through the window */
            uint8_t b = dict_at(z, z->rem_dist); dict_put(z, b); out[made++] = b; z->rem_len--; continue;
        }
        uint32_t pos_state = (uint32_t)z->processed & ((1u << z->pb) - 1);
        if (!rc_bit(z, &z->probs[IsMatch + (z->state << kNumPosBitsMax) + pos_state])) {
            uint32_t prev = (z->processed || z->dict_full) ? dict_at(z, 0) : 0;
            uint16_t *lit = z->probs + Literal + 0x300 * ((((uint32_t)z->processed & ((1u << z->lp) - 1)) << z->lc) + (prev >> (8 - z->lc)));
            uint32_t sym = 1;
            if (z->state >= 7) {
                uint32_t match = dict_at(z, z->rep[0]);
                do { uint32_t mb = (match >> 7) & 1; match <<= 1; int bit = rc_bit(z, &lit[((1 + mb) << 8) + sym]); sym = (sym << 1) | (uint32_t)bit; if (mb != (uint32_t)bit) break; } while (sym < 0x100);
            }
            while (sym < 0x100) sym = (sym << 1) | (uint32_t)rc_bit(z, &lit[sym]);
            uint8_t b = (uint8_t)sym; dict_put(z, b); out[made++] = b;
            z->state = z->state < 4 ? 0 : z->state < 10 ? z->state - 3 : z->state - 6;
            continue;
        }
        uint32_t len;
        if (rc_bit(z, &z->probs[IsRep + z->state])) {
            if (!z->processed && !z->dict_full) return -1;
            if (!rc_bit(z, &z->probs[IsRepG0 + z->state])) {
                if (!rc_bit(z, &z->probs[IsRep0Long + (z->state << kNumPosBitsMax) + pos_state])) {
                    z->state = z->state < 7 ? 9 : 11;
                    uint8_t b = dict_at(z, z->rep[0]); dict_put(z, b); out[made++] = b; continue;
                }
            } else {
                uint32_t dist;
                if (!rc_bit(z, &z->probs[IsRepG1 + z->state])) dist = z->rep[1];
                else { if (!rc_bit(z, &z->probs[IsRepG2 + z->state])) dist = z->rep[2]; else { dist = z->rep[3]; z->rep[3] = z->rep[2]; } z->rep[2] = z->rep[1]; }
                z->rep[1] = z->rep[0]; z->rep[0] = dist;
            }
            len = len_decode(z, z->probs + RepLenCoder, pos_state);
            z->state = z->state < 7 ? 8 : 11;
        } else {
            z->rep[3] = z->rep[2]; z->rep[2] = z->rep[1]; z->rep[1] = z->rep[0];
            len = len_decode(z, z->probs + LenCoder, pos_state);
            z->state = z->state < 7 ? 7 : 10;
            uint32_t slot = rc_tree(z, z->probs + PosSlot + ((len < kNumLenToPosStates - 1 ? len : kNumLenToPosStates - 1) << 6), 6);
            uint32_t dist;
            if (slot < 4) dist = slot;
            else {
                int nbits = (int)(slot >> 1) - 1; dist = (2 | (slot & 1)) << nbits;
                if (slot < kEndPosModelIndex) dist += rc_tree_rev(z, z->probs + SpecPos + dist - slot, nbits);
                else { dist += rc_direct(z, nbits - kNumAlignBits) << kNumAlignBits; dist += rc_tree_rev(z, z->probs + Align, kNumAlignBits); }
            }
            if (dist == 0xFFFFFFFFu) { z->end_marker = 1; break; }
            z->rep[0] = dist;
            if (dist >= (z->dict_full ? z->dict_size : z->dict_pos)) return -1;
        }
        z->rem_len = len + kMatchMinLen; z->rem_dist = z->rep[0];
    }
    return (int)made;
}
static int lz_alloc_dict(lzma *z, uint64_t dict_size, uint64_t total) {
    if (dict_size < 4096) dict_size = 4096;
    if (total && dict_size > total + 16) dict_size = total + 16;            /* no point in a window bigger than the data */
    if (dict_size > (1536ull << 20)) return -1;
    z->dict = malloc((size_t)dict_size); if (!z->dict) return -1;
    z->dict_size = (size_t)dict_size;
    return 0;
}
static void lz_free(lzma *z) { free(z->probs); free(z->dict); }

/* LZMA1: one stream, five bytes of properties. */
typedef struct { src b; lzma z; uint64_t total; int started; } lzma_src;
static int lzma1_read(src *s, uint8_t *out, size_t n) {
    lzma_src *l = (lzma_src *)s;
    if (!l->started) { rc_init(&l->z); l->started = 1; }
    int r = lz_decode(&l->z, out, n, l->total);
    if (r < 0) snprintf(s->err, sizeof s->err, "LZMA stream is corrupt");
    return r;
}
static void lzma1_free(src *s) { lzma_src *l = (lzma_src *)s; lz_free(&l->z); src_free(l->z.in); free(l); }
static src *lzma1_new(src *in, const uint8_t *props, size_t nprops, uint64_t total, char *err, size_t err_len) {
    if (nprops < 5) { snprintf(err, err_len, "LZMA properties are missing"); return 0; }
    lzma_src *l = calloc(1, sizeof *l); if (!l) return 0;
    int d = props[0]; int lc = d % 9; d /= 9; int lp = d % 5, pb = d / 5;
    uint32_t dict = (uint32_t)props[1] | (uint32_t)props[2] << 8 | (uint32_t)props[3] << 16 | (uint32_t)props[4] << 24;
    if (lz_set_props(&l->z, lc, lp, pb) || lz_alloc_dict(&l->z, dict, total)) { snprintf(err, err_len, "LZMA dictionary of %u MB is more than can be given", dict >> 20); free(l->z.probs); free(l); return 0; }
    lz_reset_state(&l->z); lz_reset_dict(&l->z);
    l->z.in = in; l->total = total;
    l->b.read = lzma1_read; l->b.free_ = lzma1_free;
    return &l->b;
}
/* LZMA2: chunks, each LZMA with a stated packed and unpacked size (and maybe
 * new properties or a dictionary reset), or stored. */
typedef struct { src b; lzma z; uint64_t chunk_left; int chunk_stored, need_props, at_end; src *in; uint64_t stored_left; } lzma2_src;
static int lzma2_read(src *s, uint8_t *out, size_t n) {
    lzma2_src *l = (lzma2_src *)s;
    size_t made = 0;
    while (made < n && !l->at_end) {
        if (!l->chunk_left) {
            uint8_t ctrl;
            if (src_read_all(l->in, &ctrl, 1) != 1) { snprintf(s->err, sizeof s->err, "LZMA2 stream ends without its end mark"); return -1; }
            if (ctrl == 0) { l->at_end = 1; break; }
            uint8_t hdr[5]; int nh = ctrl >= 0x80 ? 4 : 2;
            if (src_read_all(l->in, hdr, (size_t)nh) != nh) { snprintf(s->err, sizeof s->err, "LZMA2 chunk header is truncated"); return -1; }
            uint64_t unpacked = ((uint64_t)(ctrl & 0x1F) << 16 | (uint64_t)hdr[0] << 8 | hdr[1]) + 1;
            if (ctrl >= 0x80) {
                uint64_t packed = ((uint64_t)hdr[2] << 8 | hdr[3]) + 1;
                int mode = (ctrl >> 5) & 3;
                if (mode == 3) lz_reset_dict(&l->z);
                if (mode >= 2) {
                    uint8_t p; if (src_read_all(l->in, &p, 1) != 1) return -1;
                    int d = p; int lc = d % 9; d /= 9; int lp = d % 5, pb = d / 5;
                    if (lc + lp > 4 || lz_set_props(&l->z, lc, lp, pb)) { snprintf(s->err, sizeof s->err, "LZMA2 properties are invalid"); return -1; }
                    l->need_props = 0;
                } else if (l->need_props) { snprintf(s->err, sizeof s->err, "LZMA2 chunk without properties"); return -1; }
                if (mode >= 1) lz_reset_state(&l->z);
                l->z.in_left = packed; l->z.in_limited = 1; l->z.ipos = l->z.ilen = 0; l->z.in_eof = 0; l->z.end_marker = 0;
                l->z.rem_len = 0;
                rc_init(&l->z);
                l->chunk_left = unpacked; l->chunk_stored = 0;
                l->z.processed = 0;                        /* the chunk's own count; the window is what carries over */
            } else {
                if (ctrl > 2) { snprintf(s->err, sizeof s->err, "LZMA2 control byte %#x is invalid", ctrl); return -1; }
                if (ctrl == 1) lz_reset_dict(&l->z);
                l->chunk_left = unpacked; l->chunk_stored = 1;
            }
        }
        size_t want = n - made; if (want > l->chunk_left) want = (size_t)l->chunk_left;
        int r;
        if (l->chunk_stored) {
            r = src_read_all(l->in, out + made, want);
            if (r <= 0) { snprintf(s->err, sizeof s->err, "LZMA2 stored chunk is truncated"); return -1; }
            for (int i = 0; i < r; i++) dict_put(&l->z, out[made + (size_t)i]);
        } else {
            r = lz_decode(&l->z, out + made, want, l->z.processed + l->chunk_left);
            if (r < 0) { snprintf(s->err, sizeof s->err, "LZMA2 chunk is corrupt"); return -1; }
            if (r == 0) { snprintf(s->err, sizeof s->err, "LZMA2 chunk is short"); return -1; }
        }
        made += (size_t)r; l->chunk_left -= (uint64_t)r;
    }
    return (int)made;
}
static void lzma2_free(src *s) { lzma2_src *l = (lzma2_src *)s; lz_free(&l->z); src_free(l->in); free(l); }
static src *lzma2_new(src *in, const uint8_t *props, size_t nprops, uint64_t total, char *err, size_t err_len) {
    if (nprops < 1) { snprintf(err, err_len, "LZMA2 properties are missing"); return 0; }
    lzma2_src *l = calloc(1, sizeof *l); if (!l) return 0;
    uint32_t bits = props[0]; uint64_t dict = bits > 40 ? 0 : bits == 40 ? 0xFFFFFFFFull : (uint64_t)(2 | (bits & 1)) << (bits / 2 + 11);
    if (!dict || lz_alloc_dict(&l->z, dict, total)) { snprintf(err, err_len, "LZMA2 dictionary of %llu MB is more than can be given", (unsigned long long)(dict >> 20)); free(l); return 0; }
    l->z.in = in; l->in = in; l->need_props = 1;
    lz_reset_dict(&l->z);
    /* the LZMA core reads from its own bounded buffer; chunk headers and stored
     * chunks come straight from the inner source, so both must share it */
    l->b.read = lzma2_read; l->b.free_ = lzma2_free;
    return &l->b;
}
/* Note for LZMA2: the core's byte fetch uses z->in with a per-chunk limit, and
 * the chunk framing reads z->in directly between chunks. That only works
 * because the core stops fetching exactly at the limit and its buffer never
 * holds bytes past it (in_left bounds the read size). */

/* ---- the branch filters ------------------------------------------------------------ */
/* x86 (BCJ): the SDK's x86_Convert, decoding. Needs five bytes of lookahead, so
 * the last four are carried to the next call. */
#define Test86MSByte(b) ((b) == 0 || (b) == 0xFF)
static size_t x86_convert(uint8_t *data, size_t size, uint32_t ip, uint32_t *state) {
    static const uint8_t kMaskToAllowedStatus[8] = { 1, 1, 1, 0, 1, 0, 0, 0 };
    static const uint8_t kMaskToBitNumber[8] = { 0, 1, 2, 2, 3, 3, 3, 3 };
    size_t bufferPos = 0, prevPosT; uint32_t prevMask = *state & 7;
    if (size < 5) return 0;
    ip += 5; prevPosT = (size_t)0 - 1;
    for (;;) {
        uint8_t *p = data + bufferPos, *limit = data + size - 4;
        for (; p < limit; p++) if ((*p & 0xFE) == 0xE8) break;
        bufferPos = (size_t)(p - data);
        if (p >= limit) break;
        prevPosT = bufferPos - prevPosT;
        if (prevPosT > 3) prevMask = 0;
        else {
            prevMask = (prevMask << ((int)prevPosT - 1)) & 7;
            if (prevMask) { uint8_t b = p[4 - kMaskToBitNumber[prevMask]];
                if (!kMaskToAllowedStatus[prevMask] || Test86MSByte(b)) { prevPosT = bufferPos; prevMask = ((prevMask << 1) & 7) | 1; bufferPos++; continue; } }
        }
        prevPosT = bufferPos;
        if (Test86MSByte(p[4])) {
            uint32_t srcv = (uint32_t)p[4] << 24 | (uint32_t)p[3] << 16 | (uint32_t)p[2] << 8 | p[1], dest;
            for (;;) {
                dest = srcv - (ip + (uint32_t)bufferPos);
                if (!prevMask) break;
                int index = kMaskToBitNumber[prevMask] * 8;
                uint8_t b = (uint8_t)(dest >> (24 - index));
                if (!Test86MSByte(b)) break;
                srcv = dest ^ ((1u << (32 - index)) - 1);
            }
            p[4] = (uint8_t)(~(((dest >> 24) & 1) - 1)); p[3] = (uint8_t)(dest >> 16); p[2] = (uint8_t)(dest >> 8); p[1] = (uint8_t)dest;
            bufferPos += 5;
        } else { prevMask = ((prevMask << 1) & 7) | 1; bufferPos++; }
    }
    prevPosT = bufferPos - prevPosT;
    *state = prevPosT > 3 ? 0 : ((prevMask << ((int)prevPosT - 1)) & 7);
    return bufferPos;
}
typedef struct { src b; src *in; uint8_t buf[1 << 16]; size_t total, ready, pos; uint32_t ip, state; int eof; } bcj_src;
static int bcj_read(src *s, uint8_t *out, size_t n) {
    bcj_src *f = (bcj_src *)s;
    size_t made = 0;
    while (made < n) {
        if (f->pos < f->ready) { size_t take = f->ready - f->pos; if (take > n - made) take = n - made; memcpy(out + made, f->buf + f->pos, take); f->pos += take; made += take; continue; }
        if (f->eof) break;
        /* the unconverted tail moves to the front; the rest is refilled behind it */
        size_t tail = f->total - f->ready;
        memmove(f->buf, f->buf + f->ready, tail);
        f->total = tail; f->ready = 0; f->pos = 0;
        int r = f->in->read(f->in, f->buf + f->total, sizeof f->buf - f->total);
        if (r < 0) { snprintf(s->err, sizeof s->err, "%s", f->in->err); return -1; }
        f->total += (size_t)r;
        if (r == 0) { f->eof = 1; f->ready = f->total; continue; }      /* fewer than five bytes left: out as they are */
        size_t done = x86_convert(f->buf, f->total, f->ip, &f->state);
        f->ip += (uint32_t)done; f->ready = done;
    }
    return (int)made;
}
static void bcj_free(src *s) { bcj_src *f = (bcj_src *)s; src_free(f->in); free(f); }
static src *bcj_new(src *in) { bcj_src *f = calloc(1, sizeof *f); if (!f) return 0; f->in = in; f->b.read = bcj_read; f->b.free_ = bcj_free; return &f->b; }

/* a byte-at-a-time view of a source, for BCJ2 and the header */
typedef struct { src *s; uint8_t buf[4096]; size_t pos, len; int eof; } bytes;
static int bytes_get(bytes *b, uint8_t *out) {
    if (b->pos == b->len) {
        if (b->eof) return 0;
        int r = b->s->read(b->s, b->buf, sizeof b->buf);
        if (r < 0) return -1;
        if (r == 0) { b->eof = 1; return 0; }
        b->pos = 0; b->len = (size_t)r;
    }
    *out = b->buf[b->pos++];
    return 1;
}
/* BCJ2: the main stream carries the code with call and jump targets removed;
 * the call and jump streams carry them, big-endian; a range-coded bit stream
 * says, after each E8/E9/0F 8x, whether one was removed. */
typedef struct { src b; bytes main, call, jump, rc; uint16_t probs[2 + 256]; uint32_t range, code; uint8_t prev; uint32_t outpos; uint8_t pend[4]; int npend, started, eof; } bcj2_src;
static int bcj2_rc_bit(bcj2_src *f, uint16_t *p, int *bit) {
    uint32_t bound = (f->range >> kNumBitModelTotalBits) * *p;
    if (f->code < bound) { f->range = bound; *p += (kBitModelTotal - *p) >> kNumMoveBits; *bit = 0; }
    else { f->range -= bound; f->code -= bound; *p -= *p >> kNumMoveBits; *bit = 1; }
    if (f->range < kTopValue) { uint8_t c; if (bytes_get(&f->rc, &c) < 0) return -1; f->range <<= 8; f->code = (f->code << 8) | c; }
    return 0;
}
static int bcj2_read(src *s, uint8_t *out, size_t n) {
    bcj2_src *f = (bcj2_src *)s;
    if (!f->started) {
        for (int i = 0; i < 2 + 256; i++) f->probs[i] = kBitModelTotal >> 1;
        f->range = 0xFFFFFFFFu; f->code = 0;
        for (int i = 0; i < 5; i++) { uint8_t c = 0; if (bytes_get(&f->rc, &c) < 0) return -1; f->code = (f->code << 8) | c; }
        f->started = 1;
    }
    size_t made = 0;
    while (made < n) {
        if (f->npend) { out[made++] = f->pend[4 - f->npend]; f->npend--; f->outpos++; continue; }
        if (f->eof) break;
        uint8_t b; int r = bytes_get(&f->main, &b);
        if (r < 0) { snprintf(s->err, sizeof s->err, "BCJ2 main stream failed"); return -1; }
        if (r == 0) { f->eof = 1; break; }
        out[made++] = b; f->outpos++;
        int is_j = (b & 0xFE) == 0xE8 || (f->prev == 0x0F && (b & 0xF0) == 0x80);
        if (!is_j) { f->prev = b; continue; }
        uint16_t *prob = b == 0xE8 ? &f->probs[f->prev] : b == 0xE9 ? &f->probs[256] : &f->probs[257];
        int bit; if (bcj2_rc_bit(f, prob, &bit) < 0) { snprintf(s->err, sizeof s->err, "BCJ2 flag stream failed"); return -1; }
        if (!bit) { f->prev = b; continue; }
        bytes *from = b == 0xE8 ? &f->call : &f->jump;
        uint32_t srcv = 0;
        for (int i = 0; i < 4; i++) { uint8_t c; if (bytes_get(from, &c) != 1) { snprintf(s->err, sizeof s->err, "BCJ2 address stream is short"); return -1; } srcv = (srcv << 8) | c; }
        uint32_t dest = srcv - (f->outpos + 4);
        f->pend[0] = (uint8_t)dest; f->pend[1] = (uint8_t)(dest >> 8); f->pend[2] = (uint8_t)(dest >> 16); f->pend[3] = (uint8_t)(dest >> 24);
        f->npend = 4; f->prev = (uint8_t)(dest >> 24);
    }
    return (int)made;
}
static void bcj2_free(src *s) { bcj2_src *f = (bcj2_src *)s; src_free(f->main.s); src_free(f->call.s); src_free(f->jump.s); src_free(f->rc.s); free(f); }
static src *bcj2_new(src *mainv, src *call, src *jump, src *rc) {
    bcj2_src *f = calloc(1, sizeof *f); if (!f) return 0;
    f->main.s = mainv; f->call.s = call; f->jump.s = jump; f->rc.s = rc;
    f->b.read = bcj2_read; f->b.free_ = bcj2_free;
    return &f->b;
}
/* Delta: each byte is the difference from the one `dist` bytes before it. */
typedef struct { src b; src *in; uint8_t hist[256]; uint32_t dist, pos; } delta_src;
static int delta_read(src *s, uint8_t *out, size_t n) {
    delta_src *d = (delta_src *)s;
    int r = d->in->read(d->in, out, n);
    if (r < 0) { snprintf(s->err, sizeof s->err, "%s", d->in->err); return -1; }
    for (int i = 0; i < r; i++) { uint8_t v = (uint8_t)(out[i] + d->hist[(d->pos - d->dist) & 255]); out[i] = v; d->hist[d->pos & 255] = v; d->pos++; }
    return r;
}
static void delta_free(src *s) { delta_src *d = (delta_src *)s; src_free(d->in); free(d); }
static src *delta_new(src *in, const uint8_t *props, size_t nprops) { delta_src *d = calloc(1, sizeof *d); if (!d) return 0; d->in = in; d->dist = (nprops ? props[0] : 0) + 1u; d->b.read = delta_read; d->b.free_ = delta_free; return &d->b; }

/* ---- the header ---------------------------------------------------------------- */
enum { kEnd = 0, kHeader = 1, kArchiveProperties = 2, kAdditionalStreamsInfo = 3, kMainStreamsInfo = 4, kFilesInfo = 5,
       kPackInfo = 6, kUnPackInfo = 7, kSubStreamsInfo = 8, kSize = 9, kCRC = 10, kFolder = 11, kCodersUnPackSize = 12,
       kNumUnPackStream = 13, kEmptyStream = 14, kEmptyFile = 15, kAnti = 16, kName = 17, kCTime = 18, kATime = 19, kMTime = 20,
       kWinAttributes = 21, kComment = 22, kEncodedHeader = 23, kStartPos = 24, kDummy = 25 };
enum { MAX_CODERS = 8, MAX_STREAMS = 16 };
typedef struct { uint8_t id[16]; size_t idlen; uint32_t nin, nout; uint8_t props[64]; size_t nprops; } coder;
typedef struct {
    coder c[MAX_CODERS]; uint32_t ncoders;
    struct { uint32_t in, out; } bind[MAX_STREAMS]; uint32_t nbind;
    uint32_t packed[MAX_STREAMS]; uint32_t npacked;
    uint64_t unpack[MAX_STREAMS]; uint32_t nunpack;
    uint32_t crc; int has_crc;
    uint32_t first_pack;                    /* index of its first packed stream */
    uint32_t nsub;                          /* substreams (files) */
} folder;
typedef struct {
    uint64_t pack_pos; uint32_t npack; uint64_t *pack_size;
    folder *folders; uint32_t nfolders;
    uint64_t *sub_size; uint32_t *sub_crc; uint8_t *sub_crc_def; uint32_t nsub;
    uint32_t nfiles; char **names; uint8_t *empty_stream, *empty_file, *is_dir;
} archive;
typedef struct { const uint8_t *p; size_t n, pos; int bad; } cur;
static uint8_t c_byte(cur *c) { if (c->pos >= c->n) { c->bad = 1; return 0; } return c->p[c->pos++]; }
static uint64_t c_num(cur *c) {
    uint8_t first = c_byte(c); uint64_t v = 0; int mask = 0x80, extra = 0;
    for (; extra < 8 && (first & mask); extra++) mask >>= 1;
    for (int i = 0; i < extra; i++) v |= (uint64_t)c_byte(c) << (8 * i);
    if (extra < 8) v |= (uint64_t)(first & (mask - 1)) << (8 * extra);
    return v;
}
static uint32_t c_u32(cur *c) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)c_byte(c) << (8 * i); return v; }
static void c_skip(cur *c, uint64_t n) { if (n > c->n - c->pos) { c->bad = 1; c->pos = c->n; } else c->pos += (size_t)n; }
static uint8_t *c_bits(cur *c, uint32_t n, int all_defined_byte) {
    uint8_t *v = calloc(n ? n : 1, 1); if (!v) { c->bad = 1; return 0; }
    if (all_defined_byte && c_byte(c)) { memset(v, 1, n); return v; }
    uint8_t mask = 0, b = 0;
    for (uint32_t i = 0; i < n; i++) { if (!mask) { b = c_byte(c); mask = 0x80; } v[i] = (b & mask) != 0; mask >>= 1; }
    return v;
}
static void read_digests(cur *c, uint32_t n, uint32_t *crc, uint8_t *def) {
    uint8_t *d = c_bits(c, n, 1); if (!d) return;
    for (uint32_t i = 0; i < n; i++) { def[i] = d[i]; crc[i] = d[i] ? c_u32(c) : 0; }
    free(d);
}
static void archive_free(archive *a) {
    free(a->pack_size); free(a->folders); free(a->sub_size); free(a->sub_crc); free(a->sub_crc_def);
    if (a->names) for (uint32_t i = 0; i < a->nfiles; i++) free(a->names[i]);
    free(a->names); free(a->empty_stream); free(a->empty_file); free(a->is_dir);
    memset(a, 0, sizeof *a);
}
static int read_streams_info(cur *c, archive *a, char *err, size_t err_len) {
    for (;;) {
        uint8_t id = c_byte(c);
        if (c->bad) { snprintf(err, err_len, "7z header is truncated"); return -1; }
        if (id == kEnd) return 0;
        if (id == kPackInfo) {
            a->pack_pos = c_num(c); a->npack = (uint32_t)c_num(c);
            if (a->npack > 100000) { snprintf(err, err_len, "7z header is corrupt (pack count)"); return -1; }
            a->pack_size = calloc(a->npack ? a->npack : 1, sizeof *a->pack_size);
            for (;;) {
                uint8_t t = c_byte(c);
                if (t == kEnd || c->bad) break;
                if (t == kSize) { for (uint32_t i = 0; i < a->npack; i++) a->pack_size[i] = c_num(c); }
                else if (t == kCRC) { uint32_t *crc = calloc(a->npack ? a->npack : 1, 4); uint8_t *def = calloc(a->npack ? a->npack : 1, 1); read_digests(c, a->npack, crc, def); free(crc); free(def); }
                else c_skip(c, c_num(c));
            }
        } else if (id == kUnPackInfo) {
            if (c_byte(c) != kFolder) { snprintf(err, err_len, "7z header: folders expected"); return -1; }
            a->nfolders = (uint32_t)c_num(c);
            if (a->nfolders > 100000 || c_byte(c) != 0) { snprintf(err, err_len, "7z header: external folders are not supported"); return -1; }
            a->folders = calloc(a->nfolders ? a->nfolders : 1, sizeof *a->folders);
            uint32_t pack_index = 0;
            for (uint32_t f = 0; f < a->nfolders; f++) {
                folder *F = &a->folders[f];
                F->ncoders = (uint32_t)c_num(c);
                if (F->ncoders > MAX_CODERS) { snprintf(err, err_len, "7z folder with %u coders", F->ncoders); return -1; }
                uint32_t nin = 0, nout = 0;
                for (uint32_t k = 0; k < F->ncoders; k++) {
                    coder *C = &F->c[k];
                    uint8_t flags = c_byte(c);
                    C->idlen = flags & 0xF; if (C->idlen > 15) C->idlen = 15;
                    for (size_t i = 0; i < C->idlen; i++) C->id[i] = c_byte(c);
                    if (flags & 0x10) { C->nin = (uint32_t)c_num(c); C->nout = (uint32_t)c_num(c); } else C->nin = C->nout = 1;
                    if (flags & 0x20) { uint64_t np = c_num(c); if (np > sizeof C->props) { snprintf(err, err_len, "7z coder properties too long"); return -1; } for (uint64_t i = 0; i < np; i++) C->props[i] = c_byte(c); C->nprops = (size_t)np; }
                    if (flags & 0x80) { snprintf(err, err_len, "7z alternative coders are not supported"); return -1; }
                    nin += C->nin; nout += C->nout;
                }
                if (nin > MAX_STREAMS || nout > MAX_STREAMS || !nout) { snprintf(err, err_len, "7z folder stream counts"); return -1; }
                F->nbind = nout - 1;
                for (uint32_t i = 0; i < F->nbind; i++) { F->bind[i].in = (uint32_t)c_num(c); F->bind[i].out = (uint32_t)c_num(c); }
                F->npacked = nin - F->nbind;
                if (F->npacked == 1) {
                    for (uint32_t i = 0; i < nin; i++) { int bound = 0; for (uint32_t b = 0; b < F->nbind; b++) if (F->bind[b].in == i) bound = 1; if (!bound) { F->packed[0] = i; break; } }
                } else for (uint32_t i = 0; i < F->npacked; i++) F->packed[i] = (uint32_t)c_num(c);
                F->nunpack = nout; F->first_pack = pack_index; pack_index += F->npacked; F->nsub = 1;
            }
            if (c_byte(c) != kCodersUnPackSize) { snprintf(err, err_len, "7z header: unpack sizes expected"); return -1; }
            for (uint32_t f = 0; f < a->nfolders; f++) for (uint32_t i = 0; i < a->folders[f].nunpack; i++) a->folders[f].unpack[i] = c_num(c);
            for (;;) {
                uint8_t t = c_byte(c);
                if (t == kEnd || c->bad) break;
                if (t == kCRC) { uint32_t *crc = calloc(a->nfolders ? a->nfolders : 1, 4); uint8_t *def = calloc(a->nfolders ? a->nfolders : 1, 1); read_digests(c, a->nfolders, crc, def);
                                 for (uint32_t f = 0; f < a->nfolders; f++) { a->folders[f].crc = crc[f]; a->folders[f].has_crc = def[f]; } free(crc); free(def); }
                else c_skip(c, c_num(c));
            }
        } else if (id == kSubStreamsInfo) {
            uint8_t t = c_byte(c);
            if (t == kNumUnPackStream) { for (uint32_t f = 0; f < a->nfolders; f++) a->folders[f].nsub = (uint32_t)c_num(c); t = c_byte(c); }
            a->nsub = 0; for (uint32_t f = 0; f < a->nfolders; f++) a->nsub += a->folders[f].nsub;
            if (a->nsub > 1000000) { snprintf(err, err_len, "7z header: substream count"); return -1; }
            a->sub_size = calloc(a->nsub ? a->nsub : 1, sizeof *a->sub_size);
            a->sub_crc = calloc(a->nsub ? a->nsub : 1, 4); a->sub_crc_def = calloc(a->nsub ? a->nsub : 1, 1);
            uint32_t si = 0;
            for (uint32_t f = 0; f < a->nfolders; f++) {
                folder *F = &a->folders[f];
                if (!F->nsub) continue;
                uint64_t sum = 0, total = F->unpack[0];
                /* the folder's output is the unbound out stream */
                for (uint32_t o = 0; o < F->nunpack; o++) { int bound = 0; for (uint32_t b = 0; b < F->nbind; b++) if (F->bind[b].out == o) bound = 1; if (!bound) total = F->unpack[o]; }
                for (uint32_t k = 0; k + 1 < F->nsub; k++) { a->sub_size[si + k] = t == kSize ? c_num(c) : 0; sum += a->sub_size[si + k]; }
                a->sub_size[si + F->nsub - 1] = total >= sum ? total - sum : 0;
                si += F->nsub;
            }
            if (t == kSize) t = c_byte(c);
            /* CRCs for the streams whose folder does not already carry one */
            uint32_t need = 0;
            for (uint32_t f = 0; f < a->nfolders; f++) if (a->folders[f].nsub != 1 || !a->folders[f].has_crc) need += a->folders[f].nsub;
            for (;;) {
                if (t == kEnd || c->bad) break;
                if (t == kCRC) {
                    uint32_t *crc = calloc(need ? need : 1, 4); uint8_t *def = calloc(need ? need : 1, 1);
                    read_digests(c, need, crc, def);
                    uint32_t k = 0; si = 0;
                    for (uint32_t f = 0; f < a->nfolders; f++) {
                        folder *F = &a->folders[f];
                        if (F->nsub == 1 && F->has_crc) { a->sub_crc[si] = F->crc; a->sub_crc_def[si] = 1; }
                        else for (uint32_t j = 0; j < F->nsub; j++) { a->sub_crc[si + j] = crc[k]; a->sub_crc_def[si + j] = def[k]; k++; }
                        si += F->nsub;
                    }
                    free(crc); free(def);
                } else c_skip(c, c_num(c));
                t = c_byte(c);
            }
        } else { snprintf(err, err_len, "7z header: unexpected property %u", id); return -1; }
        if (c->bad) { snprintf(err, err_len, "7z header is truncated"); return -1; }
    }
}
/* substreams when the header carried no kSubStreamsInfo: one per folder */
static void default_substreams(archive *a) {
    if (a->sub_size || !a->nfolders) return;
    a->nsub = a->nfolders;
    a->sub_size = calloc(a->nsub, sizeof *a->sub_size); a->sub_crc = calloc(a->nsub, 4); a->sub_crc_def = calloc(a->nsub, 1);
    for (uint32_t f = 0; f < a->nfolders; f++) {
        folder *F = &a->folders[f]; uint64_t total = F->unpack[0];
        for (uint32_t o = 0; o < F->nunpack; o++) { int bound = 0; for (uint32_t b = 0; b < F->nbind; b++) if (F->bind[b].out == o) bound = 1; if (!bound) total = F->unpack[o]; }
        a->sub_size[f] = total; a->sub_crc[f] = F->crc; a->sub_crc_def[f] = (uint8_t)F->has_crc; F->nsub = 1;
    }
}
static void utf16_to_utf8(const uint8_t *p, size_t nunits, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < nunits && o + 4 < n; i++) {
        uint32_t c = (uint32_t)p[2 * i] | (uint32_t)p[2 * i + 1] << 8;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < nunits) { uint32_t lo = (uint32_t)p[2 * i + 2] | (uint32_t)p[2 * i + 3] << 8; if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i++; } }
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else { out[o++] = (char)(0xF0 | c >> 18); out[o++] = (char)(0x80 | ((c >> 12) & 0x3F)); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = 0;
}
static int read_files_info(cur *c, archive *a, char *err, size_t err_len) {
    a->nfiles = (uint32_t)c_num(c);
    if (a->nfiles > 1000000 || c->bad) { snprintf(err, err_len, "7z header: file count"); return -1; }
    a->names = calloc(a->nfiles ? a->nfiles : 1, sizeof *a->names);
    a->empty_stream = calloc(a->nfiles ? a->nfiles : 1, 1); a->empty_file = calloc(a->nfiles ? a->nfiles : 1, 1); a->is_dir = calloc(a->nfiles ? a->nfiles : 1, 1);
    uint32_t nempty = 0;
    for (;;) {
        uint8_t id = c_byte(c);
        if (id == kEnd || c->bad) break;
        uint64_t size = c_num(c);
        size_t end = c->pos + (size_t)size;
        if (size > c->n - c->pos) { snprintf(err, err_len, "7z header: property overruns"); return -1; }
        if (id == kEmptyStream) {
            uint8_t *v = c_bits(c, a->nfiles, 0);
            if (v) { memcpy(a->empty_stream, v, a->nfiles); free(v); }
            nempty = 0; for (uint32_t i = 0; i < a->nfiles; i++) nempty += a->empty_stream[i];
            /* a file with no stream is a directory unless said otherwise */
            for (uint32_t i = 0; i < a->nfiles; i++) a->is_dir[i] = a->empty_stream[i];
        } else if (id == kEmptyFile) {
            uint8_t *v = c_bits(c, nempty, 0);
            if (v) { uint32_t k = 0; for (uint32_t i = 0; i < a->nfiles; i++) if (a->empty_stream[i]) { a->empty_file[i] = v[k]; if (v[k]) a->is_dir[i] = 0; k++; } free(v); }
        } else if (id == kName) {
            if (c_byte(c) != 0) { snprintf(err, err_len, "7z header: external names are not supported"); return -1; }
            for (uint32_t i = 0; i < a->nfiles && c->pos < end; i++) {
                size_t start = c->pos, units = 0;
                while (c->pos + 1 < end && !(c->p[c->pos] == 0 && c->p[c->pos + 1] == 0)) { c->pos += 2; units++; }
                c->pos += 2;
                a->names[i] = malloc(3 * units + 4);
                if (a->names[i]) utf16_to_utf8(c->p + start, units, a->names[i], 3 * units + 4);
            }
        } else if (id == kWinAttributes) {
            uint8_t *def = c_bits(c, a->nfiles, 1);
            if (def) {
                if (c_byte(c) != 0) { free(def); snprintf(err, err_len, "7z header: external attributes"); return -1; }
                for (uint32_t i = 0; i < a->nfiles; i++) if (def[i]) { uint32_t attr = c_u32(c); if (attr & 0x10) a->is_dir[i] = 1; }
                free(def);
            }
        }
        c->pos = end;                                         /* whatever it was, it said how long */
    }
    for (uint32_t i = 0; i < a->nfiles; i++) if (!a->names[i]) { a->names[i] = malloc(24); if (a->names[i]) snprintf(a->names[i], 24, "unnamed_%u", i); }
    return c->bad ? (snprintf(err, err_len, "7z header is truncated"), -1) : 0;
}

/* ---- decoding a folder ----------------------------------------------------------- */
typedef struct { FILE *f; uint64_t base; const archive *a; } arch_ctx;
static uint64_t pack_offset(const arch_ctx *x, uint32_t pack_index) {
    uint64_t off = x->base + 32 + x->a->pack_pos;
    for (uint32_t i = 0; i < pack_index && i < x->a->npack; i++) off += x->a->pack_size[i];
    return off;
}
static src *make_out(const arch_ctx *x, const folder *F, uint32_t out_index, int depth, char *err, size_t err_len);
static src *make_in(const arch_ctx *x, const folder *F, uint32_t in_index, int depth, char *err, size_t err_len) {
    for (uint32_t b = 0; b < F->nbind; b++) if (F->bind[b].in == in_index) return make_out(x, F, F->bind[b].out, depth + 1, err, err_len);
    for (uint32_t p = 0; p < F->npacked; p++) if (F->packed[p] == in_index) {
        uint32_t pi = F->first_pack + p;
        if (pi >= x->a->npack) { snprintf(err, err_len, "7z folder refers to a missing packed stream"); return 0; }
        return file_src_new(x->f, pack_offset(x, pi), x->a->pack_size[pi]);
    }
    snprintf(err, err_len, "7z folder graph is inconsistent"); return 0;
}
static int id_is(const coder *C, const uint8_t *id, size_t n) { return C->idlen == n && !memcmp(C->id, id, n); }
static src *make_out(const arch_ctx *x, const folder *F, uint32_t out_index, int depth, char *err, size_t err_len) {
    if (depth > 8) { snprintf(err, err_len, "7z folder graph is too deep"); return 0; }
    uint32_t in_base = 0, out_base = 0;
    for (uint32_t k = 0; k < F->ncoders; k++) {
        const coder *C = &F->c[k];
        if (out_index >= out_base && out_index < out_base + C->nout) {
            uint64_t total = F->unpack[out_index];
            static const uint8_t ID_COPY[] = { 0x00 }, ID_LZMA[] = { 0x03, 0x01, 0x01 }, ID_LZMA2[] = { 0x21 }, ID_BCJ[] = { 0x03, 0x03, 0x01, 0x03 },
                                 ID_BCJ2[] = { 0x03, 0x03, 0x01, 0x1B }, ID_DELTA[] = { 0x03 }, ID_AES[] = { 0x06, 0xF1, 0x07, 0x01 }, ID_PPMD[] = { 0x03, 0x04, 0x01 };
            if (id_is(C, ID_AES, 4)) { snprintf(err, err_len, "the archive is encrypted: a password-protected 7z cannot be unpacked here"); return 0; }
            if (id_is(C, ID_PPMD, 3)) { snprintf(err, err_len, "the archive uses PPMd compression, which is not decoded here; repack it as LZMA2 (7-Zip's default)"); return 0; }
            if (id_is(C, ID_BCJ2, 4)) {
                if (C->nin != 4) { snprintf(err, err_len, "7z BCJ2 with %u inputs", C->nin); return 0; }
                src *in[4] = { 0, 0, 0, 0 };
                for (int i = 0; i < 4; i++) { in[i] = make_in(x, F, in_base + (uint32_t)i, depth, err, err_len); if (!in[i]) { for (int j = 0; j < i; j++) src_free(in[j]); return 0; } }
                return bcj2_new(in[0], in[1], in[2], in[3]);
            }
            if (C->nin != 1) { snprintf(err, err_len, "7z coder with %u inputs is not supported", C->nin); return 0; }
            src *in = make_in(x, F, in_base, depth, err, err_len);
            if (!in) return 0;
            src *r = 0;
            if (id_is(C, ID_COPY, 1)) return in;
            else if (id_is(C, ID_LZMA, 3)) r = lzma1_new(in, C->props, C->nprops, total, err, err_len);
            else if (id_is(C, ID_LZMA2, 1)) r = lzma2_new(in, C->props, C->nprops, total, err, err_len);
            else if (id_is(C, ID_BCJ, 4)) r = bcj_new(in);
            else if (id_is(C, ID_DELTA, 1)) r = delta_new(in, C->props, C->nprops);
            else {
                char idhex[40] = ""; for (size_t i = 0; i < C->idlen && i < 8; i++) snprintf(idhex + strlen(idhex), sizeof idhex - strlen(idhex), "%02x", C->id[i]);
                snprintf(err, err_len, "7z coder %s is not decoded here (LZMA, LZMA2, BCJ, BCJ2, Delta and stored are)", idhex);
            }
            if (!r) src_free(in);
            return r;
        }
        in_base += C->nin; out_base += C->nout;
    }
    snprintf(err, err_len, "7z folder has no such output"); return 0;
}
static src *folder_open(const arch_ctx *x, const folder *F, char *err, size_t err_len) {
    for (uint32_t o = 0; o < F->nunpack; o++) { int bound = 0; for (uint32_t b = 0; b < F->nbind; b++) if (F->bind[b].out == o) bound = 1; if (!bound) return make_out(x, F, o, 0, err, err_len); }
    snprintf(err, err_len, "7z folder has no output"); return 0;
}

/* ---- the archive ---------------------------------------------------------------- */
static const uint8_t SIG[6] = { '7', 'z', 0xBC, 0xAF, 0x27, 0x1C };
static int find_archive(FILE *f, uint64_t *base) {
    uint8_t h[32];
    if (fseeko(f, 0, SEEK_SET) || fread(h, 1, 32, f) != 32) return 0;
    if (!memcmp(h, SIG, 6)) { *base = 0; return 1; }
    if (h[0] != 'M' || h[1] != 'Z') return 0;
    /* a self-extractor: the archive follows the program, usually within a few MB */
    uint8_t *buf = malloc(1 << 20); if (!buf) return 0;
    uint64_t off = 0; size_t carry = 0; int found = 0;
    for (int blocks = 0; blocks < 96 && !found; blocks++) {
        if (fseeko(f, (off_t)off, SEEK_SET)) break;
        size_t n = fread(buf + carry, 1, (1 << 20) - carry, f);
        if (!n) break;
        size_t total = carry + n;
        for (size_t i = 0; i + 6 <= total; i++) if (buf[i] == '7' && !memcmp(buf + i, SIG, 6)) { *base = off - carry + i; found = 1; break; }
        carry = total >= 8 ? 8 : total; memmove(buf, buf + total - carry, carry);
        off += n;
    }
    free(buf);
    return found;
}
/* Read and parse the header; the archive is described in *a. */
static int open_archive(FILE *f, uint64_t base, archive *a, char *err, size_t err_len) {
    int rc = 0; uint8_t *hdr = 0, *dec = 0;
    memset(a, 0, sizeof *a);
    uint8_t sh[32];
    if (fseeko(f, (off_t)base, SEEK_SET) || fread(sh, 1, 32, f) != 32 || memcmp(sh, SIG, 6)) FAIL("not a 7z archive");
    uint64_t hoff = 0, hsize = 0; for (int i = 0; i < 8; i++) { hoff |= (uint64_t)sh[12 + i] << (8 * i); hsize |= (uint64_t)sh[20 + i] << (8 * i); }
    if (!hsize) FAIL("the 7z archive is empty");
    if (hsize > (64u << 20)) FAIL("7z header of %llu MB", (unsigned long long)(hsize >> 20));
    hdr = malloc((size_t)hsize); if (!hdr) FAIL("out of memory");
    if (fseeko(f, (off_t)(base + 32 + hoff), SEEK_SET) || fread(hdr, 1, (size_t)hsize, f) != hsize) FAIL("7z header is out of reach (truncated download?)");
    { uint32_t want = (uint32_t)sh[28] | (uint32_t)sh[29] << 8 | (uint32_t)sh[30] << 16 | (uint32_t)sh[31] << 24;
      if (crc32(0, hdr, (uInt)hsize) != want) FAIL("7z header CRC does not match: the file is damaged"); }
    for (int rounds = 0; rounds < 3; rounds++) {
        cur c = { hdr, (size_t)hsize, 0, 0 };
        uint8_t id = c_byte(&c);
        if (id == kHeader) {
            for (;;) {
                uint8_t t = c_byte(&c);
                if (t == kEnd || c.bad) break;
                if (t == kMainStreamsInfo) { if (read_streams_info(&c, a, err, err_len)) { rc = -1; goto done; } }
                else if (t == kFilesInfo) { default_substreams(a); if (read_files_info(&c, a, err, err_len)) { rc = -1; goto done; } }
                else if (t == kArchiveProperties) { for (;;) { uint8_t p = c_byte(&c); if (p == kEnd || c.bad) break; c_skip(&c, c_num(&c)); } }
                else if (t == kAdditionalStreamsInfo) FAIL("7z additional streams are not supported");
                else FAIL("7z header: unexpected section %u", t);
            }
            if (c.bad) FAIL("7z header is truncated");
            default_substreams(a);
            rc = 0; goto done;
        }
        if (id != kEncodedHeader) FAIL("7z header starts with %u", id);
        /* the header is itself a folder: decode it and go round again */
        archive ha; memset(&ha, 0, sizeof ha);
        if (read_streams_info(&c, &ha, err, err_len) || !ha.nfolders) { archive_free(&ha); FAIL("7z encoded header could not be read"); }
        arch_ctx x = { f, base, &ha };
        src *s = folder_open(&x, &ha.folders[0], err, err_len);
        if (!s) { archive_free(&ha); rc = -1; goto done; }
        uint64_t total = ha.folders[0].unpack[0];
        for (uint32_t o = 0; o < ha.folders[0].nunpack; o++) { int bound = 0; for (uint32_t b = 0; b < ha.folders[0].nbind; b++) if (ha.folders[0].bind[b].out == o) bound = 1; if (!bound) total = ha.folders[0].unpack[o]; }
        if (total > (64u << 20)) { src_free(s); archive_free(&ha); FAIL("7z encoded header too large"); }
        dec = malloc((size_t)total + 1);
        int got = dec ? src_read_all(s, dec, (size_t)total) : -1;
        if (got != (int)total) { snprintf(err, err_len, "7z encoded header failed to decode: %s", s->err[0] ? s->err : "short"); src_free(s); archive_free(&ha); rc = -1; goto done; }
        if (ha.folders[0].has_crc && crc32(0, dec, (uInt)total) != ha.folders[0].crc) { src_free(s); archive_free(&ha); FAIL("7z encoded header CRC does not match"); }
        src_free(s); archive_free(&ha);
        free(hdr); hdr = dec; dec = 0; hsize = total;
    }
    FAIL("7z header is nested too deep");
done:
    free(hdr); free(dec);
    if (rc) archive_free(a);
    return rc;
}

/* ---- the public face ---------------------------------------------------------------- */
int sz_is_7z(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    uint64_t base = 0; int r = find_archive(f, &base);
    fclose(f);
    return r;
}
int sz_is_rar(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    static const uint8_t RAR[6] = { 'R', 'a', 'r', '!', 0x1A, 0x07 };
    uint8_t buf[1 << 16]; int found = 0;
    size_t n = fread(buf, 1, sizeof buf, f);
    if (n >= 7 && !memcmp(buf, RAR, 6)) found = 1;
    else if (n > 2 && buf[0] == 'M' && buf[1] == 'Z') {
        /* a RAR self-extractor: the archive follows the stub */
        for (int blocks = 0; blocks < 64 && !found; blocks++) {
            for (size_t i = 0; i + 7 <= n; i++) if (buf[i] == 'R' && !memcmp(buf + i, RAR, 6)) { found = 1; break; }
            if (found) break;
            n = fread(buf, 1, sizeof buf, f); if (n < 7) break;
        }
    }
    fclose(f);
    return found;
}
uint64_t sz_uncompressed_total(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    uint64_t base = 0, total = 0; archive a; char err[128];
    if (find_archive(f, &base) && !open_archive(f, base, &a, err, sizeof err)) {
        for (uint32_t i = 0; i < a.nsub; i++) total += a.sub_size[i];
        archive_free(&a);
    }
    fclose(f);
    return total;
}
int sz_extract(const char *path, const char *dest_dir, uz_progress cb, void *ctx, int *skipped, char *err, size_t err_len) {
    int rc = 0, written = 0, skip = 0; FILE *f = 0; src *s = 0; uint8_t *buf = 0; archive a; memset(&a, 0, sizeof a);
    if (skipped) *skipped = 0;
    f = fopen(path, "rb");
    if (!f) FAIL("%s: %s", path, strerror(errno));
    uint64_t base = 0;
    if (!find_archive(f, &base)) FAIL("not a 7z archive");
    if (open_archive(f, base, &a, err, err_len)) { rc = -1; goto done; }
    buf = malloc(1 << 16); if (!buf) FAIL("out of memory");
    uint64_t total = 0, done_bytes = 0; for (uint32_t i = 0; i < a.nsub; i++) total += a.sub_size[i];
    arch_ctx x = { f, base, &a };
    uint32_t fi = 0, si = 0, sub_in_folder = 0; int folder_i = -1;
    for (uint32_t i = 0; i < a.nfiles; i++) {
        char name[2048], full[4096];
        if (!uz_safe_name(a.names[i], name, sizeof name)) { skip++; if (!a.empty_stream[i]) si++; continue; }
        if ((size_t)snprintf(full, sizeof full, "%s/%s", dest_dir, name) >= sizeof full) { skip++; if (!a.empty_stream[i]) si++; continue; }
        if (a.empty_stream[i]) {
            if (a.is_dir[i]) { if (uz_mkdirs(full)) skip++; }
            else { char *sl = strrchr(full, '/'); if (sl) { *sl = 0; uz_mkdirs(full); *sl = '/'; } FILE *g = fopen(full, "wb"); if (g) { fclose(g); written++; } else skip++; }
            continue;
        }
        /* the next substream: move to the folder that holds it */
        while (folder_i < 0 || sub_in_folder >= a.folders[folder_i].nsub) {
            folder_i++; sub_in_folder = 0;
            if ((uint32_t)folder_i >= a.nfolders) FAIL("7z: more files than streams");
            src_free(s); s = 0;
            if (a.folders[folder_i].nsub) { s = folder_open(&x, &a.folders[folder_i], err, err_len); if (!s) { rc = -1; goto done; } }
        }
        if (si >= a.nsub) FAIL("7z: substream index out of range");
        uint64_t size = a.sub_size[si];
        { char *sl = strrchr(full, '/'); if (sl) { *sl = 0; uz_mkdirs(full); *sl = '/'; } }
        FILE *g = fopen(full, "wb");
        if (!g) FAIL("cannot create %s: %s", name, strerror(errno));
        uint32_t crc = 0; uint64_t left = size; int bad = 0;
        while (left) {
            size_t want = left > (1 << 16) ? (1 << 16) : (size_t)left;
            int r = src_read_all(s, buf, want);
            if (r != (int)want) { fclose(g); FAIL("%s: %s", name, s->err[0] ? s->err : "the compressed data ended early"); }
            crc = (uint32_t)crc32(crc, buf, (uInt)want);
            if (fwrite(buf, 1, want, g) != want) { fclose(g); FAIL("cannot write %s: %s", name, strerror(errno)); }
            left -= want; done_bytes += want;
            if (cb && cb(ctx, name, done_bytes, total)) { fclose(g); FAIL("cancelled"); }
        }
        fclose(g);
        if (a.sub_crc_def[si] && crc != a.sub_crc[si]) { bad = 1; remove(full); skip++; }
        if (!bad) written++;
        (void)fi;
        si++; sub_in_folder++;
    }
done:
    src_free(s); free(buf); archive_free(&a);
    if (f) fclose(f);
    if (skipped) *skipped = skip;
    return rc ? -1 : written;
}
