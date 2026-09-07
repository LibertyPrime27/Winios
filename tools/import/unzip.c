/* See unzip.h. Deflate comes from zlib, which every platform this runs on
 * already ships; everything else is the container format.
 */
/* strdup and lstat are POSIX rather than C, so a strict -std=c11 build hides
 * them and the calls below silently become implicit declarations returning
 * int -- which on a 64-bit target is a pointer with its top half missing.
 * The default CMake build asks for gnu11 and never saw it; the aarch64
 * cross-check with -std=c11 did. */
#define _GNU_SOURCE
#include "unzip.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <zlib.h>

enum {
    SIG_LFH  = 0x04034b50u,   /* local file header      */
    SIG_CDH  = 0x02014b50u,   /* central directory entry */
    SIG_EOCD = 0x06054b50u,   /* end of central directory */
    SIG_Z64E = 0x06064b50u,   /* zip64 end of central directory */
    SIG_Z64L = 0x07064b50u,   /* zip64 EOCD locator     */
};
enum { EOCD_SCAN = 66u << 10 };   /* 64 KB comment field + the record itself */

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const unsigned char *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

int uz_mkdirs(const char *path) {
    char tmp[4096];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0777) && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0777) && errno != EEXIST) return -1;
    return 0;
}

int uz_safe_name(const char *raw, char *out, size_t n) {
    /* Component-wise, which is the only way to do this correctly: a filter
     * that merely looks for the substring ".." rejects "a..b" and accepts
     * "x/../../y" once the first pass has rewritten it. */
    const char *p = raw;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') p += 2;
    size_t o = 0;
    while (*p) {
        while (*p == '/' || *p == '\\') p++;          /* eat separators, incl. leading */
        const char *s = p;
        while (*p && *p != '/' && *p != '\\') p++;
        size_t len = (size_t)(p - s);
        if (len == 0) continue;
        if (len == 1 && s[0] == '.') continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') continue;   /* dropped, not rejected */
        if (o + len + 2 > n) return 0;
        if (o) out[o++] = '/';
        memcpy(out + o, s, len);
        o += len;
    }
    if (o == 0) return 0;
    out[o] = 0;
    return 1;
}

/* Find an EOCD by scanning backwards from the end. The record is 22 bytes
 * plus a comment of up to 64 KB, so that is how far back it can be.
 *
 * `skip` says how many candidates to pass over, because the first one found
 * is not always the right one: the signature can appear inside a comment, and
 * a comment comes *after* the record it belongs to, so a backwards scan meets
 * the decoy first. Two filters, in increasing strength:
 *
 *  - the declared comment length must reach exactly the end of the file,
 *    which a signature buried mid-comment fails;
 *  - and the caller checks whether a central directory can actually be found
 *    from what the record says, which is the only test a decoy cannot pass
 *    by accident. That is what `skip` is for.
 */
static long find_eocd_nth(FILE *f, unsigned char *buf, size_t *got,
                          long *scan_start, int skip) {
    if (fseek(f, 0, SEEK_END)) return -1;
    long end = ftell(f);
    if (end < 22) return -1;
    long start = end > (long)EOCD_SCAN ? end - (long)EOCD_SCAN : 0;
    if (fseek(f, start, SEEK_SET)) return -1;
    *got = fread(buf, 1, (size_t)(end - start), f);
    if (scan_start) *scan_start = start;

    int seen = 0;
    for (long i = (long)*got - 22; i >= 0; i--) {
        if (rd32(buf + i) != SIG_EOCD) continue;
        uint16_t clen = rd16(buf + i + 20);
        if (start + i + 22 + (long)clen != end) continue;
        if (seen++ == skip) return start + i;
    }
    /* Nothing left that validates. A truncated archive can still be worth
     * opening, so on the first pass fall back to the last signature and let
     * the caller's own checks fail rather than refusing a readable file. */
    if (skip == 0)
        for (long i = (long)*got - 22; i >= 0; i--)
            if (rd32(buf + i) == SIG_EOCD) return start + i;
    return -1;
}

static long find_eocd(FILE *f, unsigned char *buf, size_t *got, long *scan_start) {
    return find_eocd_nth(f, buf, got, scan_start, 0);
}

/* The central directory: what the archive says about itself, and where that
 * directory actually is.
 *
 * Those are two different questions and conflating them is a bug I had. The
 * recorded offsets are relative to where the *zip* starts, which for an
 * archive appended to an .exe is not where the file starts -- so a correction
 * is needed. Computing it as `eocd - cd_size` assumes the directory sits
 * immediately before the EOCD, which is true until the archive is zip64: then
 * the zip64 EOCD record (56 bytes) and its locator (20) sit in between, and
 * every local-header offset comes out 76 bytes wrong.
 *
 * So the position is not computed, it is *found*: try the recorded offset,
 * try the two places a directory could start given the trailer layout, and
 * accept whichever one actually has a directory entry at it. */
typedef struct {
    uint64_t nent, cd_size, cd_off;
    long     cd_real;      /* where the directory really begins */
    int64_t  delta;        /* add to a recorded offset to get a file offset */
} uz_dir;

/* An entry's zip64 extended information. The fields in it are in a fixed
 * order but only the ones that overflowed are present, so they have to be
 * consumed in sequence rather than indexed -- which is why this takes the
 * values it might replace and hands them back. */
static void zip64_entry(const unsigned char *ex, size_t elen,
                        uint64_t *usize, uint64_t *csize, uint64_t *lho) {
    size_t left = elen;
    while (left >= 4) {
        uint16_t id = rd16(ex), sz = rd16(ex + 2);
        if (4u + sz > left) return;
        if (id == 0x0001) {
            const unsigned char *q = ex + 4;
            size_t r = sz;
            if (usize && *usize == 0xFFFFFFFFu && r >= 8) { *usize = rd64(q); q += 8; r -= 8; }
            if (csize && *csize == 0xFFFFFFFFu && r >= 8) { *csize = rd64(q); q += 8; r -= 8; }
            if (lho   && *lho   == 0xFFFFFFFFu && r >= 8) { *lho   = rd64(q); }
            return;
        }
        ex += 4 + sz;
        left -= 4u + sz;
    }
}
/* The size alone, for the total: same walk, one field. */
static uint64_t zip64_usize(const unsigned char *ex, size_t elen, uint64_t usize) {
    zip64_entry(ex, elen, &usize, 0, 0);
    return usize;
}

static int has_sig_at(FILE *f, long at, uint32_t want) {
    unsigned char b[4];
    if (at < 0 || fseek(f, at, SEEK_SET)) return 0;
    if (fread(b, 1, 4, f) != 4) return 0;
    return rd32(b) == want;
}

/* One candidate EOCD, taken at its word. 0 if no directory can be found from
 * what it says -- which is how a decoy is rejected. */
static int read_dir_at(FILE *f, const unsigned char *scan, long scan_start,
                       long eocd_at, uz_dir *d) {
    const unsigned char *e = scan + (size_t)(eocd_at - scan_start);

    d->nent    = rd16(e + 10);
    d->cd_size = rd32(e + 12);
    d->cd_off  = rd32(e + 16);

    /* zip64, when any of those is saturated. The locator sits directly in
     * front of the EOCD and points at the real record. */
    long z64_at = -1;
    if (d->nent == 0xFFFFu || d->cd_size == 0xFFFFFFFFu || d->cd_off == 0xFFFFFFFFu) {
        if (eocd_at - scan_start >= 20 && rd32(e - 20) == SIG_Z64L) {
            uint64_t z64 = rd64(e - 20 + 8);
            unsigned char z[56];
            if (!fseek(f, (long)z64, SEEK_SET) && fread(z, 1, sizeof z, f) == sizeof z
                && rd32(z) == SIG_Z64E) {
                z64_at     = (long)z64;
                d->nent    = rd64(z + 32);
                d->cd_size = rd64(z + 40);
                d->cd_off  = rd64(z + 48);
            }
        }
        /* The locator's own offset is relative to the zip too, so for an
         * appended archive it will not resolve. Then the record is 20 bytes
         * before the EOCD by definition, which does not need an offset. */
        if (z64_at < 0 && has_sig_at(f, eocd_at - 20 - 56, SIG_Z64E)) {
            unsigned char z[56];
            if (!fseek(f, eocd_at - 20 - 56, SEEK_SET) && fread(z, 1, sizeof z, f) == sizeof z) {
                z64_at     = eocd_at - 20 - 56;
                d->nent    = rd64(z + 32);
                d->cd_size = rd64(z + 40);
                d->cd_off  = rd64(z + 48);
            }
        }
    }
    /* No directory to find. Not a success even when the record claims zero
     * entries: a decoy record buried in a comment claims exactly that, and
     * accepting it would report a real archive as empty. The caller tries the
     * next candidate, and only if every one of them is empty does it treat
     * the file as an empty archive. */
    if (!d->cd_size || d->cd_size > (256u << 20)) { d->cd_real = -1; return 0; }

    /* Candidates, best first: the offset as recorded (an ordinary zip), the
     * space just before the trailer (an appended archive), and the same
     * allowing for the zip64 records when they are there. */
    long cands[3];
    int n = 0;
    cands[n++] = (long)d->cd_off;
    if (z64_at >= 0) cands[n++] = z64_at - (long)d->cd_size;
    cands[n++] = eocd_at - (long)d->cd_size;
    for (int i = 0; i < n; i++) {
        if (!has_sig_at(f, cands[i], SIG_CDH)) continue;
        d->cd_real = cands[i];
        d->delta = (int64_t)cands[i] - (int64_t)d->cd_off;
        return 1;
    }
    return 0;
}

/* Try each EOCD candidate, nearest the end first, and keep the one a
 * directory can actually be reached from. A record whose comment happens to
 * contain the signature describes nothing, so it fails here and the scan
 * carries on -- rather than the archive being reported as empty, which is
 * what a single-candidate reader does and is indistinguishable from an
 * archive that really is empty. */
static int read_dir(FILE *f, unsigned char *scan, uz_dir *d) {
    uz_dir first;
    int have_first = 0;
    for (int skip = 0; skip < 8; skip++) {
        size_t got = 0;
        long scan_start = 0;
        long at = find_eocd_nth(f, scan, &got, &scan_start, skip);
        if (at < 0) break;
        uz_dir cand;
        memset(&cand, 0, sizeof cand);
        if (read_dir_at(f, scan, scan_start, at, &cand)) { *d = cand; return 1; }
        if (!have_first) { first = cand; have_first = 1; }
    }
    /* Nothing yielded a directory. Hand back what the first candidate said so
     * the caller can tell "an empty archive" from "not an archive". */
    if (have_first) *d = first;
    return 0;
}

int uz_is_zip(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char *buf = (unsigned char *)malloc(EOCD_SCAN);
    if (!buf) { fclose(f); return 0; }
    size_t got = 0;
    long at = find_eocd(f, buf, &got, 0);
    free(buf);
    fclose(f);
    return at >= 0;
}

/* Walk the central directory adding up the uncompressed sizes. The same
 * offset arithmetic as the extractor, including the self-extractor
 * correction, because a total computed from a directory read at the wrong
 * place would be worse than no total at all. */
uint64_t uz_uncompressed_total(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char *scan = (unsigned char *)malloc(EOCD_SCAN);
    if (!scan) { fclose(f); return 0; }

    uint64_t total = 0;
    uz_dir d;
    memset(&d, 0, sizeof d);
    if (read_dir(f, scan, &d) && d.nent && d.cd_size) {
        unsigned char *cd = (unsigned char *)malloc((size_t)d.cd_size);
        if (cd && !fseek(f, d.cd_real, SEEK_SET)
            && fread(cd, 1, (size_t)d.cd_size, f) == d.cd_size) {
            size_t p = 0;
            for (uint64_t i = 0; i < d.nent; i++) {
                if (p + 46 > d.cd_size || rd32(cd + p) != SIG_CDH) break;
                uint64_t usize = rd32(cd + p + 24);
                uint16_t nlen = rd16(cd + p + 28), elen = rd16(cd + p + 30),
                         clen = rd16(cd + p + 32);
                if (p + 46 + nlen + elen + clen > d.cd_size) break;
                if (usize == 0xFFFFFFFFu) usize = zip64_usize(cd + p + 46 + nlen, elen, usize);
                total += usize;
                p += 46u + nlen + elen + clen;
            }
        }
        free(cd);
    }
    free(scan);
    fclose(f);
    return total;
}

#define FAIL(...) do { if (err) snprintf(err, err_len, __VA_ARGS__); goto fail; } while (0)

int uz_extract(const char *zip_path, const char *dest_dir,
               uz_progress cb, void *ctx,
               int *skipped, char *err, size_t err_len) {
    FILE *f = 0;
    unsigned char *scan = 0, *in = 0, *out = 0;
    int written = 0, skip = 0;
    if (skipped) *skipped = 0;

    f = fopen(zip_path, "rb");
    if (!f) FAIL("%s: %s", zip_path, strerror(errno));

    scan = (unsigned char *)malloc(EOCD_SCAN);
    if (!scan) FAIL("out of memory");

    /* Where the directory is and how to correct its offsets: one place, so
     * the extractor and the size estimate cannot disagree about it. */
    uz_dir d;
    memset(&d, 0, sizeof d);
    if (!read_dir(f, scan, &d)) {
        if (d.nent == 0 && d.cd_size == 0) { free(scan); fclose(f); return 0; }
        FAIL("no readable zip central directory (not a zip, or truncated)");
    }
    if (!d.nent) { free(scan); fclose(f); return 0; }   /* an empty archive is not an error */

    unsigned char *cd = (unsigned char *)malloc((size_t)d.cd_size);
    if (!cd) FAIL("out of memory reading the central directory");
    if (fseek(f, d.cd_real, SEEK_SET) || fread(cd, 1, (size_t)d.cd_size, f) != d.cd_size) {
        free(cd); FAIL("could not read the central directory");
    }
    const uint64_t nent = d.nent, cd_size = d.cd_size;
    const int64_t delta = d.delta;

    in  = (unsigned char *)malloc(1u << 16);
    out = (unsigned char *)malloc(1u << 16);
    if (!in || !out) { free(cd); FAIL("out of memory"); }

    size_t p = 0;
    for (uint64_t i = 0; i < nent; i++) {
        if (p + 46 > cd_size || rd32(cd + p) != SIG_CDH) break;
        uint16_t method = rd16(cd + p + 10);
        uint64_t csize  = rd32(cd + p + 20);
        uint64_t usize  = rd32(cd + p + 24);
        uint16_t nlen   = rd16(cd + p + 28);
        uint16_t elen   = rd16(cd + p + 30);
        uint16_t clen   = rd16(cd + p + 32);
        uint64_t lho    = rd32(cd + p + 42);
        if (p + 46 + nlen + elen + clen > cd_size) break;
        const char *raw = (const char *)cd + p + 46;

        /* zip64 extended information, if any of the 32-bit fields was
         * saturated. Shared with the size estimate, so the two cannot read
         * the same extra field differently. */
        if (usize == 0xFFFFFFFFu || csize == 0xFFFFFFFFu || lho == 0xFFFFFFFFu)
            zip64_entry(cd + p + 46 + nlen, elen, &usize, &csize, &lho);
        p += 46u + nlen + elen + clen;

        char name[2048];
        { char tmp[1024];
          size_t take = nlen < sizeof tmp - 1 ? nlen : sizeof tmp - 1;
          memcpy(tmp, raw, take); tmp[take] = 0;
          if (!uz_safe_name(tmp, name, sizeof name)) { skip++; continue; } }

        char full[4096];
        if ((size_t)snprintf(full, sizeof full, "%s/%s", dest_dir, name) >= sizeof full) { skip++; continue; }

        int is_dir = nlen && (raw[nlen - 1] == '/' || raw[nlen - 1] == '\\');
        if (is_dir || (usize == 0 && csize == 0 && method == 0)) {
            /* A zero-length entry is ambiguous: a directory entry without the
             * trailing slash and an empty file look the same. Treat a name
             * with no extension as a directory and anything else as a file,
             * which is right far more often than either choice alone. */
            if (is_dir || !strrchr(name, '.')) { uz_mkdirs(full); continue; }
        }
        { char *slash = strrchr(full, '/');
          if (slash) { *slash = 0; if (uz_mkdirs(full)) { skip++; continue; } *slash = '/'; } }

        if (method != 0 && method != 8) { skip++; continue; }   /* not stored, not deflate */

        if (fseek(f, (long)((int64_t)lho + delta), SEEK_SET)) { skip++; continue; }
        unsigned char lfh[30];
        if (fread(lfh, 1, sizeof lfh, f) != sizeof lfh || rd32(lfh) != SIG_LFH) { skip++; continue; }
        /* The local header repeats the name and extra fields, and its extra
         * field is routinely a different length from the central one, so the
         * data offset must be computed from the local header and not from the
         * central directory's idea of it. */
        if (fseek(f, (long)((int64_t)lho + delta + 30 + rd16(lfh + 26) + rd16(lfh + 28)), SEEK_SET)) { skip++; continue; }

        FILE *g = fopen(full, "wb");
        if (!g) { skip++; continue; }

        int bad = 0;
        if (method == 0) {
            uint64_t left = csize ? csize : usize;
            while (left) {
                size_t want = left > (1u << 16) ? (1u << 16) : (size_t)left;
                size_t got = fread(in, 1, want, f);
                if (!got) { bad = 1; break; }
                if (fwrite(in, 1, got, g) != got) { bad = 1; break; }
                left -= got;
            }
        } else {
            z_stream z; memset(&z, 0, sizeof z);
            /* -15: raw deflate. A zip member has no zlib header of its own. */
            if (inflateInit2(&z, -15) != Z_OK) { bad = 1; }
            else {
                uint64_t left = csize;
                int done = 0;
                while (!done) {
                    if (z.avail_in == 0) {
                        size_t want = left > (1u << 16) ? (1u << 16) : (size_t)left;
                        if (!want) { bad = 1; break; }
                        size_t got = fread(in, 1, want, f);
                        if (!got) { bad = 1; break; }
                        left -= got;
                        z.next_in = in; z.avail_in = (unsigned)got;
                    }
                    z.next_out = out; z.avail_out = 1u << 16;
                    int r = inflate(&z, Z_NO_FLUSH);
                    size_t have = (1u << 16) - z.avail_out;
                    if (have && fwrite(out, 1, have, g) != have) { bad = 1; break; }
                    if (r == Z_STREAM_END) done = 1;
                    else if (r != Z_OK) { bad = 1; break; }
                }
                inflateEnd(&z);
            }
        }
        fclose(g);
        if (bad) { remove(full); skip++; continue; }
        written++;
        if (cb && cb(ctx, name, (uint64_t)written, nent)) {
            if (err) snprintf(err, err_len, "cancelled");
            free(cd); free(in); free(out); free(scan); fclose(f);
            if (skipped) *skipped = skip;
            return written;
        }
    }

    free(cd); free(in); free(out); free(scan); fclose(f);
    if (skipped) *skipped = skip;
    return written;

fail:
    free(scan); free(in); free(out);
    if (f) fclose(f);
    return -1;
}
