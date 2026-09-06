/* msvcrt.dll -- the C runtime a mingw-w64 (and MSVC) program imports.
 *
 * The CRT's startup (`__getmainargs`, `_initterm`, `__set_app_type`...),
 * memory (`malloc` on the process heap), the string functions on guest
 * memory, and stdio on the three standard handles. printf-family formatting
 * cannot go to the host's vsnprintf -- the arguments live in guest memory,
 * in the guest's calling convention -- so `gfmt()` walks the format string
 * itself, pulls each argument from the guest va_list (8-byte slots on x64,
 * 4-byte slots with 8 for doubles and 64-bit integers on x86) and formats the
 * single conversion through the host's snprintf. That handles the msvcrt
 * extensions (%I64d, %I32u) and the bit that matters for games: msvcrt's
 * exponent format ("1e+005" three digits on the classic CRT) is *not*
 * reproduced -- mingw programs use their own __mingw_ printf for that.
 */
#define _GNU_SOURCE
#include "w32.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* ---- guest stdio state ---- */
static uint64_t g_iob;          /* FILE[3] in guest memory (_iob / __iob_func) */
static uint64_t g_errno;        /* int in guest memory */
static uint64_t g_lconv;        /* struct lconv */
static uint64_t g_argv, g_envp; /* what __getmainargs handed out */
static uint64_t g_onexit[128]; static int g_nonexit;

static int file_size(w32 *w) { return w->is32 ? 32 : 48; }
static int file_off(w32 *w) { return w->is32 ? 16 : 28; }      /* offsetof(FILE, _file) */
static void ensure_state(w32 *w) {
    if (g_iob) return;
    /* `_iob` may also be imported as data (x86 mingw): the same array either way */
    g_iob = w32_stub_for(w, "msvcrt.dll", "_iob");
    for (int i = 0; i < 3; i++) w32_write(w, g_iob + (uint64_t)file_size(w) * i + (uint64_t)file_off(w), 4, (uint64_t)i);   /* _file */
    w32_write(w, w32_stub_for(w, "msvcrt.dll", "__mb_cur_max"), 4, 1);
    g_errno = w32_heap_alloc(w, 4);
    /* struct lconv: decimal_point, thousands_sep, grouping, ... (pointers) */
    g_lconv = w32_heap_alloc(w, 24 * 8);
    int psz = (int)w32_ptrsize(w);
    uint64_t dot = w32_strdup(w, "."), empty = w32_strdup(w, "");
    w32_write(w, g_lconv, psz, dot);
    for (int i = 1; i < 6; i++) w32_write(w, g_lconv + (uint64_t)psz * i, psz, empty);
}
/* which fd is this FILE*? -1 if not one of ours */
static int file_fd(w32 *w, uint64_t fp) {
    ensure_state(w);
    if (fp < g_iob || fp >= g_iob + 3u * (uint64_t)file_size(w)) return -1;
    return (int)((fp - g_iob) / (uint64_t)file_size(w));
}
static void set_errno(w32 *w, int e) { ensure_state(w); w32_write(w, g_errno, 4, (uint32_t)e); }

/* ---- printf engine over a guest va_list ---- */
typedef struct { w32 *w; uint64_t ap; } gva;
static uint64_t va_int(gva *v, int bytes) {
    uint64_t r;
    if (v->w->is32) { r = w32_read(v->w, v->ap, bytes == 8 ? 8 : 4); v->ap += bytes == 8 ? 8 : 4; }
    else { r = w32_read(v->w, v->ap, 8); v->ap += 8; }
    return r;
}
static double va_dbl(gva *v) { uint64_t b = va_int(v, 8); double d; memcpy(&d, &b, 8); return d; }

typedef struct { char *buf; size_t cap, len; int fd; w32 *w; } out_t;
static void out_write(out_t *o, const char *s, size_t n) {
    if (o->buf) { size_t room = o->len < o->cap ? o->cap - o->len : 0; size_t k = n < room ? n : room; memcpy(o->buf + o->len, s, k); }
    else if (o->fd >= 0) { if (write(o->fd, s, n) < 0) { /* ignore */ } }
    o->len += n;
}

/* Format `fmt` with arguments from `ap`; wide=1 when the format is UTF-16.
 * Returns the number of characters that would have been written. */
static int gfmt(w32 *w, out_t *o, const char *fmt, const uint16_t *wfmt, uint64_t ap) {
    gva v = { w, ap };
    char spec[32], tmp[512];
    size_t i = 0;
    for (;;) {
        uint32_t c = wfmt ? wfmt[i] : (uint8_t)fmt[i];
        if (!c) break;
        if (c != '%') {
            /* copy a literal run */
            size_t j = i;
            while ((wfmt ? wfmt[j] : (uint8_t)fmt[j]) && (wfmt ? wfmt[j] : (uint8_t)fmt[j]) != '%') j++;
            if (wfmt) { for (size_t k = i; k < j; k++) { char ch = wfmt[k] < 128 ? (char)wfmt[k] : '?'; out_write(o, &ch, 1); } }
            else out_write(o, fmt + i, j - i);
            i = j; continue;
        }
        i++;
        size_t sp = 0; spec[sp++] = '%';
        /* flags */
        for (;;) { c = wfmt ? wfmt[i] : (uint8_t)fmt[i]; if (c == '-' || c == '+' || c == ' ' || c == '#' || c == '0') { if (sp < 20) spec[sp++] = (char)c; i++; } else break; }
        /* width */
        int width = -1;
        if (c == '*') { width = (int)(int32_t)va_int(&v, 4); i++; c = wfmt ? wfmt[i] : (uint8_t)fmt[i]; }
        else { while (c >= '0' && c <= '9') { width = (width < 0 ? 0 : width) * 10 + (int)(c - '0'); i++; c = wfmt ? wfmt[i] : (uint8_t)fmt[i]; } }
        if (width >= 0) sp += (size_t)snprintf(spec + sp, sizeof spec - sp, "%d", width);
        /* precision */
        int prec = -1;
        if (c == '.') {
            i++; c = wfmt ? wfmt[i] : (uint8_t)fmt[i]; prec = 0;
            if (c == '*') { prec = (int)(int32_t)va_int(&v, 4); i++; c = wfmt ? wfmt[i] : (uint8_t)fmt[i]; }
            else while (c >= '0' && c <= '9') { prec = prec * 10 + (int)(c - '0'); i++; c = wfmt ? wfmt[i] : (uint8_t)fmt[i]; }
            sp += (size_t)snprintf(spec + sp, sizeof spec - sp, ".%d", prec < 0 ? 0 : prec);
        }
        /* length */
        int bytes = 4, wide = 0;
        for (;;) {
            c = wfmt ? wfmt[i] : (uint8_t)fmt[i];
            if (c == 'h') { i++; if ((wfmt ? wfmt[i] : (uint8_t)fmt[i]) == 'h') i++; bytes = 4; continue; }
            if (c == 'l') { i++; if ((wfmt ? wfmt[i] : (uint8_t)fmt[i]) == 'l') { i++; bytes = 8; } else { bytes = 4; wide = 1; } continue; }
            if (c == 'I') { i++; c = wfmt ? wfmt[i] : (uint8_t)fmt[i];
                            if (c == '6' && (wfmt ? wfmt[i + 1] : (uint8_t)fmt[i + 1]) == '4') { i += 2; bytes = 8; }
                            else if (c == '3' && (wfmt ? wfmt[i + 1] : (uint8_t)fmt[i + 1]) == '2') { i += 2; bytes = 4; }
                            else bytes = (int)w32_ptrsize(w);
                            continue; }
            if (c == 'z' || c == 't' || c == 'j') { i++; bytes = c == 'j' ? 8 : (int)w32_ptrsize(w); continue; }
            if (c == 'L' || c == 'q') { i++; bytes = 8; continue; }
            if (c == 'w') { i++; wide = 1; continue; }
            break;
        }
        c = wfmt ? wfmt[i] : (uint8_t)fmt[i]; i++;
        int n = 0;
        switch (c) {
        case 'd': case 'i': {
            int64_t val = bytes == 8 ? (int64_t)va_int(&v, 8) : (int32_t)va_int(&v, 4);
            snprintf(spec + sp, sizeof spec - sp, "lld"); n = snprintf(tmp, sizeof tmp, spec, (long long)val); break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t val = bytes == 8 ? va_int(&v, 8) : (uint32_t)va_int(&v, 4);
            snprintf(spec + sp, sizeof spec - sp, "ll%c", (char)c); n = snprintf(tmp, sizeof tmp, spec, (unsigned long long)val); break;
        }
        case 'p': {
            uint64_t val = va_int(&v, (int)w32_ptrsize(w));
            n = snprintf(tmp, sizeof tmp, w->is32 ? "%08llX" : "%016llX", (unsigned long long)val); break;
        }
        case 'c': {
            uint32_t ch = (uint32_t)va_int(&v, 4);
            spec[sp] = 'c'; spec[sp + 1] = 0; n = snprintf(tmp, sizeof tmp, spec, (int)(ch < 256 ? ch : '?')); break;
        }
        case 's': {
            uint64_t p = va_int(&v, (int)w32_ptrsize(w));
            const char *s; char wbuf[512];
            if (!p) s = "(null)";
            else if (wide ^ (wfmt != 0)) { w32_wtoa(w, p, wbuf, sizeof wbuf); s = wbuf; }
            else s = GSTR(p);
            spec[sp] = 's'; spec[sp + 1] = 0;
            n = snprintf(tmp, sizeof tmp, spec, s);
            if (n >= (int)sizeof tmp) { out_write(o, s, strlen(s)); n = (int)strlen(s); continue; }   /* long strings: unpadded */
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d = va_dbl(&v);
            spec[sp] = (char)c; spec[sp + 1] = 0; n = snprintf(tmp, sizeof tmp, spec, d); break;
        }
        case 'n': { uint64_t p = va_int(&v, (int)w32_ptrsize(w)); w32_write(w, p, 4, (uint64_t)o->len); continue; }
        case '%': tmp[0] = '%'; n = 1; break;
        default:  tmp[0] = '%'; tmp[1] = (char)c; n = 2; break;
        }
        if (n > (int)sizeof tmp - 1) n = (int)sizeof tmp - 1;
        out_write(o, tmp, (size_t)n);
    }
    return (int)o->len;
}

/* ---- startup ---- */
static void m___set_app_type(w32 *w) { (void)w; }
static void build_args(w32 *w) {
    ensure_state(w);
    int psz = (int)w32_ptrsize(w);
    if (!g_argv) {
        g_argv = w32_heap_alloc(w, (uint64_t)psz * (w->argc + 1));
        for (int i = 0; i < w->argc; i++) {
            char buf[4096];
            if (i == 0) { const char *slash = strrchr(w->argv[0], '/'); snprintf(buf, sizeof buf, "C:\\xcore\\%s", slash ? slash + 1 : w->argv[0]); }
            else snprintf(buf, sizeof buf, "%s", w->argv[i]);
            w32_write(w, g_argv + (uint64_t)psz * i, psz, w32_strdup(w, buf));
        }
        /* environment as an array of pointers into the block */
        int n = 0; const char *e = W32P(w, w->env_block); for (const char *p = e; *p; p += strlen(p) + 1) n++;
        g_envp = w32_heap_alloc(w, (uint64_t)psz * (n + 1));
        int k = 0; for (uint64_t p = w->env_block; *(const char *)W32P(w, p); p += strlen(W32P(w, p)) + 1) w32_write(w, g_envp + (uint64_t)psz * k++, psz, p);
        /* the data exports some CRTs read directly */
        w32_write(w, w32_stub_for(w, "msvcrt.dll", "__argc"), 4, (uint64_t)w->argc);
        w32_write(w, w32_stub_for(w, "msvcrt.dll", "__argv"), psz, g_argv);
        w32_write(w, w32_stub_for(w, "msvcrt.dll", "_environ"), psz, g_envp);
        w32_write(w, w32_stub_for(w, "msvcrt.dll", "__initenv"), psz, g_envp);
    }
}
static void m___getmainargs(w32 *w) {
    /* (int *argc, char ***argv, char ***env, int wildcard, _startupinfo *si) */
    build_args(w);
    int psz = (int)w32_ptrsize(w);
    w32_write(w, ARG(0), 4, (uint64_t)w->argc);
    w32_write(w, ARG(1), psz, g_argv);
    w32_write(w, ARG(2), psz, g_envp);
    RET(0);
}
static void m___wgetmainargs(w32 *w) {
    ensure_state(w);
    int psz = (int)w32_ptrsize(w);
    uint64_t argv = w32_heap_alloc(w, (uint64_t)psz * (w->argc + 1));
    for (int i = 0; i < w->argc; i++) {
        char buf[4096];
        if (i == 0) { const char *slash = strrchr(w->argv[0], '/'); snprintf(buf, sizeof buf, "C:\\xcore\\%s", slash ? slash + 1 : w->argv[0]); }
        else snprintf(buf, sizeof buf, "%s", w->argv[i]);
        w32_write(w, argv + (uint64_t)psz * i, psz, w32_wstrdup(w, buf));
    }
    uint64_t envp = w32_heap_alloc(w, (uint64_t)psz * 8);
    w32_write(w, ARG(0), 4, (uint64_t)w->argc); w32_write(w, ARG(1), psz, argv); w32_write(w, ARG(2), psz, envp);
    RET(0);
}
static void m__initterm(w32 *w) {
    uint64_t b = ARG(0), e = ARG(1); int psz = (int)w32_ptrsize(w);
    for (uint64_t p = b; p < e; p += (uint64_t)psz) {
        uint64_t fn = w32_read(w, p, psz);
        if (fn) { w32_call_guest(w, fn, 0, 0); if (w->exited) return; }
    }
}
static void m__initterm_e(w32 *w) {
    uint64_t b = ARG(0), e = ARG(1); int psz = (int)w32_ptrsize(w);
    for (uint64_t p = b; p < e; p += (uint64_t)psz) {
        uint64_t fn = w32_read(w, p, psz);
        if (fn) { uint64_t r = w32_call_guest(w, fn, 0, 0); if (w->exited) return; if ((uint32_t)r) { RET(r); return; } }
    }
    RET(0);
}
static void run_onexit(w32 *w) { while (g_nonexit > 0 && !w->exited) w32_call_guest(w, g_onexit[--g_nonexit], 0, 0); }
static void m_exit(w32 *w) { int code = (int)(uint32_t)ARG(0); run_onexit(w); if (!w->exited) w32_exit(w, code); }
static void m__exit(w32 *w) { w32_exit(w, (int)(uint32_t)ARG(0)); }
static void m__cexit(w32 *w) { run_onexit(w); }
static void m__c_exit(w32 *w) { (void)w; }
static void m__onexit(w32 *w) { if (g_nonexit < 128) g_onexit[g_nonexit++] = ARG(0); RET(ARG(0)); }
static void m_atexit(w32 *w) { if (g_nonexit < 128) g_onexit[g_nonexit++] = ARG(0); RET(0); }
static void m___dllonexit(w32 *w) { if (g_nonexit < 128) g_onexit[g_nonexit++] = ARG(0); RET(ARG(0)); }
static void m__amsg_exit(w32 *w) { fprintf(stderr, "runtime error R60%02u\n", (unsigned)ARG(0)); w32_exit(w, 255); }
static void m_abort(w32 *w) { fputs("abort() called\n", stderr); w32_exit(w, 3); }
static void m_signal(w32 *w) { RET(0); }
static void m_raise(w32 *w) { fprintf(stderr, "raise(%u)\n", (unsigned)ARG(0)); w32_exit(w, 3); }
static void m___p__fmode(w32 *w) { static uint64_t p; if (!p) p = w32_heap_alloc(w, 4); RET(p); }
static void m___p__commode(w32 *w) { static uint64_t p; if (!p) p = w32_heap_alloc(w, 4); RET(p); }
static void m___p___argc(w32 *w) { build_args(w); RET(w32_stub_for(w, "msvcrt.dll", "__argc")); }
static void m___p___argv(w32 *w) { build_args(w); RET(w32_stub_for(w, "msvcrt.dll", "__argv")); }
static void m___p__environ(w32 *w) { build_args(w); RET(w32_stub_for(w, "msvcrt.dll", "_environ")); }
static void m__errno(w32 *w) { ensure_state(w); RET(g_errno); }
static void m___iob_func(w32 *w) { ensure_state(w); RET(g_iob); }
static void m___acrt_iob_func(w32 *w) { ensure_state(w); RET(g_iob + (uint64_t)file_size(w) * (uint32_t)ARG(0)); }
static void m___setusermatherr(w32 *w) { (void)w; }
static void m__lock(w32 *w) { (void)w; }
static void m__unlock(w32 *w) { (void)w; }
static void m___lc_codepage_func(w32 *w) { RET(1252); }
static void m___mb_cur_max_func(w32 *w) { RET(1); }
static void m_localeconv(w32 *w) { ensure_state(w); RET(g_lconv); }
static void m_setlocale(w32 *w) { static uint64_t c; if (!c) c = w32_strdup(w, "C"); RET(c); }
/* _controlfp/_control87: the CRT's view of the FPU control word. Abstract
 * bits: _MCW_EM 0x0008001F (inexact 1, underflow 2, overflow 4, zerodivide 8,
 * invalid 0x10, denormal 0x80000), _MCW_RC 0x300 (near/down/up/chop),
 * _MCW_PC 0x30000 (64 = 0, 53 = 0x10000, 24 = 0x20000), _MCW_IC 0x40000,
 * _MCW_DN 0x3000000. x87 FCW: masks IM 1 DM 2 ZM 4 OM 8 UM 0x10 PM 0x20,
 * PC bits 8-9 (24 = 0, 53 = 2, 64 = 3), RC bits 10-11. Both units follow
 * (the CRT does the same on an SSE2 machine): the rounding mode and masks
 * go to MXCSR too. This is how MSVC-built programs end up in 53-bit mode
 * -- the dynarec's native x87 mode -- so it has to be real. */
static uint32_t cw_abstract(const w32 *w) {
    uint16_t f = w->c->fcw; uint32_t a = 0;
    if (f & 0x20) a |= 0x01; if (f & 0x10) a |= 0x02; if (f & 0x08) a |= 0x04; if (f & 0x04) a |= 0x08; if (f & 0x01) a |= 0x10; if (f & 0x02) a |= 0x80000;
    a |= ((f >> 10) & 3) << 8;
    switch ((f >> 8) & 3) { case 0: a |= 0x20000; break; case 2: a |= 0x10000; break; default: break; }
    return a;
}
static void cw_apply(w32 *w, uint32_t a) {
    uint16_t f = (uint16_t)(w->c->fcw & ~0x0F3Fu);
    if (a & 0x01) f |= 0x20; if (a & 0x02) f |= 0x10; if (a & 0x04) f |= 0x08; if (a & 0x08) f |= 0x04; if (a & 0x10) f |= 0x01; if (a & 0x80000) f |= 0x02;
    f |= (uint16_t)(((a >> 8) & 3) << 10);
    switch (a & 0x30000) { case 0x20000: break; case 0x10000: f |= 0x200; break; default: f |= 0x300; break; }
    w->c->fcw = f;
    uint32_t m = w->c->mxcsr & ~0x7F80u;                      /* masks 7-12, RC 13-14 */
    m |= (uint32_t)(f & 0x3F) << 7; m |= (uint32_t)((f >> 10) & 3) << 13;
    w->c->mxcsr = m;
}
static void m__controlfp(w32 *w) {
    uint32_t nw = (uint32_t)ARG(0), mask = (uint32_t)ARG(1), cur = cw_abstract(w);
    if (mask) cw_apply(w, (cur & ~mask) | (nw & mask));
    RET(cw_abstract(w));
}
static void m__control87(w32 *w) { m__controlfp(w); }
static void m__controlfp_s(w32 *w) {
    uint32_t nw = (uint32_t)ARG(1), mask = (uint32_t)ARG(2), cur = cw_abstract(w);
    if (mask) cw_apply(w, (cur & ~mask) | (nw & mask));
    if (ARG(0)) w32_write(w, ARG(0), 4, cw_abstract(w));
    RET(0);
}
static void m__fpreset(w32 *w) { w->c->fcw = 0x027F; w->c->fsw = 0; w->c->ftag_empty = 0xFF; w->c->mxcsr = 0x1F80; }
static void m__clearfp(w32 *w) { uint32_t sw = w->c->fsw & 0x3F; w->c->fsw &= ~0x80FFu; RET(sw); }
static void m__statusfp(w32 *w) { RET(w->c->fsw & 0x3F); }
static void m__configthreadlocale(w32 *w) { RET(0); }
static void m__set_invalid_parameter_handler(w32 *w) { RET(0); }
static void m__crt_atexit(w32 *w) { m_atexit(w); }
static void m__get_initial_narrow_environment(w32 *w) { build_args(w); RET(g_envp); }
static void m__initialize_narrow_environment(w32 *w) { RET(0); }
static void m___p___initenv(w32 *w) { static uint64_t p; if (!p) p = w32_heap_alloc(w, 8); RET(p); }
static void m__XcptFilter(w32 *w) { RET(0); }
static void m___C_specific_handler(w32 *w) { RET(1); }
static void m__except_handler3(w32 *w) { RET(1); }
static void m__except_handler4_common(w32 *w) { RET(1); }
static void m__beginthreadex(w32 *w) { fprintf(stderr, "winrun: threads are not supported yet (_beginthreadex)\n"); RET(0); }
static void m__time64(w32 *w) { uint64_t t = (uint64_t)time(0); if (ARG(0)) w32_write(w, ARG(0), 8, t); w32_ret64(w, t); }
static void m_time(w32 *w) { m__time64(w); }
static void m_clock(w32 *w) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); RET((uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000)); }
static void m_getenv(w32 *w) {
    const char *name = GSTR(ARG(0)); const char *e = W32P(w, w->env_block); size_t nl = strlen(name);
    (void)e;
    for (uint64_t p = w->env_block; *(const char *)W32P(w, p); p += strlen(W32P(w, p)) + 1) {
        const char *s = W32P(w, p);
        if (!strncasecmp(s, name, nl) && s[nl] == '=') { RET(p + nl + 1); return; }
    }
    RET(0);
}

/* ---- memory ---- */
static void m_malloc(w32 *w) { uint64_t p = w32_heap_alloc(w, ARG(0)); if (!p) set_errno(w, ENOMEM); RET(p); }
static void m_calloc(w32 *w) { uint64_t n = ARG(0) * ARG(1); uint64_t p = w32_heap_alloc(w, n); if (p) memset(W32P(w, p), 0, n); RET(p); }
static void m_realloc(w32 *w) { RET(w32_heap_realloc(w, ARG(0), ARG(1))); }
static void m_free(w32 *w) { w32_heap_free(w, ARG(0)); }
static void m__msize(w32 *w) { RET(w32_heap_size(w, ARG(0))); }
static void m__aligned_malloc(w32 *w) {
    uint64_t size = ARG(0), align = ARG(1); if (align < 16) align = 16;
    uint64_t raw = w32_heap_alloc(w, size + align + 16);
    uint64_t p = (raw + 16 + align - 1) & ~(align - 1);
    w32_write(w, p - 8, 8, raw);                  /* stash the block address just below */
    RET(p);
}
static void m__aligned_free(w32 *w) { uint64_t p = ARG(0); if (p) w32_heap_free(w, w32_read(w, p - 8, 8)); }

/* ---- strings (on guest memory) ---- */
static void m_memcpy(w32 *w) { memcpy(W32P(w, ARG(0)), W32P(w, ARG(1)), ARG(2)); RET(ARG(0)); }
static void m_memmove(w32 *w) { memmove(W32P(w, ARG(0)), W32P(w, ARG(1)), ARG(2)); RET(ARG(0)); }
static void m_memset(w32 *w) { memset(W32P(w, ARG(0)), (int)ARG(1), ARG(2)); RET(ARG(0)); }
static void m_memcmp(w32 *w) { RET((uint64_t)(int64_t)memcmp(W32P(w, ARG(0)), W32P(w, ARG(1)), ARG(2))); }
static void m_memchr(w32 *w) { const void *p = W32P(w, ARG(0)); const void *r = memchr(p, (int)ARG(1), ARG(2)); RET(r ? ARG(0) + (uint64_t)((const uint8_t *)r - (const uint8_t *)p) : 0); }
static void m_strlen(w32 *w) { RET(strlen(GSTR(ARG(0)))); }
static void m_strnlen(w32 *w) { RET(strnlen(GSTR(ARG(0)), ARG(1))); }
static void m_strcpy(w32 *w) { strcpy(W32P(w, ARG(0)), GSTR(ARG(1))); RET(ARG(0)); }
static void m_strncpy(w32 *w) { strncpy(W32P(w, ARG(0)), GSTR(ARG(1)), ARG(2)); RET(ARG(0)); }
static void m_strcat(w32 *w) { strcat(W32P(w, ARG(0)), GSTR(ARG(1))); RET(ARG(0)); }
static void m_strncat(w32 *w) { strncat(W32P(w, ARG(0)), GSTR(ARG(1)), ARG(2)); RET(ARG(0)); }
static void m_strcmp(w32 *w) { RET((uint64_t)(int64_t)strcmp(GSTR(ARG(0)), GSTR(ARG(1)))); }
static void m_strncmp(w32 *w) { RET((uint64_t)(int64_t)strncmp(GSTR(ARG(0)), GSTR(ARG(1)), ARG(2))); }
static void m__stricmp(w32 *w) { RET((uint64_t)(int64_t)strcasecmp(GSTR(ARG(0)), GSTR(ARG(1)))); }
static void m__strnicmp(w32 *w) { RET((uint64_t)(int64_t)strncasecmp(GSTR(ARG(0)), GSTR(ARG(1)), ARG(2))); }
static void m_strchr(w32 *w) { const char *s = GSTR(ARG(0)); const char *r = strchr(s, (int)ARG(1)); RET(r ? ARG(0) + (uint64_t)(r - s) : 0); }
static void m_strrchr(w32 *w) { const char *s = GSTR(ARG(0)); const char *r = strrchr(s, (int)ARG(1)); RET(r ? ARG(0) + (uint64_t)(r - s) : 0); }
static void m_strstr(w32 *w) { const char *s = GSTR(ARG(0)); const char *r = strstr(s, GSTR(ARG(1))); RET(r ? ARG(0) + (uint64_t)(r - s) : 0); }
static void m_strdup(w32 *w) { RET(w32_strdup(w, GSTR(ARG(0)))); }
static void m_strtol(w32 *w) { const char *s = GSTR(ARG(0)); char *e; long v = strtol(s, &e, (int)ARG(2)); if (ARG(1)) w32_write(w, ARG(1), (int)w32_ptrsize(w), ARG(0) + (uint64_t)(e - s)); RET((uint64_t)(int64_t)(w->is32 ? (int32_t)v : v)); }
static void m_strtoul(w32 *w) { const char *s = GSTR(ARG(0)); char *e; unsigned long v = strtoul(s, &e, (int)ARG(2)); if (ARG(1)) w32_write(w, ARG(1), (int)w32_ptrsize(w), ARG(0) + (uint64_t)(e - s)); RET(w->is32 ? (uint32_t)v : v); }
static void m__strtoi64(w32 *w) { const char *s = GSTR(ARG(0)); char *e; long long v = strtoll(s, &e, (int)ARG(2)); if (ARG(1)) w32_write(w, ARG(1), (int)w32_ptrsize(w), ARG(0) + (uint64_t)(e - s)); w32_ret64(w, (uint64_t)v); }
static void m_strtod(w32 *w) { const char *s = GSTR(ARG(0)); char *e; double v = strtod(s, &e); if (ARG(1)) w32_write(w, ARG(1), (int)w32_ptrsize(w), ARG(0) + (uint64_t)(e - s)); w32_fret(w, v); }
static void m_atoi(w32 *w) { RET((uint64_t)(int64_t)atoi(GSTR(ARG(0)))); }
static void m_atol(w32 *w) { RET((uint64_t)(int64_t)(int32_t)atol(GSTR(ARG(0)))); }
static void m_atof(w32 *w) { w32_fret(w, atof(GSTR(ARG(0)))); }
static void m_toupper(w32 *w) { RET((uint64_t)toupper((int)ARG(0))); }
static void m_tolower(w32 *w) { RET((uint64_t)tolower((int)ARG(0))); }
static void m_isspace(w32 *w) { RET((uint64_t)(isspace((int)ARG(0)) != 0)); }
static void m_isdigit(w32 *w) { RET((uint64_t)(isdigit((int)ARG(0)) != 0)); }
static void m_isalpha(w32 *w) { RET((uint64_t)(isalpha((int)ARG(0)) != 0)); }
static void m_isalnum(w32 *w) { RET((uint64_t)(isalnum((int)ARG(0)) != 0)); }
static void m_isupper(w32 *w) { RET((uint64_t)(isupper((int)ARG(0)) != 0)); }
static void m_islower(w32 *w) { RET((uint64_t)(islower((int)ARG(0)) != 0)); }
static void m_isprint(w32 *w) { RET((uint64_t)(isprint((int)ARG(0)) != 0)); }
static void m_wcslen(w32 *w) { RET(w32_wcslen(w, ARG(0))); }
static void m_wcscpy(w32 *w) { size_t n = w32_wcslen(w, ARG(1)); memcpy(W32P(w, ARG(0)), W32P(w, ARG(1)), 2 * (n + 1)); RET(ARG(0)); }
static void m_wcscmp(w32 *w) { const uint16_t *a = W32P(w, ARG(0)), *b = W32P(w, ARG(1)); while (*a && *a == *b) { a++; b++; } RET((uint64_t)(int64_t)((int)*a - (int)*b)); }
static void m_strerror(w32 *w) { static uint64_t p; if (!p) p = w32_heap_alloc(w, 128); snprintf(W32P(w, p), 128, "%s", strerror((int)ARG(0))); RET(p); }
static void m_qsort(w32 *w) {
    /* (base, n, size, cmp): the comparator is guest code */
    uint64_t base = ARG(0), n = ARG(1), size = ARG(2), cmp = ARG(3);
    /* insertion sort through w32_call_guest -- n is small in CRT use */
    uint8_t *tmp = malloc(size);
    for (uint64_t i = 1; i < n && !w->exited; i++) {
        memcpy(tmp, (uint8_t *)W32P(w, base) + i * size, size);
        /* insertion step */
        uint64_t j = i;
        while (j > 0) {
            uint64_t scratch = w32_heap_alloc(w, size); memcpy(W32P(w, scratch), tmp, size);
            uint64_t args[2] = { base + (j - 1) * size, scratch };
            int64_t r = (int32_t)w32_call_guest(w, cmp, 2, args);
            w32_heap_free(w, scratch);
            if (w->exited || r <= 0) break;
            memcpy((uint8_t *)W32P(w, base) + j * size, (uint8_t *)W32P(w, base) + (j - 1) * size, size);
            j--;
        }
        memcpy((uint8_t *)W32P(w, base) + j * size, tmp, size);
    }
    free(tmp);
}

/* ---- stdio ---- */
static void wr(int fd, const void *p, size_t n) { if (write(fd, p, n) < 0) { /* stream closed */ } }
static void m_puts(w32 *w) { const char *s = GSTR(ARG(0)); wr(1, s, strlen(s)); wr(1, "\n", 1); RET(0); }
static void m_putchar(w32 *w) { char c = (char)ARG(0); wr(1, &c, 1); RET((uint8_t)c); }
static void m_fputs(w32 *w) { int fd = file_fd(w, ARG(1)); const char *s = GSTR(ARG(0)); if (fd < 0) { RET((uint64_t)-1); return; } wr(fd, s, strlen(s)); RET(0); }
static void m_fputc(w32 *w) { int fd = file_fd(w, ARG(1)); char c = (char)ARG(0); if (fd < 0) { RET((uint64_t)-1); return; } wr(fd, &c, 1); RET((uint8_t)c); }
static void m_fwrite(w32 *w) { int fd = file_fd(w, ARG(3)); uint64_t n = ARG(1) * ARG(2); if (fd < 0) { RET(0); return; } wr(fd, W32P(w, ARG(0)), n); RET(ARG(2)); }
static void m_fflush(w32 *w) { RET(0); }
static void m_fgetc(w32 *w) { int fd = file_fd(w, ARG(0)); unsigned char c; if (fd < 0 || read(fd, &c, 1) != 1) { RET((uint64_t)-1); return; } RET(c); }
static void m_fgets(w32 *w) {
    int fd = file_fd(w, ARG(2)); char *buf = W32P(w, ARG(0)); int n = (int)ARG(1);
    if (fd < 0 || n <= 0) { RET(0); return; }
    int i = 0; while (i < n - 1) { char c; if (read(fd, &c, 1) != 1) break; buf[i++] = c; if (c == '\n') break; }
    buf[i] = 0; RET(i ? ARG(0) : 0);
}
static void m_fopen(w32 *w) {
    const char *name = GSTR(ARG(0)), *mode = GSTR(ARG(1));
    char path[4096]; size_t i = 0; const char *p = name; if (((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':') p += 2;
    for (; *p && i + 1 < sizeof path; p++) path[i++] = *p == '\\' ? '/' : *p;
    path[i] = 0;
    if (!strncasecmp(path, "/xcore/", 7)) memmove(path, path + 7, strlen(path + 7) + 1);
    char m[8]; size_t k = 0; for (const char *q = mode; *q && k < 6; q++) if (*q != 't') m[k++] = *q; m[k] = 0;
    FILE *f = fopen(path, m);
    if (!f) { set_errno(w, errno); RET(0); return; }
    /* a FILE in guest memory whose _file is a host fd (the host FILE is not kept: the fd is enough) */
    int fd = dup(fileno(f)); fclose(f);
    uint64_t gf = w32_heap_alloc(w, 48);
    w32_write(w, gf + (uint64_t)file_off(w), 4, (uint64_t)fd);
    RET(gf);
}
static int gfile_fd(w32 *w, uint64_t gf) { int fd = file_fd(w, gf); if (fd >= 0) return fd; return (int)(int32_t)w32_read(w, gf + (uint64_t)file_off(w), 4); }
static void m_fclose(w32 *w) { int fd = gfile_fd(w, ARG(0)); if (fd > 2) close(fd); RET(0); }
static void m_fread(w32 *w) { int fd = gfile_fd(w, ARG(3)); uint64_t n = ARG(1) * ARG(2); ssize_t r = read(fd, W32P(w, ARG(0)), n); RET(r <= 0 || !ARG(1) ? 0 : (uint64_t)r / ARG(1)); }
static void m_fseek(w32 *w) { int fd = gfile_fd(w, ARG(0)); RET(lseek(fd, (int64_t)(int32_t)ARG(1), (int)ARG(2)) < 0 ? (uint64_t)-1 : 0); }
static void m_ftell(w32 *w) { int fd = gfile_fd(w, ARG(0)); RET((uint64_t)(int64_t)lseek(fd, 0, SEEK_CUR)); }
static void m_feof(w32 *w) { RET(0); }
static void m_ferror(w32 *w) { RET(0); }
static void m_setvbuf(w32 *w) { RET(0); }
static void m__fileno(w32 *w) { RET((uint64_t)gfile_fd(w, ARG(0))); }
static void m__isatty(w32 *w) { RET((uint64_t)isatty((int)ARG(0))); }
static void m__write(w32 *w) { ssize_t r = write((int)ARG(0), W32P(w, ARG(1)), (uint32_t)ARG(2)); RET((uint64_t)(int64_t)r); }
static void m__read(w32 *w) { ssize_t r = read((int)ARG(0), W32P(w, ARG(1)), (uint32_t)ARG(2)); RET((uint64_t)(int64_t)r); }

/* the printf family: the va_list is either the argument list itself (variadic
 * entry points -- the arguments follow the fixed ones in memory / registers) or
 * a pointer handed to us (v* entry points) */
static uint64_t vararg_start(w32 *w, int fixed) {
    if (w->is32) return w->c->gpr[XC_RSP] + 4 + 4u * fixed;
    /* x64: the first four arguments are in registers; spill them to the home area
     * so the variadic reader can walk memory. The home area is [rsp+8, rsp+0x28). */
    xc_cpu *c = w->c;
    uint64_t home = c->gpr[XC_RSP] + 8;
    w32_write(w, home + 0, 8, c->gpr[XC_RCX]); w32_write(w, home + 8, 8, c->gpr[XC_RDX]);
    w32_write(w, home + 16, 8, c->gpr[XC_R8]); w32_write(w, home + 24, 8, c->gpr[XC_R9]);
    return home + 8u * fixed;
}
static void do_printf(w32 *w, int fd, char *buf, size_t cap, const char *fmt, const uint16_t *wfmt, uint64_t ap) {
    out_t o = { buf, cap, 0, fd, w };
    int n = gfmt(w, &o, fmt, wfmt, ap);
    if (buf) { size_t t = o.len < cap ? o.len : (cap ? cap - 1 : 0); if (cap) buf[t] = 0; }
    RET((uint64_t)(int64_t)n);
}
static void m_printf(w32 *w)   { do_printf(w, 1, 0, 0, GSTR(ARG(0)), 0, vararg_start(w, 1)); }
static void m_vprintf(w32 *w)  { do_printf(w, 1, 0, 0, GSTR(ARG(0)), 0, ARG(1)); }
static void m_fprintf(w32 *w)  { do_printf(w, gfile_fd(w, ARG(0)), 0, 0, GSTR(ARG(1)), 0, vararg_start(w, 2)); }
static void m_vfprintf(w32 *w) { do_printf(w, gfile_fd(w, ARG(0)), 0, 0, GSTR(ARG(1)), 0, ARG(2)); }
static void m_sprintf(w32 *w)  { char tmp[65536]; do_printf(w, -1, tmp, sizeof tmp, GSTR(ARG(1)), 0, vararg_start(w, 2)); strcpy(W32P(w, ARG(0)), tmp); }
static void m_vsprintf(w32 *w) { char tmp[65536]; do_printf(w, -1, tmp, sizeof tmp, GSTR(ARG(1)), 0, ARG(2)); strcpy(W32P(w, ARG(0)), tmp); }
static void m__snprintf(w32 *w) { char tmp[65536]; uint64_t n = ARG(1); do_printf(w, -1, tmp, sizeof tmp, GSTR(ARG(2)), 0, vararg_start(w, 3));
    if (n) { size_t l = strlen(tmp); if (l >= n) { memcpy(W32P(w, ARG(0)), tmp, n); RET((uint64_t)-1); } else memcpy(W32P(w, ARG(0)), tmp, l + 1); } }
static void m__vsnprintf(w32 *w) { char tmp[65536]; uint64_t n = ARG(1); do_printf(w, -1, tmp, sizeof tmp, GSTR(ARG(2)), 0, ARG(3));
    if (n) { size_t l = strlen(tmp); if (l >= n) { memcpy(W32P(w, ARG(0)), tmp, n); RET((uint64_t)-1); } else memcpy(W32P(w, ARG(0)), tmp, l + 1); } }
static void m__vscprintf(w32 *w) { do_printf(w, -1, 0, 0, GSTR(ARG(0)), 0, ARG(1)); }
static void m___stdio_common_vfprintf(w32 *w) { /* (options64, FILE*, fmt, locale, va_list) */
    int a = w->is32 ? 2 : 1; do_printf(w, gfile_fd(w, ARG(a)), 0, 0, GSTR(ARG(a + 1)), 0, ARG(a + 3)); }
static void m___stdio_common_vsprintf(w32 *w) { /* (options64, buf, len, fmt, locale, va_list) */
    int a = w->is32 ? 2 : 1; char tmp[65536]; uint64_t buf = ARG(a), n = ARG(a + 1);
    do_printf(w, -1, tmp, sizeof tmp, GSTR(ARG(a + 2)), 0, ARG(a + 4));
    size_t l = strlen(tmp); if (buf && n) { if (l >= n) { memcpy(W32P(w, buf), tmp, n - 1); w32_write(w, buf + n - 1, 1, 0); } else memcpy(W32P(w, buf), tmp, l + 1); } }
static void m_wprintf(w32 *w) { do_printf(w, 1, 0, 0, 0, W32P(w, ARG(0)), vararg_start(w, 1)); }

/* ---- math (x87 return on x86, xmm0 on x64) ---- */
#define M1(n) static void m_##n(w32 *w) { w32_fret(w, n(w32_farg(w, 0))); }
M1(sqrt) M1(sin) M1(cos) M1(tan) M1(asin) M1(acos) M1(atan) M1(exp) M1(log) M1(log10) M1(floor) M1(ceil) M1(fabs) M1(sinh) M1(cosh) M1(tanh)
static void m_pow(w32 *w) { w32_fret(w, pow(w32_farg(w, 0), w32_farg(w, 1))); }
static void m_atan2(w32 *w) { w32_fret(w, atan2(w32_farg(w, 0), w32_farg(w, 1))); }
static void m_fmod(w32 *w) { w32_fret(w, fmod(w32_farg(w, 0), w32_farg(w, 1))); }
static void m_ldexp(w32 *w) { w32_fret(w, ldexp(w32_farg(w, 0), (int)(int32_t)(w->is32 ? ARG(2) : ARG(1)))); }
static void m_modf(w32 *w) { double ip; double f = modf(w32_farg(w, 0), &ip); uint64_t b; memcpy(&b, &ip, 8); w32_write(w, w->is32 ? ARG(2) : ARG(1), 8, b); w32_fret(w, f); }
static void m_frexp(w32 *w) { int e; double f = frexp(w32_farg(w, 0), &e); w32_write(w, w->is32 ? ARG(2) : ARG(1), 4, (uint32_t)e); w32_fret(w, f); }
static void m__finite(w32 *w) { RET((uint64_t)isfinite(w32_farg(w, 0))); }
static void m__isnan(w32 *w) { RET((uint64_t)(isnan(w32_farg(w, 0)) != 0)); }
static void m_abs(w32 *w) { RET((uint64_t)(uint32_t)abs((int)(int32_t)ARG(0))); }
static void m_labs(w32 *w) { RET((uint64_t)(uint32_t)labs((int32_t)ARG(0))); }
static void m_rand(w32 *w) { static uint32_t s = 1; s = s * 214013u + 2531011u; RET((s >> 16) & 0x7FFF); }
static void m_srand(w32 *w) { (void)w; }

#define F(n, a)   { #n, a, 1, m_##n, 0 }
#define D(n, sz)  { #n, 0, 1, 0, sz }
const w32_api w32_msvcrt[] = {
    F(__set_app_type, 1), F(__getmainargs, 5), F(__wgetmainargs, 5), F(_initterm, 2), F(_initterm_e, 2), F(exit, 1), F(_exit, 1), F(_cexit, 0), F(_c_exit, 0),
    F(_onexit, 1), F(atexit, 1), F(__dllonexit, 3), F(_amsg_exit, 1), F(abort, 0), F(signal, 2), F(raise, 1),
    F(__p__fmode, 0), F(__p__commode, 0), F(__p___argc, 0), F(__p___argv, 0), F(__p__environ, 0), F(_errno, 0), F(__iob_func, 0), F(__acrt_iob_func, 1),
    F(__setusermatherr, 1), F(_lock, 1), F(_unlock, 1), { "___lc_codepage_func", 0, 1, m___lc_codepage_func, 0 }, { "___mb_cur_max_func", 0, 1, m___mb_cur_max_func, 0 }, F(localeconv, 0), F(setlocale, 2),
    F(_controlfp, 2), F(_controlfp_s, 3), F(_control87, 2), F(_fpreset, 0), F(_clearfp, 0), F(_statusfp, 0), F(_configthreadlocale, 1), F(_set_invalid_parameter_handler, 1), F(_crt_atexit, 1),
    F(_get_initial_narrow_environment, 0), F(_initialize_narrow_environment, 0), F(__p___initenv, 0), F(_XcptFilter, 2),
    F(__C_specific_handler, 4), F(_except_handler3, 4), F(_except_handler4_common, 6), F(_beginthreadex, 6), F(_time64, 1), F(time, 1), F(clock, 0), F(getenv, 1),
    D(__initenv, 8), D(_commode, 4), D(_fmode, 4), D(__mb_cur_max, 4), D(_iob, 3 * 48), D(__argc, 4), D(__argv, 8), D(_environ, 8),
    F(malloc, 1), F(calloc, 2), F(realloc, 2), F(free, 1), F(_msize, 1), F(_aligned_malloc, 2), F(_aligned_free, 1),
    F(memcpy, 3), F(memmove, 3), F(memset, 3), F(memcmp, 3), F(memchr, 3), F(strlen, 1), F(strnlen, 2), F(strcpy, 2), F(strncpy, 3), F(strcat, 2), F(strncat, 3),
    F(strcmp, 2), F(strncmp, 3), F(_stricmp, 2), F(_strnicmp, 3), F(strchr, 2), F(strrchr, 2), F(strstr, 2), F(strdup, 1), F(strtol, 3), F(strtoul, 3), F(_strtoi64, 3), F(strtod, 2),
    F(atoi, 1), F(atol, 1), F(atof, 1), F(toupper, 1), F(tolower, 1), F(isspace, 1), F(isdigit, 1), F(isalpha, 1), F(isalnum, 1), F(isupper, 1), F(islower, 1), F(isprint, 1),
    F(wcslen, 1), F(wcscpy, 2), F(wcscmp, 2), F(strerror, 1), F(qsort, 4),
    F(puts, 1), F(putchar, 1), F(fputs, 2), F(fputc, 2), F(fwrite, 4), F(fflush, 1), F(fgetc, 1), F(fgets, 3), F(fopen, 2), F(fclose, 1), F(fread, 4), F(fseek, 3), F(ftell, 1),
    F(feof, 1), F(ferror, 1), F(setvbuf, 4), F(_fileno, 1), F(_isatty, 1), F(_write, 3), F(_read, 3),
    F(printf, 1), F(vprintf, 2), F(fprintf, 2), F(vfprintf, 3), F(sprintf, 2), F(vsprintf, 3), F(_snprintf, 3), F(_vsnprintf, 4), F(_vscprintf, 2),
    F(__stdio_common_vfprintf, 5), F(__stdio_common_vsprintf, 6), F(wprintf, 1),
    F(sqrt, 1), F(sin, 1), F(cos, 1), F(tan, 1), F(asin, 1), F(acos, 1), F(atan, 1), F(exp, 1), F(log, 1), F(log10, 1), F(floor, 1), F(ceil, 1), F(fabs, 1), F(sinh, 1), F(cosh, 1), F(tanh, 1),
    F(pow, 2), F(atan2, 2), F(fmod, 2), F(ldexp, 2), F(modf, 2), F(frexp, 2), F(_finite, 1), F(_isnan, 1), F(abs, 1), F(labs, 1), F(rand, 0), F(srand, 1),
    { 0, 0, 0, 0, 0 }
};
#undef F
#undef D
