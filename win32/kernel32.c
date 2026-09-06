/* kernel32.dll -- the process, memory, file and console surface.
 *
 * Every function here is the Win32 API as documented, implemented on POSIX.
 * Handles are small integers into w->handles; the three standard streams are
 * handles 4, 8 and 12 (GetStdHandle's pseudo-handles map onto them). Error
 * codes go to TEB.LastErrorValue like the real thing, so GetLastError is a
 * memory read the guest could even inline.
 */
#define _GNU_SOURCE
#include "w32.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum { ERROR_FILE_NOT_FOUND = 2, ERROR_ACCESS_DENIED = 5, ERROR_INVALID_HANDLE = 6, ERROR_NOT_ENOUGH_MEMORY = 8,
       ERROR_INVALID_PARAMETER = 87, ERROR_PROC_NOT_FOUND = 127, ERROR_MOD_NOT_FOUND = 126, ERROR_ALREADY_EXISTS = 183,
       ERROR_INSUFFICIENT_BUFFER = 122, ERROR_CALL_NOT_IMPLEMENTED = 120 };

static uint64_t bool_(int b) { return b ? 1 : 0; }
static uint64_t filetime_now(void) {
    struct timeval tv; gettimeofday(&tv, 0);
    return ((uint64_t)tv.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)tv.tv_usec * 10;
}
static uint64_t ticks_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000; }
static uint64_t ticks_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }

/* Windows path -> host path: strip a drive letter, flip the slashes. Paths
 * under C:\xcore\ are the guest's own directory (where the .exe lives). */
static void host_path(w32 *w, const char *win, char *out, size_t n) {
    char tmp[4096]; size_t i = 0;
    const char *p = win;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') p += 2;
    for (; *p && i + 1 < sizeof tmp; p++) tmp[i++] = *p == '\\' ? '/' : *p;
    tmp[i] = 0;
    if (!strncasecmp(tmp, "/xcore/", 7)) {
        const char *slash = strrchr(w->exe_path, '/');
        if (slash) snprintf(out, n, "%.*s/%s", (int)(slash - w->exe_path), w->exe_path, tmp + 7);
        else snprintf(out, n, "%s", tmp + 7);
    } else snprintf(out, n, "%s", tmp[0] == '/' && win[1] == ':' ? tmp + 1 : tmp);   /* absolute Windows paths become relative */
}

/* ---- process ---- */
static void k_ExitProcess(w32 *w) { w32_exit(w, (int)(uint32_t)ARG(0)); }
static void k_TerminateProcess(w32 *w) { w32_exit(w, (int)(uint32_t)ARG(1)); }
static void k_GetCurrentProcess(w32 *w) { RET(w->is32 ? 0xFFFFFFFFu : ~0ull); }
static void k_GetCurrentProcessId(w32 *w) { RET(4242); }
static void k_GetCurrentThread(w32 *w) { RET(w->is32 ? 0xFFFFFFFEu : ~1ull); }
static void k_GetCurrentThreadId(w32 *w) { RET(4243); }
static void k_GetCommandLineA(w32 *w) { RET(w->cmdline); }
static void k_GetCommandLineW(w32 *w) { RET(w->cmdline_w); }
static void k_GetLastError(w32 *w) { RET(w32_read(w, w->teb + (w->is32 ? TEB32_LASTERROR : TEB64_LASTERROR), 4)); }
static void k_SetLastError(w32 *w) { w32_set_last_error(w, (uint32_t)ARG(0)); }
static void k_GetStartupInfoA(w32 *w) {
    uint64_t p = ARG(0); int psz = (int)w32_ptrsize(w);
    uint64_t size = w->is32 ? 68 : 104;
    memset(W32P(w, p), 0, size);
    w32_write(w, p, 4, size);
    /* hStdInput/Output/Error at the end */
    uint64_t h = p + size - 3 * psz;
    w32_write(w, h, psz, 4); w32_write(w, h + psz, psz, 8); w32_write(w, h + 2 * psz, psz, 12);
}
static void k_GetEnvironmentStringsA(w32 *w) { RET(w->env_block); }
static void k_GetEnvironmentStringsW(w32 *w) { RET(w->env_block_w); }
static void k_FreeEnvironmentStrings(w32 *w) { RET(1); }
static void k_GetEnvironmentVariableA(w32 *w) {
    const char *name = GSTR(ARG(0)); uint64_t buf = ARG(1); uint32_t n = (uint32_t)ARG(2);
    const char *e = W32P(w, w->env_block); size_t nl = strlen(name);
    for (; *e; e += strlen(e) + 1) if (!strncasecmp(e, name, nl) && e[nl] == '=') {
        size_t vl = strlen(e + nl + 1);
        if (vl + 1 > n) { RET(vl + 1); return; }
        memcpy(W32P(w, buf), e + nl + 1, vl + 1); RET(vl); return;
    }
    w32_set_last_error(w, 203 /* ERROR_ENVVAR_NOT_FOUND */); RET(0);
}
static void k_GetModuleHandleA(w32 *w) {
    uint64_t name = ARG(0);
    if (!name) { RET(w->image_base); return; }
    uint64_t h = w32_module_handle(w, GSTR(name));
    if (!h) w32_set_last_error(w, ERROR_MOD_NOT_FOUND);
    RET(h);
}
static void k_GetModuleHandleW(w32 *w) {
    uint64_t name = ARG(0); char buf[260];
    if (!name) { RET(w->image_base); return; }
    w32_wtoa(w, name, buf, sizeof buf);
    uint64_t h = w32_module_handle(w, buf);
    if (!h) w32_set_last_error(w, ERROR_MOD_NOT_FOUND);
    RET(h);
}
static void k_GetModuleHandleExW(w32 *w) {
    uint64_t name = ARG(1), out = ARG(2); char buf[260]; uint64_t h;
    if (!name) h = w->image_base; else { w32_wtoa(w, name, buf, sizeof buf); h = w32_module_handle(w, buf); }
    w32_write(w, out, (int)w32_ptrsize(w), h);
    RET(bool_(h != 0));
}
static void k_LoadLibraryA(w32 *w) { uint64_t h = w32_module_handle(w, GSTR(ARG(0))); if (!h) w32_set_last_error(w, ERROR_MOD_NOT_FOUND); RET(h); }
static void k_LoadLibraryW(w32 *w) { char buf[260]; w32_wtoa(w, ARG(0), buf, sizeof buf); uint64_t h = w32_module_handle(w, buf); if (!h) w32_set_last_error(w, ERROR_MOD_NOT_FOUND); RET(h); }
static void k_LoadLibraryExA(w32 *w) { k_LoadLibraryA(w); }
static void k_LoadLibraryExW(w32 *w) { k_LoadLibraryW(w); }
static void k_FreeLibrary(w32 *w) { RET(1); }
static void k_GetProcAddress(w32 *w) {
    uint64_t h = ARG(0), name = ARG(1);
    const char *dll = 0;
    static const char *names[] = { "kernel32.dll", "msvcrt.dll", "ntdll.dll", "user32.dll" };
    for (int d = 0; d < 4; d++) if (h == w->stub_base + 0x10000u * (d + 1)) dll = names[d];
    if (!dll || (name >> 16) == 0) { w32_set_last_error(w, ERROR_PROC_NOT_FOUND); RET(0); return; }
    uint64_t a = w32_stub_for(w, dll, GSTR(name));
    /* a "missing" stub means we do not have it: report absence like Windows would */
    if (w->verbose) fprintf(stderr, "winrun: GetProcAddress(%s, %s) = %#llx\n", dll, GSTR(name), (unsigned long long)a);
    RET(a);
}
static void k_GetModuleFileNameA(w32 *w) {
    uint64_t buf = ARG(1); uint32_t n = (uint32_t)ARG(2);
    const char *slash = strrchr(w->exe_path, '/');
    char s[300]; snprintf(s, sizeof s, "C:\\xcore\\%s", slash ? slash + 1 : w->exe_path);
    size_t l = strlen(s); if (l + 1 > n) l = n ? n - 1 : 0;
    if (n) { memcpy(W32P(w, buf), s, l); w32_write(w, buf + l, 1, 0); }
    RET(l);
}
static void k_GetModuleFileNameW(w32 *w) {
    uint64_t buf = ARG(1); uint32_t n = (uint32_t)ARG(2);
    const char *slash = strrchr(w->exe_path, '/');
    char s[300]; snprintf(s, sizeof s, "C:\\xcore\\%s", slash ? slash + 1 : w->exe_path);
    size_t l = strlen(s); if (l + 1 > n) l = n ? n - 1 : 0;
    uint16_t *d = W32P(w, buf);
    if (d) { for (size_t i = 0; i < l; i++) d[i] = (uint8_t)s[i]; d[l] = 0; }
    RET(l);
}
static void k_IsDebuggerPresent(w32 *w) { RET(0); }
static void k_OutputDebugStringA(w32 *w) { fprintf(stderr, "[dbg] %s", GSTR(ARG(0))); }
static void k_SetUnhandledExceptionFilter(w32 *w) { RET(0); }
static void k_UnhandledExceptionFilter(w32 *w) { RET(1); }
static void k_GetSystemInfo(w32 *w) {
    uint64_t p = ARG(0); int psz = (int)w32_ptrsize(w);
    memset(W32P(w, p), 0, w->is32 ? 36 : 48);
    w32_write(w, p + 4, 4, 4096);                                   /* dwPageSize */
    w32_write(w, p + 8, psz, 0x10000);                               /* lpMinimumApplicationAddress */
    w32_write(w, p + 8 + psz, psz, w->is32 ? 0x7FFEFFFF : 0x7FFFFFFEFFFFull);
    w32_write(w, p + 8 + 2 * psz, psz, 0xF);                         /* dwActiveProcessorMask */
    w32_write(w, p + 8 + 3 * psz, 4, 4);                             /* dwNumberOfProcessors */
    w32_write(w, p + 8 + 3 * psz + 8, 4, 65536);                     /* dwAllocationGranularity */
    w32_write(w, p + 8 + 3 * psz + 12, 2, w->is32 ? 0 : 9);          /* wProcessorArchitecture: x86 / AMD64 */
    w32_write(w, p, 2, w->is32 ? 0 : 9);
}
static void k_GetNativeSystemInfo(w32 *w) { k_GetSystemInfo(w); }
static void k_GetVersion(w32 *w) { RET(0x4A64000Au); }             /* 10.0 build 19045 */
static void k_GetVersionExA(w32 *w) {
    uint64_t p = ARG(0);
    w32_write(w, p + 4, 4, 10); w32_write(w, p + 8, 4, 0); w32_write(w, p + 12, 4, 19045); w32_write(w, p + 16, 4, 2);
    RET(1);
}
static void k_GetVersionExW(w32 *w) { k_GetVersionExA(w); }
static void k_Sleep(w32 *w) { uint32_t ms = (uint32_t)ARG(0); if (ms) usleep(ms * 1000u); }
static void k_GetTickCount(w32 *w) { RET((uint32_t)ticks_ms()); }
static void k_GetTickCount64(w32 *w) { w32_ret64(w, ticks_ms()); }
static void k_QueryPerformanceCounter(w32 *w) { w32_write(w, ARG(0), 8, ticks_ns()); RET(1); }
static void k_QueryPerformanceFrequency(w32 *w) { w32_write(w, ARG(0), 8, 1000000000ull); RET(1); }
static void k_GetSystemTimeAsFileTime(w32 *w) { w32_write(w, ARG(0), 8, filetime_now()); }
static void k_GetSystemTimePreciseAsFileTime(w32 *w) { k_GetSystemTimeAsFileTime(w); }
static void k_GetLocalTime(w32 *w) {
    time_t t = time(0); struct tm tm; localtime_r(&t, &tm); uint64_t p = ARG(0);
    uint16_t f[8] = { (uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_wday, (uint16_t)tm.tm_mday, (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min, (uint16_t)tm.tm_sec, 0 };
    memcpy(W32P(w, p), f, sizeof f);
}
static void k_GetSystemTime(w32 *w) { k_GetLocalTime(w); }
static void k_GetTimeZoneInformation(w32 *w) { memset(W32P(w, ARG(0)), 0, 172); RET(0); }
static void k_GetACP(w32 *w) { RET(1252); }
static void k_GetOEMCP(w32 *w) { RET(437); }
static void k_GetConsoleCP(w32 *w) { RET(437); }
static void k_GetConsoleOutputCP(w32 *w) { RET(437); }
static void k_IsValidCodePage(w32 *w) { RET(1); }
static void k_GetCPInfo(w32 *w) { uint64_t p = ARG(1); memset(W32P(w, p), 0, 20); w32_write(w, p, 4, 1); w32_write(w, p + 4, 1, '?'); RET(1); }
static void k_GetUserDefaultLCID(w32 *w) { RET(0x409); }
static void k_GetUserDefaultLangID(w32 *w) { RET(0x409); }
static void k_GetSystemDefaultLCID(w32 *w) { RET(0x409); }
static void k_GetThreadLocale(w32 *w) { RET(0x409); }
static void k_IsDBCSLeadByteEx(w32 *w) { RET(0); }
static void k_IsDBCSLeadByte(w32 *w) { RET(0); }
static void k_MultiByteToWideChar(w32 *w) {
    /* (cp, flags, str, cb, wstr, cch): ASCII/Latin-1, UTF-8 for cp 65001 */
    uint32_t cp = (uint32_t)ARG(0); const uint8_t *s = W32P(w, ARG(2)); int cb = (int)(int32_t)ARG(3);
    uint16_t *d = W32P(w, ARG(4)); int cch = (int)(int32_t)ARG(5);
    if (!s) { w32_set_last_error(w, ERROR_INVALID_PARAMETER); RET(0); return; }
    int n = cb < 0 ? (int)strlen((const char *)s) + 1 : cb, out = 0;
    for (int i = 0; i < n; ) {
        uint32_t ch = s[i++];
        if (cp == 65001 && ch >= 0xC0) {
            int extra = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : 1; ch &= (0x3F >> extra);
            for (int k = 0; k < extra && i < n; k++) ch = (ch << 6) | (s[i++] & 0x3F);
        }
        if (ch >= 0x10000) { if (cch) { if (out + 2 > cch) { w32_set_last_error(w, ERROR_INSUFFICIENT_BUFFER); RET(0); return; } d[out] = (uint16_t)(0xD800 + ((ch - 0x10000) >> 10)); d[out + 1] = (uint16_t)(0xDC00 + (ch & 0x3FF)); } out += 2; }
        else { if (cch) { if (out + 1 > cch) { w32_set_last_error(w, ERROR_INSUFFICIENT_BUFFER); RET(0); return; } d[out] = (uint16_t)ch; } out++; }
    }
    RET(out);
}
static void k_WideCharToMultiByte(w32 *w) {
    /* (cp, flags, wstr, cch, str, cb, defchar, useddef) */
    uint32_t cp = (uint32_t)ARG(0); const uint16_t *s = W32P(w, ARG(2)); int cch = (int)(int32_t)ARG(3);
    uint8_t *d = W32P(w, ARG(4)); int cb = (int)(int32_t)ARG(5);
    if (!s) { w32_set_last_error(w, ERROR_INVALID_PARAMETER); RET(0); return; }
    int n = cch; if (n < 0) { n = 0; while (s[n]) n++; n++; }
    int out = 0;
    for (int i = 0; i < n; i++) {
        uint32_t ch = s[i];
        if (ch >= 0xD800 && ch < 0xDC00 && i + 1 < n) { ch = 0x10000 + ((ch - 0xD800) << 10) + (s[i + 1] - 0xDC00); i++; }
        uint8_t enc[4]; int len;
        if (cp == 65001) {
            if (ch < 0x80) { enc[0] = (uint8_t)ch; len = 1; }
            else if (ch < 0x800) { enc[0] = (uint8_t)(0xC0 | ch >> 6); enc[1] = (uint8_t)(0x80 | (ch & 0x3F)); len = 2; }
            else if (ch < 0x10000) { enc[0] = (uint8_t)(0xE0 | ch >> 12); enc[1] = (uint8_t)(0x80 | ((ch >> 6) & 0x3F)); enc[2] = (uint8_t)(0x80 | (ch & 0x3F)); len = 3; }
            else { enc[0] = (uint8_t)(0xF0 | ch >> 18); enc[1] = (uint8_t)(0x80 | ((ch >> 12) & 0x3F)); enc[2] = (uint8_t)(0x80 | ((ch >> 6) & 0x3F)); enc[3] = (uint8_t)(0x80 | (ch & 0x3F)); len = 4; }
        } else { enc[0] = ch < 256 ? (uint8_t)ch : '?'; len = 1; }
        if (cb) { if (out + len > cb) { w32_set_last_error(w, ERROR_INSUFFICIENT_BUFFER); RET(0); return; } memcpy(d + out, enc, len); }
        out += len;
    }
    RET(out);
}
static void k_GetStringTypeW(w32 *w) {
    /* (type, wstr, cch, out): C1 flags, ASCII only */
    const uint16_t *s = W32P(w, ARG(1)); int n = (int)(int32_t)ARG(2); uint16_t *o = W32P(w, ARG(3));
    if (n < 0) { n = 0; while (s[n]) n++; }
    for (int i = 0; i < n; i++) {
        uint32_t c = s[i], t = 0;
        if (c >= 'A' && c <= 'Z') t |= 0x1 | 0x100;
        if (c >= 'a' && c <= 'z') t |= 0x2 | 0x100;
        if (c >= '0' && c <= '9') t |= 0x4 | 0x80;
        if (c == ' ' || (c >= 9 && c <= 13)) t |= 0x8 | 0x40;
        if (c < 32 || c == 127) t |= 0x20;
        if (c > 32 && c < 127 && !(t & 0x107)) t |= 0x10;
        o[i] = (uint16_t)t;
    }
    RET(1);
}
static void k_LCMapStringW(w32 *w) { RET(0); }
static void k_CompareStringW(w32 *w) {
    const uint16_t *a = W32P(w, ARG(2)), *b = W32P(w, ARG(4)); int na = (int)(int32_t)ARG(3), nb = (int)(int32_t)ARG(5);
    if (na < 0) { na = 0; while (a[na]) na++; } if (nb < 0) { nb = 0; while (b[nb]) nb++; }
    int i = 0; for (; i < na && i < nb; i++) if (a[i] != b[i]) { RET(a[i] < b[i] ? 1 : 3); return; }
    RET(na == nb ? 2 : na < nb ? 1 : 3);
}

/* ---- memory ---- */
static uint64_t valloc_(w32 *w, uint64_t addr, uint64_t size, uint32_t type) {
    if (w->is32) { addr = (uint32_t)addr; size = (uint32_t)size; }
    uint64_t r = 0;
    if (addr) {
        uint64_t a = addr & ~0xFFFull, end = (addr + size + 0xFFF) & ~0xFFFull;
        /* commit on an already-reserved range is the common case: pages are ours already */
        r = w32_alloc_at(w, a, end - a, 1) ? a : (type & 0x1000 ? a : 0);
    } else r = w32_alloc(w, size, 1);
    if (!r) w32_set_last_error(w, ERROR_NOT_ENOUGH_MEMORY);
    return r;
}
static void k_VirtualAlloc(w32 *w) { RET(valloc_(w, ARG(0), ARG(1), (uint32_t)ARG(2))); }
static void k_VirtualAllocEx(w32 *w) { RET(valloc_(w, ARG(1), ARG(2), (uint32_t)ARG(3))); }
static void k_VirtualFree(w32 *w) {
    uint64_t addr = ARG(0), size = ARG(1); uint32_t type = (uint32_t)ARG(2);
    if (type == 0x8000 /* MEM_RELEASE */ || !w->is32) {
        /* we do not track sizes for VirtualAlloc; releasing a whole region without a
         * size is only possible for the last allocation -- accept and leak otherwise */
        if (size && !w->is32) munmap(W32P(w, addr & ~0xFFFull), (size + 0xFFF) & ~0xFFFull);
    } else if (size) mmap(W32P(w, addr & ~0xFFFull), (size + 0xFFF) & ~0xFFFull, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    RET(1);
}
static int prot_of(uint32_t p) {
    switch (p & 0xFF) {
    case 0x01: return PROT_NONE;
    case 0x02: return PROT_READ;
    case 0x04: case 0x08: return PROT_READ | PROT_WRITE;
    case 0x10: return PROT_EXEC | PROT_READ;
    case 0x20: return PROT_EXEC | PROT_READ;
    default:   return PROT_EXEC | PROT_READ | PROT_WRITE;
    }
}
static void k_VirtualProtect(w32 *w) {
    uint64_t addr = ARG(0), size = ARG(1); uint32_t np = (uint32_t)ARG(2), old = ARG(3);
    uint64_t a = addr & ~0xFFFull;
    /* the JIT keeps code in the interpreter's block cache: a page that becomes writable may change */
    xc_cache_invalidate(a, a + ((addr + size + 0xFFF) & ~0xFFFull) - a);
    mprotect(W32P(w, a), (addr + size + 0xFFF) / 0x1000 * 0x1000 - a, prot_of(np) | PROT_READ);
    if (old) w32_write(w, old, 4, 0x40);
    RET(1);
}
static void k_VirtualQuery(w32 *w) {
    uint64_t addr = ARG(0), out = ARG(1); int psz = (int)w32_ptrsize(w);
    if (w->is32) addr = (uint32_t)addr;
    uint64_t base = addr & ~0xFFFull, size = 0x1000, state = 0x1000 /* MEM_COMMIT */, prot = 0x40;
    if (addr >= w->image_base && addr < w->image_base + w->image_size) { base = w->image_base; size = w->image_size; }
    else if (addr >= w->stack_limit && addr < w->stack_base) { base = w->stack_limit; size = w->stack_base - w->stack_limit; prot = 0x04; }
    /* MEMORY_BASIC_INFORMATION: BaseAddress, AllocationBase, AllocationProtect, [pad], RegionSize, State, Protect, Type */
    memset(W32P(w, out), 0, w->is32 ? 28 : 48);
    w32_write(w, out, psz, base); w32_write(w, out + psz, psz, base); w32_write(w, out + 2 * psz, 4, prot);
    uint64_t rs = w->is32 ? 12 : 24;
    w32_write(w, out + rs, psz, size); w32_write(w, out + rs + psz, 4, state); w32_write(w, out + rs + psz + 4, 4, prot);
    w32_write(w, out + rs + psz + 8, 4, 0x20000 /* MEM_PRIVATE */);
    RET(w->is32 ? 28 : 48);
}
static void k_GetProcessHeap(w32 *w) { RET(w32_read(w, w->peb + (w->is32 ? 0x18 : 0x30), (int)w32_ptrsize(w))); }
static void k_HeapCreate(w32 *w) { RET(w32_handle_new(w, H_HEAP, -1)); }
static void k_HeapDestroy(w32 *w) { RET(1); }
static void k_HeapAlloc(w32 *w) {
    uint32_t flags = (uint32_t)ARG(1); uint64_t size = ARG(2);
    uint64_t p = w32_heap_alloc(w, size);
    if (p && (flags & 8)) memset(W32P(w, p), 0, size);
    RET(p);
}
static void k_HeapReAlloc(w32 *w) { uint32_t flags = (uint32_t)ARG(1); uint64_t old = ARG(2), size = ARG(3);
    uint64_t osz = w32_heap_size(w, old); uint64_t p = w32_heap_realloc(w, old, size);
    if (p && (flags & 8) && size > osz) memset((uint8_t *)W32P(w, p) + osz, 0, size - osz);
    RET(p); }
static void k_HeapFree(w32 *w) { w32_heap_free(w, ARG(2)); RET(1); }
static void k_HeapSize(w32 *w) { RET(w32_heap_size(w, ARG(2))); }
static void k_HeapValidate(w32 *w) { RET(1); }
static void k_HeapSetInformation(w32 *w) { RET(1); }
static void k_LocalAlloc(w32 *w) { uint64_t p = w32_heap_alloc(w, ARG(1)); RET(p); }
static void k_LocalFree(w32 *w) { w32_heap_free(w, ARG(0)); RET(0); }
static void k_GlobalAlloc(w32 *w) { RET(w32_heap_alloc(w, ARG(1))); }
static void k_GlobalFree(w32 *w) { w32_heap_free(w, ARG(0)); RET(0); }

/* ---- TLS, fibers, critical sections ---- */
static void k_TlsAlloc(w32 *w) {
    for (int i = 1; i < W32_MAX_TLS; i++) if (!(w->tls_used & (1ull << i))) { w->tls_used |= 1ull << i; RET(i); return; }
    RET(0xFFFFFFFFu);
}
static void k_TlsFree(w32 *w) { uint32_t i = (uint32_t)ARG(0); if (i < W32_MAX_TLS) w->tls_used &= ~(1ull << i); RET(1); }
static void k_TlsGetValue(w32 *w) {
    uint32_t i = (uint32_t)ARG(0); int psz = (int)w32_ptrsize(w);
    if (i >= W32_MAX_TLS) { w32_set_last_error(w, ERROR_INVALID_PARAMETER); RET(0); return; }
    w32_set_last_error(w, 0);
    RET(w32_read(w, w->teb + (w->is32 ? TEB32_TLS : TEB64_TLS) + (uint64_t)psz * i, psz));
}
static void k_TlsSetValue(w32 *w) {
    uint32_t i = (uint32_t)ARG(0); int psz = (int)w32_ptrsize(w);
    if (i >= W32_MAX_TLS) { w32_set_last_error(w, ERROR_INVALID_PARAMETER); RET(0); return; }
    w32_write(w, w->teb + (w->is32 ? TEB32_TLS : TEB64_TLS) + (uint64_t)psz * i, psz, ARG(1)); RET(1);
}
static void k_FlsAlloc(w32 *w) { k_TlsAlloc(w); }
static void k_FlsFree(w32 *w) { k_TlsFree(w); }
static void k_FlsGetValue(w32 *w) { k_TlsGetValue(w); }
static void k_FlsSetValue(w32 *w) { k_TlsSetValue(w); }
static void k_nop_true(w32 *w) { RET(1); }
static void k_nop_void(w32 *w) { (void)w; }
static void k_nop_zero(w32 *w) { RET(0); }
static void k_InitializeCriticalSectionEx(w32 *w) { RET(1); }
static void k_TryEnterCriticalSection(w32 *w) { RET(1); }

/* ---- files and console ---- */
static void k_GetStdHandle(w32 *w) {
    uint32_t n = (uint32_t)ARG(0);
    RET(n == (uint32_t)-10 ? 4 : n == (uint32_t)-11 ? 8 : n == (uint32_t)-12 ? 12 : (w32_set_last_error(w, ERROR_INVALID_HANDLE), (uint64_t)(w->is32 ? 0xFFFFFFFFu : ~0ull)));
}
static void k_SetStdHandle(w32 *w) { RET(1); }
static void k_WriteFile(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0));
    const void *buf = W32P(w, ARG(1)); uint32_t n = (uint32_t)ARG(2); uint64_t written = ARG(3);
    if (!h || h->type != H_FILE) { w32_set_last_error(w, ERROR_INVALID_HANDLE); RET(0); return; }
    ssize_t r = n ? write(h->fd, buf, n) : 0;
    if (r < 0) { w32_set_last_error(w, ERROR_ACCESS_DENIED); RET(0); return; }
    if (written) w32_write(w, written, 4, (uint64_t)r);
    RET(1);
}
static void k_WriteConsoleA(w32 *w) { k_WriteFile(w); }
static void k_WriteConsoleW(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0)); const uint16_t *s = W32P(w, ARG(1)); uint32_t n = (uint32_t)ARG(2);
    if (!h) { RET(0); return; }
    char *buf = malloc(n * 3 + 1); size_t o = 0;
    for (uint32_t i = 0; i < n; i++) { uint32_t c = s[i]; if (c < 0x80) buf[o++] = (char)c; else if (c < 0x800) { buf[o++] = (char)(0xC0 | c >> 6); buf[o++] = (char)(0x80 | (c & 0x3F)); } else { buf[o++] = (char)(0xE0 | c >> 12); buf[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); buf[o++] = (char)(0x80 | (c & 0x3F)); } }
    if (write(h->fd, buf, o) < 0) { /* console gone */ } free(buf);
    if (ARG(3)) w32_write(w, ARG(3), 4, n);
    RET(1);
}
static void k_ReadFile(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0)); void *buf = W32P(w, ARG(1)); uint32_t n = (uint32_t)ARG(2); uint64_t got = ARG(3);
    if (!h || h->type != H_FILE) { w32_set_last_error(w, ERROR_INVALID_HANDLE); RET(0); return; }
    ssize_t r = read(h->fd, buf, n);
    if (r < 0) { w32_set_last_error(w, ERROR_ACCESS_DENIED); RET(0); return; }
    if (got) w32_write(w, got, 4, (uint64_t)r);
    RET(1);
}
static void k_CreateFileA(w32 *w) {
    /* (name, access, share, sa, disposition, flags, template) */
    char path[4096]; host_path(w, GSTR(ARG(0)), path, sizeof path);
    uint32_t access = (uint32_t)ARG(1), disp = (uint32_t)ARG(4);
    int fl = (access & 0x40000000) ? ((access & 0x80000000u) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) { case 1: fl |= O_CREAT | O_EXCL; break; case 2: fl |= O_CREAT | O_TRUNC; break; case 4: fl |= O_CREAT; break; case 5: fl |= O_TRUNC; break; default: break; }
    int fd = open(path, fl, 0644);
    if (fd < 0) { w32_set_last_error(w, errno == ENOENT ? ERROR_FILE_NOT_FOUND : errno == EEXIST ? ERROR_ALREADY_EXISTS : ERROR_ACCESS_DENIED); RET(w->is32 ? 0xFFFFFFFFu : ~0ull); return; }
    RET(w32_handle_new(w, H_FILE, fd));
}
static void k_CreateFileW(w32 *w) {
    char name[4096]; w32_wtoa(w, ARG(0), name, sizeof name);
    uint64_t saved = ARG(0);
    /* reuse the A version by pointing arg 0 at a temporary guest string */
    uint64_t tmp = w32_strdup(w, name);
    if (w->is32) w32_write(w, w->c->gpr[XC_RSP] + 4, 4, tmp); else w->c->gpr[XC_RCX] = tmp;
    k_CreateFileA(w);
    w32_heap_free(w, tmp);
    (void)saved;
}
static void k_CloseHandle(w32 *w) { w32_handle_close(w, ARG(0)); RET(1); }
static void k_GetFileType(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0));
    if (!h) { RET(0); return; }
    struct stat st; if (fstat(h->fd, &st)) { RET(0); return; }
    RET(S_ISCHR(st.st_mode) ? 2 /* FILE_TYPE_CHAR */ : S_ISFIFO(st.st_mode) ? 3 : 1);
}
static void k_GetFileSize(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0)); struct stat st;
    if (!h || fstat(h->fd, &st)) { RET(0xFFFFFFFFu); return; }
    if (ARG(1)) w32_write(w, ARG(1), 4, (uint64_t)st.st_size >> 32);
    RET((uint32_t)st.st_size);
}
static void k_GetFileSizeEx(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0)); struct stat st;
    if (!h || fstat(h->fd, &st)) { RET(0); return; }
    w32_write(w, ARG(1), 8, (uint64_t)st.st_size); RET(1);
}
static void k_SetFilePointer(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0)); int64_t dist = (int32_t)ARG(1); uint64_t hi = ARG(2); uint32_t whence = (uint32_t)ARG(3);
    if (hi) dist = (int64_t)(((uint64_t)w32_read(w, hi, 4) << 32) | (uint32_t)dist);
    if (!h) { RET(0xFFFFFFFFu); return; }
    off_t r = lseek(h->fd, dist, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
    if (hi) w32_write(w, hi, 4, (uint64_t)r >> 32);
    RET((uint32_t)r);
}
static void k_SetFilePointerEx(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0)); int64_t dist = (int64_t)(w->is32 ? (ARG(1) | ARG(2) << 32) : ARG(1));
    uint64_t out = w->is32 ? ARG(3) : ARG(2); uint32_t whence = (uint32_t)(w->is32 ? ARG(4) : ARG(3));
    if (!h) { RET(0); return; }
    off_t r = lseek(h->fd, dist, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
    if (out) w32_write(w, out, 8, (uint64_t)r);
    RET(1);
}
static void k_FlushFileBuffers(w32 *w) { RET(1); }
static void k_GetConsoleMode(w32 *w) { w32_handle *h = w32_handle_get(w, ARG(0)); if (h && isatty(h->fd)) { w32_write(w, ARG(1), 4, 3); RET(1); } else { w32_set_last_error(w, ERROR_INVALID_HANDLE); RET(0); } }
static void k_SetConsoleMode(w32 *w) { RET(1); }
static void k_GetConsoleScreenBufferInfo(w32 *w) { RET(0); }
static void k_SetConsoleCtrlHandler(w32 *w) { RET(1); }
static void k_GetFileAttributesA(w32 *w) {
    char path[4096]; host_path(w, GSTR(ARG(0)), path, sizeof path); struct stat st;
    if (stat(path, &st)) { w32_set_last_error(w, ERROR_FILE_NOT_FOUND); RET(0xFFFFFFFFu); return; }
    RET(S_ISDIR(st.st_mode) ? 0x10 : 0x80);
}
static void k_DeleteFileA(w32 *w) { char path[4096]; host_path(w, GSTR(ARG(0)), path, sizeof path); RET(bool_(unlink(path) == 0)); }
static void k_GetCurrentDirectoryA(w32 *w) {
    uint32_t n = (uint32_t)ARG(0); uint64_t buf = ARG(1); const char *d = "C:\\xcore";
    if (n <= strlen(d)) { RET(strlen(d) + 1); return; }
    memcpy(W32P(w, buf), d, strlen(d) + 1); RET(strlen(d));
}
static void k_SetCurrentDirectoryA(w32 *w) { RET(1); }
static void k_GetTempPathA(w32 *w) { uint32_t n = (uint32_t)ARG(0); const char *d = "C:\\Temp\\"; if (n > strlen(d)) memcpy(W32P(w, ARG(1)), d, strlen(d) + 1); RET(strlen(d)); }
static void k_GetFullPathNameA(w32 *w) {
    const char *s = GSTR(ARG(0)); uint32_t n = (uint32_t)ARG(1); uint64_t buf = ARG(2);
    char full[4096]; if (s[1] == ':') snprintf(full, sizeof full, "%s", s); else snprintf(full, sizeof full, "C:\\xcore\\%s", s);
    if (n <= strlen(full)) { RET(strlen(full) + 1); return; }
    memcpy(W32P(w, buf), full, strlen(full) + 1);
    if (ARG(3)) { const char *b = strrchr(full, '\\'); w32_write(w, ARG(3), (int)w32_ptrsize(w), buf + (b ? (uint64_t)(b - full + 1) : 0)); }
    RET(strlen(full));
}
static void k_FormatMessageA(w32 *w) {
    /* (flags, source, msgid, langid, buf, size, args) -> a generic text */
    uint64_t buf = ARG(4); uint32_t n = (uint32_t)ARG(5);
    char s[64]; snprintf(s, sizeof s, "Error %u", (unsigned)ARG(2));
    if (n > strlen(s)) { memcpy(W32P(w, buf), s, strlen(s) + 1); RET(strlen(s)); } else RET(0);
}

/* ---- threads: one, this one ---- */
static void k_GetThreadPriority(w32 *w) { RET(0); }
static void k_SetThreadPriority(w32 *w) { RET(1); }
static void k_GetExitCodeProcess(w32 *w) { w32_write(w, ARG(1), 4, 0); RET(1); }
static void k_CreateEventA(w32 *w) { RET(w32_handle_new(w, H_EVENT, -1)); }
static void k_CreateEventW(w32 *w) { RET(w32_handle_new(w, H_EVENT, -1)); }
static void k_CreateMutexA(w32 *w) { RET(w32_handle_new(w, H_MUTEX, -1)); }
static void k_SetEvent(w32 *w) { RET(1); }
static void k_ResetEvent(w32 *w) { RET(1); }
static void k_WaitForSingleObject(w32 *w) { RET(0); }
static void k_ReleaseMutex(w32 *w) { RET(1); }
static void k_GetProcessAffinityMask(w32 *w) { w32_write(w, ARG(1), (int)w32_ptrsize(w), 0xF); w32_write(w, ARG(2), (int)w32_ptrsize(w), 0xF); RET(1); }
static void k_SetErrorMode(w32 *w) { RET(0); }
static void k_RtlCaptureContext(w32 *w) { memset(W32P(w, ARG(0)), 0, w->is32 ? 716 : 1232); }
static void k_RtlPcToFileHeader(w32 *w) { w32_write(w, ARG(1), (int)w32_ptrsize(w), w->image_base); RET(w->image_base); }
static void k_RtlLookupFunctionEntry(w32 *w) { RET(0); }
static void k_RtlVirtualUnwind(w32 *w) { RET(0); }
static void k_RtlUnwindEx(w32 *w) { fprintf(stderr, "winrun: RtlUnwindEx: exception unwinding is not supported\n"); w32_exit(w, 129); }
static void k_RaiseException(w32 *w) { fprintf(stderr, "winrun: RaiseException(%#x)\n", (unsigned)ARG(0)); w32_exit(w, 129); }
static void k_InterlockedIncrement(w32 *w) { uint64_t p = ARG(0); uint32_t v = (uint32_t)w32_read(w, p, 4) + 1; w32_write(w, p, 4, v); RET(v); }
static void k_InterlockedDecrement(w32 *w) { uint64_t p = ARG(0); uint32_t v = (uint32_t)w32_read(w, p, 4) - 1; w32_write(w, p, 4, v); RET(v); }
static void k_InterlockedExchange(w32 *w) { uint64_t p = ARG(0); uint32_t old = (uint32_t)w32_read(w, p, 4); w32_write(w, p, 4, ARG(1)); RET(old); }
static void k_InterlockedCompareExchange(w32 *w) { uint64_t p = ARG(0); uint32_t old = (uint32_t)w32_read(w, p, 4); if (old == (uint32_t)ARG(2)) w32_write(w, p, 4, ARG(1)); RET(old); }
static void k_EncodePointer(w32 *w) { RET(ARG(0)); }
static void k_DecodePointer(w32 *w) { RET(ARG(0)); }
static void k_InitializeSListHead(w32 *w) { memset(W32P(w, ARG(0)), 0, 16); }
static void k_GetStartupInfoW(w32 *w) { k_GetStartupInfoA(w); }
static void k_SetHandleCount(w32 *w) { RET(ARG(0)); }
static void k_GetEnvironmentVariableW(w32 *w) { RET(0); }
static void k_GetLogicalDrives(w32 *w) { RET(4); }
static void k_GetDriveTypeA(w32 *w) { RET(3); }
static void k_GetComputerNameA(w32 *w) { const char *n = "XCORE"; memcpy(W32P(w, ARG(0)), n, 6); w32_write(w, ARG(1), 4, 5); RET(1); }
static void k_GetUserNameA(w32 *w) { const char *n = "xcore"; memcpy(W32P(w, ARG(0)), n, 6); w32_write(w, ARG(1), 4, 6); RET(1); }
static void k_lstrlenA(w32 *w) { RET(strlen(GSTR(ARG(0)))); }
static void k_lstrlenW(w32 *w) { RET(w32_wcslen(w, ARG(0))); }
static void k_lstrcpyA(w32 *w) { strcpy(W32P(w, ARG(0)), GSTR(ARG(1))); RET(ARG(0)); }
static void k_lstrcmpiA(w32 *w) { RET((uint64_t)(int64_t)strcasecmp(GSTR(ARG(0)), GSTR(ARG(1)))); }
static void k_GetSystemDirectoryA(w32 *w) { const char *d = "C:\\Windows\\System32"; if ((uint32_t)ARG(1) > strlen(d)) memcpy(W32P(w, ARG(0)), d, strlen(d) + 1); RET(strlen(d)); }
static void k_GetWindowsDirectoryA(w32 *w) { const char *d = "C:\\Windows"; if ((uint32_t)ARG(1) > strlen(d)) memcpy(W32P(w, ARG(0)), d, strlen(d) + 1); RET(strlen(d)); }
static void k_IsProcessorFeaturePresent(w32 *w) { uint32_t f = (uint32_t)ARG(0); RET(bool_(f == 6 || f == 10 || f == 13 || f == 17 || f == 23)); }   /* SSE, SSE2, SSE3, SSE4, fastfail */
static void k_GetCurrentProcessorNumber(w32 *w) { RET(0); }

#define F(n, a)        { #n, a, 0, k_##n, 0 }
#define FN(n, a, impl) { #n, a, 0, impl, 0 }
const w32_api w32_kernel32[] = {
    F(ExitProcess, 1), F(TerminateProcess, 2), F(GetCurrentProcess, 0), F(GetCurrentProcessId, 0), F(GetCurrentThread, 0), F(GetCurrentThreadId, 0),
    F(GetCommandLineA, 0), F(GetCommandLineW, 0), F(GetLastError, 0), F(SetLastError, 1), F(GetStartupInfoA, 1), F(GetStartupInfoW, 1),
    F(GetEnvironmentStringsA, 0), F(GetEnvironmentStringsW, 0), FN(FreeEnvironmentStringsA, 1, k_FreeEnvironmentStrings), FN(FreeEnvironmentStringsW, 1, k_FreeEnvironmentStrings),
    FN(GetEnvironmentStrings, 0, k_GetEnvironmentStringsA), F(GetEnvironmentVariableA, 3), F(GetEnvironmentVariableW, 3),
    F(GetModuleHandleA, 1), F(GetModuleHandleW, 1), F(GetModuleHandleExW, 3), F(LoadLibraryA, 1), F(LoadLibraryW, 1), F(LoadLibraryExA, 3), F(LoadLibraryExW, 3),
    F(FreeLibrary, 1), F(GetProcAddress, 2), F(GetModuleFileNameA, 3), F(GetModuleFileNameW, 3),
    F(IsDebuggerPresent, 0), F(OutputDebugStringA, 1), F(SetUnhandledExceptionFilter, 1), F(UnhandledExceptionFilter, 1),
    F(GetSystemInfo, 1), F(GetNativeSystemInfo, 1), F(GetVersion, 0), F(GetVersionExA, 1), F(GetVersionExW, 1),
    F(Sleep, 1), F(GetTickCount, 0), F(GetTickCount64, 0), F(QueryPerformanceCounter, 1), F(QueryPerformanceFrequency, 1),
    F(GetSystemTimeAsFileTime, 1), F(GetSystemTimePreciseAsFileTime, 1), F(GetLocalTime, 1), F(GetSystemTime, 1), F(GetTimeZoneInformation, 1),
    F(GetACP, 0), F(GetOEMCP, 0), F(GetConsoleCP, 0), F(GetConsoleOutputCP, 0), F(IsValidCodePage, 1), F(GetCPInfo, 2),
    F(GetUserDefaultLCID, 0), F(GetUserDefaultLangID, 0), F(GetSystemDefaultLCID, 0), F(GetThreadLocale, 0),
    F(IsDBCSLeadByteEx, 2), F(IsDBCSLeadByte, 1), F(MultiByteToWideChar, 6), F(WideCharToMultiByte, 8), F(GetStringTypeW, 4), F(LCMapStringW, 6), F(CompareStringW, 6),
    F(VirtualAlloc, 4), F(VirtualAllocEx, 5), F(VirtualFree, 3), F(VirtualProtect, 4), F(VirtualQuery, 3),
    F(GetProcessHeap, 0), F(HeapCreate, 3), F(HeapDestroy, 1), F(HeapAlloc, 3), F(HeapReAlloc, 4), F(HeapFree, 3), F(HeapSize, 3), F(HeapValidate, 3), F(HeapSetInformation, 4),
    F(LocalAlloc, 2), F(LocalFree, 1), F(GlobalAlloc, 2), F(GlobalFree, 1),
    F(TlsAlloc, 0), F(TlsFree, 1), F(TlsGetValue, 1), F(TlsSetValue, 2), F(FlsAlloc, 1), F(FlsFree, 1), F(FlsGetValue, 1), F(FlsSetValue, 2),
    FN(InitializeCriticalSection, 1, k_nop_void), FN(InitializeCriticalSectionAndSpinCount, 2, k_nop_true), F(InitializeCriticalSectionEx, 3),
    FN(DeleteCriticalSection, 1, k_nop_void), FN(EnterCriticalSection, 1, k_nop_void), FN(LeaveCriticalSection, 1, k_nop_void), F(TryEnterCriticalSection, 1),
    FN(InitializeSRWLock, 1, k_nop_void), FN(AcquireSRWLockExclusive, 1, k_nop_void), FN(ReleaseSRWLockExclusive, 1, k_nop_void),
    FN(AcquireSRWLockShared, 1, k_nop_void), FN(ReleaseSRWLockShared, 1, k_nop_void), FN(InitOnceExecuteOnce, 4, k_nop_true),
    FN(InitializeConditionVariable, 1, k_nop_void), FN(WakeAllConditionVariable, 1, k_nop_void), FN(WakeConditionVariable, 1, k_nop_void),
    F(GetStdHandle, 1), F(SetStdHandle, 2), F(WriteFile, 5), F(WriteConsoleA, 5), F(WriteConsoleW, 5), F(ReadFile, 5),
    F(CreateFileA, 7), F(CreateFileW, 7), F(CloseHandle, 1), F(GetFileType, 1), F(GetFileSize, 2), F(GetFileSizeEx, 2),
    F(SetFilePointer, 4), F(SetFilePointerEx, 5), F(FlushFileBuffers, 1), F(GetConsoleMode, 2), F(SetConsoleMode, 2), F(GetConsoleScreenBufferInfo, 2), F(SetConsoleCtrlHandler, 2),
    F(GetFileAttributesA, 1), F(DeleteFileA, 1), F(GetCurrentDirectoryA, 2), F(SetCurrentDirectoryA, 1), F(GetTempPathA, 2), F(GetFullPathNameA, 4), F(FormatMessageA, 7),
    F(GetThreadPriority, 1), F(SetThreadPriority, 2), F(GetExitCodeProcess, 2), F(CreateEventA, 4), F(CreateEventW, 4), F(CreateMutexA, 3), F(SetEvent, 1), F(ResetEvent, 1),
    F(WaitForSingleObject, 2), F(ReleaseMutex, 1), F(GetProcessAffinityMask, 3), F(SetErrorMode, 1),
    F(RtlCaptureContext, 1), F(RtlPcToFileHeader, 2), F(RtlLookupFunctionEntry, 3), F(RtlVirtualUnwind, 8), F(RtlUnwindEx, 6), F(RaiseException, 4),
    F(InterlockedIncrement, 1), F(InterlockedDecrement, 1), F(InterlockedExchange, 2), F(InterlockedCompareExchange, 3),
    F(EncodePointer, 1), F(DecodePointer, 1), F(InitializeSListHead, 1), F(SetHandleCount, 1), F(GetLogicalDrives, 0), F(GetDriveTypeA, 1),
    F(GetComputerNameA, 2), F(GetUserNameA, 2), F(lstrlenA, 1), F(lstrlenW, 1), F(lstrcpyA, 2), F(lstrcmpiA, 2),
    F(GetSystemDirectoryA, 2), F(GetWindowsDirectoryA, 2), F(IsProcessorFeaturePresent, 1), F(GetCurrentProcessorNumber, 0),
    FN(SetConsoleTitleA, 1, k_nop_true), FN(FlushInstructionCache, 3, k_nop_true), FN(GetProcessTimes, 5, k_nop_true), FN(SwitchToThread, 0, k_nop_zero),
    FN(SetThreadAffinityMask, 2, k_nop_true), FN(SetPriorityClass, 2, k_nop_true), FN(GetPriorityClass, 1, k_nop_zero), FN(DisableThreadLibraryCalls, 1, k_nop_true),
    { 0, 0, 0, 0, 0 }
};
#undef F
#undef FN

/* ntdll / user32: placeholders so the tables exist; grow as programs need them */
static void n_RtlGetVersion(w32 *w) { uint64_t p = ARG(0); w32_write(w, p + 4, 4, 10); w32_write(w, p + 8, 4, 0); w32_write(w, p + 12, 4, 19045); w32_write(w, p + 16, 4, 2); RET(0); }
static void n_NtQueryInformationProcess(w32 *w) { RET(0xC0000002u); }
const w32_api w32_ntdll[] = {
    { "RtlGetVersion", 1, 0, n_RtlGetVersion, 0 },
    { "NtQueryInformationProcess", 5, 0, n_NtQueryInformationProcess, 0 },
    { 0, 0, 0, 0, 0 }
};
static void u_MessageBoxA(w32 *w) { fprintf(stderr, "[MessageBox] %s: %s\n", GSTR(ARG(2)), GSTR(ARG(1))); RET(1); }
const w32_api w32_user32[] = {
    { "MessageBoxA", 4, 0, u_MessageBoxA, 0 },
    { 0, 0, 0, 0, 0 }
};
