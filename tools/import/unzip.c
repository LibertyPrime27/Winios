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

/* Find the EOCD by scanning backwards from the end. The record is 22 bytes
 * plus a comment of up to 64 KB, so that is how far back it can be. */
static long find_eocd(FILE *f, unsigned char *buf, size_t *got) {
    if (fseek(f, 0, SEEK_END)) return -1;
    long end = ftell(f);
    if (end < 22) return -1;
    long start = end > (long)EOCD_SCAN ? end - (long)EOCD_SCAN : 0;
    if (fseek(f, start, SEEK_SET)) return -1;
    *got = fread(buf, 1, (size_t)(end - start), f);
    for (long i = (long)*got - 22; i >= 0; i--)
        if (rd32(buf + i) == SIG_EOCD) return start + i;
    return -1;
}

int uz_is_zip(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char *buf = (unsigned char *)malloc(EOCD_SCAN);
    if (!buf) { fclose(f); return 0; }
    size_t got = 0;
    long at = find_eocd(f, buf, &got);
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
    size_t sgot = 0;
    long eocd_at = find_eocd(f, scan, &sgot);
    uint64_t total = 0;
    if (eocd_at >= 0 && fseek(f, 0, SEEK_END) == 0) {
        long scan_start = ftell(f) - (long)sgot;
        const unsigned char *e = scan + (size_t)(eocd_at - scan_start);
        uint64_t nent = rd16(e + 10), cd_size = rd32(e + 12), cd_off = rd32(e + 16);
        if (nent == 0xFFFFu || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {
            if (eocd_at - scan_start >= 20 && rd32(e - 20) == SIG_Z64L) {
                uint64_t z64 = rd64(e - 20 + 8);
                unsigned char z[56];
                if (!fseek(f, (long)z64, SEEK_SET) && fread(z, 1, sizeof z, f) == sizeof z
                    && rd32(z) == SIG_Z64E) {
                    nent = rd64(z + 32); cd_size = rd64(z + 40);
                }
            }
        }
        if (nent && cd_size && cd_size <= (256u << 20)) {
            unsigned char *cd = (unsigned char *)malloc((size_t)cd_size);
            if (cd) {
                long cd_real = eocd_at - (long)cd_size;
                if (!fseek(f, cd_real, SEEK_SET) && fread(cd, 1, (size_t)cd_size, f) == cd_size) {
                    size_t p2 = 0;
                    for (uint64_t i = 0; i < nent; i++) {
                        if (p2 + 46 > cd_size || rd32(cd + p2) != SIG_CDH) break;
                        uint64_t usize = rd32(cd + p2 + 24);
                        uint16_t nlen = rd16(cd + p2 + 28), elen = rd16(cd + p2 + 30),
                                 clen = rd16(cd + p2 + 32);
                        if (usize == 0xFFFFFFFFu) {
                            const unsigned char *ex = cd + p2 + 46 + nlen;
                            size_t left = elen;
                            while (left >= 4) {
                                uint16_t id = rd16(ex), sz = rd16(ex + 2);
                                if (4u + sz > left) break;
                                if (id == 0x0001 && sz >= 8) { usize = rd64(ex + 4); break; }
                                ex += 4 + sz; left -= 4u + sz;
                            }
                        }
                        total += usize;
                        p2 += 46u + nlen + elen + clen;
                    }
                }
                free(cd);
            }
        }
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
    size_t sgot = 0;
    long eocd_at = find_eocd(f, scan, &sgot);
    if (eocd_at < 0) FAIL("no zip central directory found (not a zip?)");

    /* `scan` holds the tail of the file, starting at end - sgot, so the EOCD
     * record sits at its file offset minus that start. */
    if (fseek(f, 0, SEEK_END)) FAIL("seek failed");
    long scan_start = ftell(f) - (long)sgot;
    const unsigned char *e = scan + (size_t)(eocd_at - scan_start);

    uint64_t nent    = rd16(e + 10);
    uint64_t cd_size = rd32(e + 12);
    uint64_t cd_off  = rd32(e + 16);

    /* zip64, when any of those fields is saturated. The locator sits directly
     * in front of the EOCD and points at the real record. */
    if (nent == 0xFFFFu || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {
        if (eocd_at - scan_start >= 20 && rd32(e - 20) == SIG_Z64L) {
            uint64_t z64 = rd64(e - 20 + 8);
            unsigned char z[56];
            if (!fseek(f, (long)z64, SEEK_SET) && fread(z, 1, sizeof z, f) == sizeof z
                && rd32(z) == SIG_Z64E) {
                nent    = rd64(z + 32);
                cd_size = rd64(z + 40);
                cd_off  = rd64(z + 48);
            }
        }
    }
    if (!nent) { free(scan); fclose(f); return 0; }        /* an empty archive is not an error */
    if (cd_size > (256u << 20)) FAIL("central directory implausibly large (%llu bytes)",
                                     (unsigned long long)cd_size);

    unsigned char *cd = (unsigned char *)malloc((size_t)cd_size);
    if (!cd) FAIL("out of memory reading the central directory");
    /* Where the directory really is, versus where the archive says it is.
     * For a plain zip these agree; for one appended to an .exe every recorded
     * offset is short by the size of the loader in front, and this is that
     * amount. Trusting the recorded offset instead is how an SFX reader ends
     * up inflating the middle of a PE section. */
    long cd_real = eocd_at - (long)cd_size;
    long delta = cd_real - (long)cd_off;
    if (fseek(f, cd_real, SEEK_SET) || fread(cd, 1, (size_t)cd_size, f) != cd_size) {
        free(cd); FAIL("could not read the central directory");
    }

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

        /* zip64 extended information, if the 32-bit fields were saturated.
         * The order of the fields in it is fixed but which are present depends
         * on which of the outer ones overflowed, so they have to be consumed
         * in order rather than indexed. */
        if (usize == 0xFFFFFFFFu || csize == 0xFFFFFFFFu || lho == 0xFFFFFFFFu) {
            const unsigned char *ex = cd + p + 46 + nlen;
            size_t left = elen;
            while (left >= 4) {
                uint16_t id = rd16(ex), sz = rd16(ex + 2);
                if (4u + sz > left) break;
                if (id == 0x0001) {
                    const unsigned char *q = ex + 4; size_t r = sz;
                    if (usize == 0xFFFFFFFFu && r >= 8) { usize = rd64(q); q += 8; r -= 8; }
                    if (csize == 0xFFFFFFFFu && r >= 8) { csize = rd64(q); q += 8; r -= 8; }
                    if (lho   == 0xFFFFFFFFu && r >= 8) { lho   = rd64(q); }
                    break;
                }
                ex += 4 + sz; left -= 4u + sz;
            }
        }
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
