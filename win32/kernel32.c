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
#include <dirent.h>
#include <ctype.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum { ERROR_FILE_NOT_FOUND = 2, ERROR_ACCESS_DENIED = 5, ERROR_INVALID_HANDLE = 6, ERROR_NOT_ENOUGH_MEMORY = 8,
       ERROR_INVALID_PARAMETER = 87, ERROR_PROC_NOT_FOUND = 127, ERROR_MOD_NOT_FOUND = 126, ERROR_ALREADY_EXISTS = 183,
       ERROR_INSUFFICIENT_BUFFER = 122, ERROR_CALL_NOT_IMPLEMENTED = 120,
       ERROR_FILENAME_EXCED_RANGE = 206,
       /* The three Windows uses for "the caller gave me a pointer I cannot
        * use": a buffer that cannot be read or written, a range that is not
        * the caller's to free or protect, and everything else. */
       ERROR_INVALID_ADDRESS = 487, ERROR_NOACCESS = 998, ERROR_INVALID_USER_BUFFER = 1784 };

static uint64_t bool_(int b) { return b ? 1 : 0; }
static uint64_t filetime_now(void) {
    struct timeval tv; gettimeofday(&tv, 0);
    return ((uint64_t)tv.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)tv.tv_usec * 10;
}
static uint64_t ticks_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000; }
static uint64_t ticks_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }

/* Where C:\ is. The app points this at its own storage, so a game copied in
 * from Files can open its data the way it expects to. Empty (the command-line
 * tool's default) keeps the older behaviour, which the test suite relies on. */
static char g_drive_c[1024];
void w32_set_drive_c(const char *path) { snprintf(g_drive_c, sizeof g_drive_c, "%s", path ? path : ""); }
const char *w32_drive_c(void) { return g_drive_c; }

/* The current directory. A Windows program starts in its own install folder
 * and opens "data\\x.bsa" expecting that, so that is where this begins -- but
 * it is a variable and not a rule, because an installer changes directory and
 * then opens things relative to the new one. SetCurrentDirectory used to
 * return success without doing anything, which is the kind of lie that shows
 * up later as a file not found in a place nobody looked.
 *
 * Two forms are kept: the host path relative paths are resolved against
 * (empty meaning "the executable's directory", which is what every recorded
 * test expects), and the Windows path GetCurrentDirectory reports back. */
static char g_cwd_host[1024];
static char g_cwd_win[1024];

/* The directory the executable lives in, which is where the working
 * directory starts. */
/* A path is a path, not an essay: bounding each half explicitly is what lets
 * the compiler see that joining them cannot overrun the caller's buffer, and
 * truncating an absurd path is the right behaviour anyway. */
#define P_DIR  1000
#define P_REST 2000
static void exe_dir(w32 *w, char *out, size_t n) {
    const char *slash = w->exe_path ? strrchr(w->exe_path, '/') : 0;
    if (slash) snprintf(out, n, "%.*s", (int)(slash - w->exe_path) > P_DIR ? P_DIR : (int)(slash - w->exe_path), w->exe_path);
    else snprintf(out, n, ".");
}

/* Windows path -> host path: flip the slashes, then decide what the root is.
 *
 *   C:\xcore\...   the executable's own directory (what winrun has always
 *                  reported as its location, so the guests' recorded output
 *                  keeps working)
 *   C:\...         under the drive root when one is set, else relative --
 *                  a real program's absolute paths have to land somewhere
 *   anything else  relative to the executable's directory, because that is
 *                  where a Windows program is started
 */
static void host_path(w32 *w, const char *win, char *out, size_t n) {
    char tmp[4096]; size_t i = 0;
    const char *p = win;
    int had_drive = ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':';
    if (had_drive) p += 2;
    for (; *p && i + 1 < sizeof tmp; p++) tmp[i++] = *p == '\\' ? '/' : *p;
    tmp[i] = 0;

    char dir[2048];
    if (!strncasecmp(tmp, "/xcore/", 7)) { exe_dir(w, dir, sizeof dir); snprintf(out, n, "%.*s/%.*s", P_DIR, dir, P_REST, tmp + 7); return; }
    if (had_drive) {
        if (g_drive_c[0]) snprintf(out, n, "%.*s/%.*s", P_DIR, g_drive_c, P_REST, tmp[0] == '/' ? tmp + 1 : tmp);
        else snprintf(out, n, "%.*s", P_REST, tmp[0] == '/' ? tmp + 1 : tmp);
        return;
    }
    if (tmp[0] == '/') { snprintf(out, n, "%.*s", P_REST, tmp); return; }   /* already a host path */
    if (g_cwd_host[0]) { snprintf(out, n, "%.*s/%.*s", P_DIR, g_cwd_host, P_REST, tmp); return; }
    exe_dir(w, dir, sizeof dir);
    snprintf(out, n, "%.*s/%.*s", P_DIR, dir, P_REST, tmp);
}

/* The same, for the other files in this layer: shell32 has to resolve the
 * folders it reports back to real directories. One set of rules for where a
 * Windows path lands, in one place, rather than a second set that drifts. */
void w32_host_path(w32 *w, const char *win, char *out, size_t n) { host_path(w, win, out, n); }

/* Create the directories a Windows program expects to already exist.
 *
 * Only called when something is about to be installed, never for an ordinary
 * run: a directory walk of C:\ is observable, so conjuring six folders into
 * a drive that a test recorded the contents of would change that recording
 * for no reason. An installer, on the other hand, will refuse to start
 * without somewhere to install to.
 */
void w32_drive_init(void) {
    if (!g_drive_c[0]) return;
    static const char *dirs[] = {
        "Program Files", "Program Files (x86)", "Program Files/Common Files",
        "Windows", "Windows/System32", "Windows/Temp", "Windows/Fonts",
        "ProgramData", "Temp",
        "Users", "Users/Winios", "Users/Winios/Desktop", "Users/Winios/Documents",
        "Users/Winios/AppData", "Users/Winios/AppData/Roaming",
        "Users/Winios/AppData/Local", "Users/Winios/AppData/LocalLow",
        "Users/Winios/Saved Games", "Users/Winios/Start Menu",
        "Users/Public", "Users/Public/Documents",
    };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        char path[P_DIR + P_REST + 8];
        snprintf(path, sizeof path, "%.*s/%.*s", P_DIR, g_drive_c, P_REST, dirs[i]);
        (void)mkdir(path, 0777);      /* already there is the normal case */
    }
}

/* ---- finding files, and mapping them ----
 *
 * How a game opens its data. FindFirstFile walks a directory against a
 * wildcard; MapViewOfFile puts a file in the address space so a multi-gigabyte
 * archive does not have to be read to be used. Both were on the list of things
 * a game-shaped program asked for and got nothing.
 */
enum { FA_READONLY = 0x01, FA_DIRECTORY = 0x10, FA_NORMAL = 0x80 };
enum { ERR_FILE_NOT_FOUND = 2, ERR_NO_MORE_FILES = 18, ERR_INVALID_HANDLE = 6 };

/* DOS wildcards: * any run, ? any one, case-insensitive. Recursive because a
 * pattern is short and the alternative is an explicit backtracking stack for
 * no gain. */
static int wild_match(const char *pat, const char *name) {
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            for (const char *n = name; ; n++) {
                if (wild_match(pat, n)) return 1;
                if (!*n) return 0;
            }
        }
        if (!*name) return 0;
        if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*name)) return 0;
        pat++; name++;
    }
    return !*name;
}

static uint64_t filetime_of(time_t t) { return ((uint64_t)t + 11644473600ull) * 10000000ull; }

/* One directory entry into a WIN32_FIND_DATA. The A and W forms differ only in
 * the name at offset 44, and the layout is the same in both bitnesses (there
 * are no pointers in it), which is why one function does both. */
static int fill_find_data(w32 *w, uint64_t out, int wide, const char *dir, const char *name) {
    /* The whole structure is checked once here rather than field by field:
     * everything below writes into it at a fixed offset, and a caller that
     * cannot receive it is better told the enumeration ended than handed a
     * half-filled record. */
    size_t fdsz = wide ? 592 : 320;
    if (!W32PN(w, out, fdsz)) { w32_set_last_error(w, ERROR_NOACCESS); return 0; }
    memset(W32P(w, out), 0, fdsz);
    char full[4096];
    snprintf(full, sizeof full, "%.*s/%.*s", 2000, dir, 1000, name);
    struct stat st;
    uint32_t attr = FA_NORMAL;
    if (stat(full, &st) == 0) {
        if (S_ISDIR(st.st_mode)) attr = FA_DIRECTORY;
        else if (!(st.st_mode & S_IWUSR)) attr |= FA_READONLY;
        w32_write(w, out + 20, 8, filetime_of(st.st_mtime));      /* ftLastWriteTime */
        w32_write(w, out + 4,  8, filetime_of(st.st_mtime));      /* ftCreationTime */
        w32_write(w, out + 12, 8, filetime_of(st.st_atime));      /* ftLastAccessTime */
        if (!S_ISDIR(st.st_mode)) {
            w32_write(w, out + 28, 4, (uint64_t)st.st_size >> 32);
            w32_write(w, out + 32, 4, (uint32_t)st.st_size);
        }
    }
    w32_write(w, out + 0, 4, attr);
    if (wide) {
        uint16_t *d = W32P(w, out + 44);
        size_t i = 0;
        for (; name[i] && i < 259; i++) d[i] = (uint8_t)name[i];
        d[i] = 0;
    } else {
        char *d = W32P(w, out + 44);
        snprintf(d, 260, "%s", name);
    }
    return 1;
}

/* The next entry matching this handle's pattern, or 0 at the end. "." and ".."
 * are reported, because Windows reports them and code that walks a tree
 * expects to have to skip them. */
static int find_step(w32 *w, w32_handle *h, uint64_t out, int wide) {
    DIR *d = (DIR *)h->p;
    if (!d) return 0;
    const char *pat = (const char *)(uintptr_t)h->u1;
    const char *dir = (const char *)(uintptr_t)h->u2;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!wild_match(pat, e->d_name)) continue;
        return fill_find_data(w, out, wide, dir, e->d_name);
    }
    return 0;
}

/* FindFirstFile(pattern, out). The pattern's last component is the wildcard;
 * everything before it is the directory to walk. */
static uint64_t find_first(w32 *w, const char *winpat, uint64_t out, int wide) {
    char path[4096];
    host_path(w, winpat, path, sizeof path);
    char *slash = strrchr(path, '/');
    char *dir, *pat;
    if (slash) { *slash = 0; dir = path; pat = slash + 1; }
    else { dir = (char *)"."; pat = path; }
    if (!*pat) pat = (char *)"*";

    DIR *d = opendir(dir);
    if (!d) { w32_set_last_error(w, ERR_FILE_NOT_FOUND); return w->is32 ? 0xFFFFFFFFu : ~0ull; }

    uint64_t hv = w32_handle_new(w, H_FIND, -1);
    w32_handle *h = w32_handle_get(w, hv);
    if (!h) { closedir(d); w32_set_last_error(w, ERR_FILE_NOT_FOUND); return w->is32 ? 0xFFFFFFFFu : ~0ull; }
    h->p = d;
    h->u1 = (uint64_t)(uintptr_t)strdup(pat);
    h->u2 = (uint64_t)(uintptr_t)strdup(dir);

    if (!find_step(w, h, out, wide)) {
        w32_handle_close(w, hv);
        w32_set_last_error(w, ERR_FILE_NOT_FOUND);
        return w->is32 ? 0xFFFFFFFFu : ~0ull;            /* INVALID_HANDLE_VALUE */
    }
    return hv;
}
static void k_FindFirstFileA(w32 *w) { RET(find_first(w, GSTR(ARG(0)), ARG(1), 0)); }
static void k_FindFirstFileW(w32 *w) { char b[1024]; w32_wtoa(w, ARG(0), b, sizeof b); RET(find_first(w, b, ARG(1), 1)); }
static void find_next(w32 *w, int wide) {
    w32_handle *h = w32_handle_get(w, ARG(0));
    if (!h || h->type != H_FIND) { w32_set_last_error(w, ERR_INVALID_HANDLE); RET(0); return; }
    if (!find_step(w, h, ARG(1), wide)) { w32_set_last_error(w, ERR_NO_MORE_FILES); RET(0); return; }
    RET(1);
}
static void k_FindNextFileA(w32 *w) { find_next(w, 0); }
static void k_FindNextFileW(w32 *w) { find_next(w, 1); }
static void k_FindClose(w32 *w) { w32_handle_close(w, ARG(0)); RET(1); }

/* CreateFileMapping(hFile, sa, protect, sizeHigh, sizeLow, name). A mapping is
 * a handle over a descriptor and a length; nothing is in the address space
 * until MapViewOfFile. INVALID_HANDLE_VALUE for the file means anonymous,
 * which is how a program asks for shared memory. */
enum { PAGE_READONLY_ = 0x02, PAGE_READWRITE_ = 0x04 };
static void k_CreateFileMappingA(w32 *w) {
    uint64_t hfile = ARG(0), prot = ARG(2);
    uint64_t size = ((uint64_t)(uint32_t)ARG(3) << 32) | (uint32_t)ARG(4);
    int fd = -1;
    w32_handle *f = w32_handle_get(w, hfile);
    if (f && f->type == H_FILE) {
        fd = f->fd;
        if (!size) { struct stat st; if (fstat(fd, &st) == 0) size = (uint64_t)st.st_size; }
    }
    if (!size) { w32_set_last_error(w, 87 /* ERROR_INVALID_PARAMETER */); RET(0); return; }
    uint64_t hv = w32_handle_new(w, H_MAPPING, fd);
    w32_handle *h = w32_handle_get(w, hv);
    if (!h) { RET(0); return; }
    h->u1 = size;
    h->flags = (int)prot;
    RET(hv);
}
static void k_CreateFileMappingW(w32 *w) { k_CreateFileMappingA(w); }

/* MapViewOfFile(hMap, access, offHigh, offLow, bytes) */
enum { FILE_MAP_WRITE_ = 2 };
static void k_MapViewOfFile(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0));
    if (!h || h->type != H_MAPPING) { w32_set_last_error(w, ERR_INVALID_HANDLE); RET(0); return; }
    uint64_t off = ((uint64_t)(uint32_t)ARG(2) << 32) | (uint32_t)ARG(3);
    uint64_t want = ARG(4);
    if (!want) want = h->u1 > off ? h->u1 - off : 0;
    if (!want) { w32_set_last_error(w, 87); RET(0); return; }
    int writable = (h->flags == PAGE_READWRITE_) && (ARG(1) & FILE_MAP_WRITE_);
    int mapped = 0;
    uint64_t a = w32_map_file(w, h->fd, off, want, writable, &mapped);
    if (!a) { w32_set_last_error(w, 8 /* ERROR_NOT_ENOUGH_MEMORY */); RET(0); return; }
    if (!mapped && h->fd >= 0) {
        /* the pages are ordinary memory: fill them so the guest sees the file */
        uint8_t *dst = W32P(w, a);
        uint64_t done = 0;
        while (done < want) {
            ssize_t n = pread(h->fd, dst + done, (size_t)(want - done), (off_t)(off + done));
            if (n <= 0) break;
            done += (uint64_t)n;
        }
    }
    if (w->verbose) fprintf(stderr, "winrun: mapped %llu bytes at %#llx (%s)\n",
                            (unsigned long long)want, (unsigned long long)a, mapped ? "mmap" : "read");
    RET(a);
}
static void k_MapViewOfFileEx(w32 *w) { k_MapViewOfFile(w); }
/* Nothing is unmapped: an address the guest may still hold is safer left
 * readable than returned to the allocator, and the process is about to end. */
static void k_UnmapViewOfFile(w32 *w) { RET(1); }
static void k_FlushViewOfFile(w32 *w) { RET(1); }

/* ---- process ---- */
static void k_ExitProcess(w32 *w) { w32_exit(w, (int)(uint32_t)ARG(0)); }
static void k_TerminateProcess(w32 *w) { w32_exit(w, (int)(uint32_t)ARG(1)); }
static void k_GetCurrentProcess(w32 *w) { RET(w->is32 ? 0xFFFFFFFFu : ~0ull); }
static void k_GetCurrentProcessId(w32 *w) { RET(4242); }
static void k_GetCommandLineA(w32 *w) { RET(w->cmdline); }
static void k_GetCommandLineW(w32 *w) { RET(w->cmdline_w); }
static void k_GetLastError(w32 *w) { RET(w32_read(w, w32_self()->teb + (w->is32 ? TEB32_LASTERROR : TEB64_LASTERROR), 4)); }
static void k_SetLastError(w32 *w) { w32_set_last_error(w, (uint32_t)ARG(0)); }
static void k_GetStartupInfoA(w32 *w) {
    uint64_t p = ARG(0); int psz = (int)w32_ptrsize(w);
    uint64_t size = w->is32 ? 68 : 104;
    /* GetStartupInfo returns void, so an unusable pointer leaves the caller's
     * structure as it found it -- which is the only thing left to do. */
    void *si = W32PN(w, p, size);
    if (!si) return;
    memset(si, 0, size);
    w32_write(w, p, 4, size);
    /* hStdInput/Output/Error at the end */
    uint64_t h = p + size - 3 * psz;
    w32_write(w, h, psz, 4); w32_write(w, h + psz, psz, 8); w32_write(w, h + 2 * psz, psz, 12);
}
static void k_GetEnvironmentStringsA(w32 *w) { RET(w->env_block); }
static void k_GetEnvironmentStringsW(w32 *w) { RET(w->env_block_w); }
static void k_FreeEnvironmentStrings(w32 *w) { RET(1); }
/* Variables set while the process runs.
 *
 * The environment the guest was started with is a block of NAME=VALUE in its
 * own memory, sized once -- so it cannot absorb a new variable, let alone a
 * longer value for one it already has. Rather than reallocate and relocate
 * that block (and leave every pointer a guest may have taken into it
 * dangling), anything set later lives here and is consulted first. An
 * installer sets a handful; sixty-four is not a limit anyone will meet. */
enum { ENV_MAX = 64 };
static struct { char name[128], val[1024]; int used; } g_envset[ENV_MAX];

static int env_find(const char *name) {
    for (int i = 0; i < ENV_MAX; i++)
        if (g_envset[i].used && !strcasecmp(g_envset[i].name, name)) return i;
    return -1;
}
/* The value of a variable, or NULL. Checks what was set at runtime before
 * the block the process started with, because a later set has to win. */
const char *w32_env_lookup(w32 *w, const char *name) {
    int i = env_find(name);
    if (i >= 0) return g_envset[i].val;
    if (!w->env_block) return 0;
    const char *e = W32P(w, w->env_block);
    if (!e) return 0;
    size_t nl = strlen(name);
    for (; *e; e += strlen(e) + 1)
        if (!strncasecmp(e, name, nl) && e[nl] == '=') return e + nl + 1;
    return 0;
}
/* NULL removes it. Returns 0 if there is no room, which is a failure the
 * caller can see rather than a silent no-op. */
int w32_env_set(const char *name, const char *val) {
    if (!name || !*name || strlen(name) >= sizeof g_envset[0].name) return 0;
    int i = env_find(name);
    if (!val) { if (i >= 0) g_envset[i].used = 0; return 1; }
    if (strlen(val) >= sizeof g_envset[0].val) return 0;
    if (i < 0) for (i = 0; i < ENV_MAX && g_envset[i].used; i++) { }
    if (i >= ENV_MAX) return 0;
    snprintf(g_envset[i].name, sizeof g_envset[i].name, "%s", name);
    snprintf(g_envset[i].val, sizeof g_envset[i].val, "%s", val);
    g_envset[i].used = 1;
    return 1;
}

static void k_GetEnvironmentVariableA(w32 *w) {
    const char *name = GSTR(ARG(0)); uint64_t buf = ARG(1); uint32_t n = (uint32_t)ARG(2);
    const char *v = w32_env_lookup(w, name);
    if (!v) { w32_set_last_error(w, 203 /* ERROR_ENVVAR_NOT_FOUND */); RET(0); return; }
    size_t vl = strlen(v);
    if (vl + 1 > n || !buf) { RET(vl + 1); return; }
    void *d = W32PN(w, buf, vl + 1);
    if (!d) { w32_set_last_error(w, ERROR_NOACCESS); RET(0); return; }
    memcpy(d, v, vl + 1);
    RET(vl);
}
static void k_SetEnvironmentVariableA(w32 *w) {
    const char *name = ARG(0) ? GSTR(ARG(0)) : 0;
    RET(bool_(name && w32_env_set(name, ARG(1) ? GSTR(ARG(1)) : 0)));
}
/* A module handle is a loaded guest image's base, or one of the fake pages
 * that stand for a host-implemented DLL. GetModuleHandle never loads: an
 * image that is not there yet is simply absent. */
static uint64_t module_handle(w32 *w, const char *name) {
    for (int i = 0; i < w->nmods; i++) {
        const char *b = name, *p;
        for (p = name; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
        if (!strcasecmp(w->mods[i].name, b)) return w->mods[i].base;
        size_t ln = strlen(w->mods[i].name);
        if (ln > 4 && !strcasecmp(w->mods[i].name + ln - 4, ".dll")
            && !strncasecmp(w->mods[i].name, b, ln - 4) && !b[ln - 4]) return w->mods[i].base;
    }
    return w32_module_handle(w, name);
}
static void k_GetModuleHandleA(w32 *w) {
    uint64_t name = ARG(0);
    if (!name) { RET(w->image_base); return; }
    uint64_t h = module_handle(w, GSTR(name));
    if (!h) w32_set_last_error(w, ERROR_MOD_NOT_FOUND);
    RET(h);
}
static void k_GetModuleHandleW(w32 *w) {
    uint64_t name = ARG(0); char buf[260];
    if (!name) { RET(w->image_base); return; }
    w32_wtoa(w, name, buf, sizeof buf);
    uint64_t h = module_handle(w, buf);
    if (!h) w32_set_last_error(w, ERROR_MOD_NOT_FOUND);
    RET(h);
}
static void k_GetModuleHandleExW(w32 *w) {
    uint64_t name = ARG(1), out = ARG(2); char buf[260]; uint64_t h;
    if (!name) h = w->image_base; else { w32_wtoa(w, name, buf, sizeof buf); h = module_handle(w, buf); }
    w32_write(w, out, (int)w32_ptrsize(w), h);
    RET(bool_(h != 0));
}
/* LoadLibrary does load: a guest DLL found beside the executable is mapped,
 * its own imports resolved, and its DllMain run before this returns -- which
 * is what a plugin host, a mod loader or a late-bound d3d9 expects. */
static void load_library(w32 *w, const char *name) {
    uint64_t h = w32_load_library(w, name);
    if (!h) { w32_set_last_error(w, ERROR_MOD_NOT_FOUND); RET(0); return; }
    w32_attach_modules(w);
    if (w->verbose) fprintf(stderr, "winrun: LoadLibrary(%s) = %#llx\n", name, (unsigned long long)h);
    RET(h);
}
static void k_LoadLibraryA(w32 *w) { load_library(w, GSTR(ARG(0))); }
static void k_LoadLibraryW(w32 *w) { char buf[260]; w32_wtoa(w, ARG(0), buf, sizeof buf); load_library(w, buf); }
static void k_LoadLibraryExA(w32 *w) { load_library(w, GSTR(ARG(0))); }
static void k_LoadLibraryExW(w32 *w) { char buf[260]; w32_wtoa(w, ARG(0), buf, sizeof buf); load_library(w, buf); }
/* Nothing is ever unmapped: a module's code may still be on the stack, and
 * an emulator that keeps a dead image mapped is strictly safer than one that
 * unmaps it under a live return address. */
static void k_FreeLibrary(w32 *w) {
    w32_module *m = w32_module_at(w, ARG(0));
    if (m && m->refs > 0) m->refs--;
    RET(1);
}
static void k_GetProcAddress(w32 *w) {
    uint64_t h = ARG(0), name = ARG(1);
    /* the low word is an ordinal when the high word is zero (MAKEINTRESOURCE) */
    int ordinal = (name >> 16) == 0 ? (int)(name & 0xFFFF) : -1;
    const char *nm = ordinal < 0 ? GSTR(name) : 0;
    uint64_t a = 0;
    if (w32_module_at(w, h)) {
        a = w32_module_export(w, h, nm, ordinal);
    } else {
        /* A built-in DLL's handle: any of them, by name or by ordinal -- a game
         * that loads dsound or xinput lazily asks this way, and used to get
         * NULL for everything but the first four. */
        const char *dn = w32_builtin_dll_name(w, h);
        if (dn && nm) a = w32_stub_for(w, dn, nm);
        else if (dn) { const char *o = w32_ordinal_name(dn, ordinal); if (o) a = w32_stub_for(w, dn, o); }
    }
    if (w->verbose) {
        char ob[16]; if (!nm) snprintf(ob, sizeof ob, "#%d", ordinal);
        fprintf(stderr, "winrun: GetProcAddress(%#llx, %s) = %#llx\n", (unsigned long long)h, nm ? nm : ob, (unsigned long long)a);
    }
    if (!a) w32_set_last_error(w, ERROR_PROC_NOT_FOUND);
    RET(a);
}
/* The DOS path of a loaded module -- the executable when hModule is NULL. */
static void module_file_name(w32 *w, uint64_t h, char *out, size_t n) {
    const char *path = w->exe_path;
    w32_module *m = h ? w32_module_at(w, h) : 0;
    if (m) path = m->path;
    const char *slash = strrchr(path, '/');
    const char *file = slash ? slash + 1 : path;
    size_t room = n > 10 ? n - 10 : 0;                     /* "C:\xcore\" plus the terminator */
    snprintf(out, n, "C:\\xcore\\%.*s", (int)room, file);
}
static void k_GetModuleFileNameA(w32 *w) {
    uint64_t buf = ARG(1); uint32_t n = (uint32_t)ARG(2);
    char s[300]; module_file_name(w, ARG(0), s, sizeof s);
    size_t l = strlen(s); if (l + 1 > n) l = n ? n - 1 : 0;
    char *d = n ? W32PN(w, buf, l + 1) : 0;
    if (n && !d) { w32_set_last_error(w, ERROR_NOACCESS); RET(0); return; }
    if (d) { memcpy(d, s, l); d[l] = 0; }
    RET(l);
}
static void k_GetModuleFileNameW(w32 *w) {
    uint64_t buf = ARG(1); uint32_t n = (uint32_t)ARG(2);
    char s[300]; module_file_name(w, ARG(0), s, sizeof s);
    size_t l = strlen(s); if (l + 1 > n) l = n ? n - 1 : 0;
    uint16_t *d = n ? W32PN(w, buf, 2 * ((uint64_t)l + 1)) : 0;
    if (n && !d) { w32_set_last_error(w, ERROR_NOACCESS); RET(0); return; }
    if (d) { for (size_t i = 0; i < l; i++) d[i] = (uint8_t)s[i]; d[l] = 0; }
    RET(l);
}
static void k_IsDebuggerPresent(w32 *w) { RET(0); }
/* OutputDebugString goes nowhere unless somebody asked to see it.
 *
 * A program calling this in its main loop -- and installers do -- writes a
 * line per iteration, and on a phone each of those crosses into the system
 * log and wakes the host app's UI. That was enough on its own to hold the
 * main thread at 100% until iOS terminated the process, which is a strange
 * way for a debug print to kill a game. */
static void k_OutputDebugStringA(w32 *w) {
    if (w->verbose) fprintf(stderr, "[dbg] %s", GSTR(ARG(0)));
}
static void k_GetSystemInfo(w32 *w) {
    uint64_t p = ARG(0); int psz = (int)w32_ptrsize(w);
    void *si = W32PN(w, p, w->is32 ? 36 : 48);
    if (!si) return;                        /* returns void: nothing to report */
    memset(si, 0, w->is32 ? 36 : 48);
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
static void k_GetTickCount(w32 *w) { RET((uint32_t)ticks_ms()); }
static void k_GetTickCount64(w32 *w) { w32_ret64(w, ticks_ms()); }
static void k_QueryPerformanceCounter(w32 *w) { w32_write(w, ARG(0), 8, ticks_ns()); RET(1); }
static void k_QueryPerformanceFrequency(w32 *w) { w32_write(w, ARG(0), 8, 1000000000ull); RET(1); }
static void k_GetSystemTimeAsFileTime(w32 *w) { w32_write(w, ARG(0), 8, filetime_now()); }
static void k_GetSystemTimePreciseAsFileTime(w32 *w) { k_GetSystemTimeAsFileTime(w); }
static void k_GetLocalTime(w32 *w) {
    time_t t = time(0); struct tm tm; localtime_r(&t, &tm); uint64_t p = ARG(0);
    uint16_t f[8] = { (uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_wday, (uint16_t)tm.tm_mday, (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min, (uint16_t)tm.tm_sec, 0 };
    void *d = W32PN(w, p, sizeof f);
    if (d) memcpy(d, f, sizeof f);
}
static void k_GetSystemTime(w32 *w) { k_GetLocalTime(w); }
/* TIME_ZONE_ID_UNKNOWN is 0 and TIME_ZONE_ID_INVALID is 0xFFFFFFFF, which is
 * the documented answer when the structure cannot be filled in. */
static void k_GetTimeZoneInformation(w32 *w) {
    void *p = W32PN(w, ARG(0), 172);
    if (!p) { RET(0xFFFFFFFFu); return; }
    memset(p, 0, 172);
    RET(0);
}
static void k_GetACP(w32 *w) { RET(1252); }
static void k_GetOEMCP(w32 *w) { RET(437); }
static void k_GetConsoleCP(w32 *w) { RET(437); }
static void k_GetConsoleOutputCP(w32 *w) { RET(437); }
static void k_IsValidCodePage(w32 *w) { RET(1); }
static void k_GetCPInfo(w32 *w) { uint64_t p = ARG(1); void *d = W32PN(w, p, 20);
    if (!d) { w32_set_last_error(w, ERROR_INVALID_PARAMETER); RET(0); return; }
    memset(d, 0, 20); w32_write(w, p, 4, 1); w32_write(w, p + 4, 1, '?'); RET(1); }
static void k_GetUserDefaultLCID(w32 *w) { RET(0x409); }
static void k_AreFileApisANSI(w32 *w) { (void)w; RET(1); }
/* GetUserDefaultLocaleName(buf, cch): "en-US", which is what every other
 * locale answer here agrees with. Returns the length including the
 * terminator, as Windows does. */
static void k_GetUserDefaultLocaleName(w32 *w) {
    static const char name[] = "en-US";
    uint32_t cch = (uint32_t)ARG(1), need = (uint32_t)sizeof name;
    if (cch < need) { w32_set_last_error(w, 122); RET(0); return; }          /* ERROR_INSUFFICIENT_BUFFER */
    if (!w32_mem_ok(w, ARG(0), 2ull * need)) { w32_set_last_error(w, ERROR_NOACCESS); RET(0); return; }
    for (uint32_t i = 0; i < need; i++) w32_write(w, ARG(0) + 2ull * i, 2, (uint8_t)name[i]);
    RET(need);
}
static void k_GetUserDefaultLangID(w32 *w) { RET(0x409); }
static void k_GetSystemDefaultLCID(w32 *w) { RET(0x409); }
static void k_GetThreadLocale(w32 *w) { RET(0x409); }
static void k_IsDBCSLeadByteEx(w32 *w) { RET(0); }
static void k_IsDBCSLeadByte(w32 *w) { RET(0); }
static void k_MultiByteToWideChar(w32 *w) {
    /* (cp, flags, str, cb, wstr, cch): ASCII/Latin-1, UTF-8 for cp 65001 */
    uint32_t cp = (uint32_t)ARG(0); int cb = (int)(int32_t)ARG(3);
    int cch = (int)(int32_t)ARG(5);
    /* A negative cb means "to the terminator", and there is no length to
     * check then -- one byte establishes that the string is in guest memory
     * at all, which is what separates a real pointer from an uninitialised
     * local. The destination has a count, so it is checked in full. */
    const uint8_t *s = W32PN(w, ARG(2), cb < 0 ? 1 : (uint64_t)cb);
    uint16_t *d = cch > 0 ? W32PN(w, ARG(4), 2 * (uint64_t)cch) : 0;
    if (!s || (cch > 0 && !d)) { w32_set_last_error(w, ERROR_INVALID_PARAMETER); RET(0); return; }
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
    uint32_t cp = (uint32_t)ARG(0); int cch = (int)(int32_t)ARG(3);
    int cb = (int)(int32_t)ARG(5);
    const uint16_t *s = W32PN(w, ARG(2), cch < 0 ? 2 : 2 * (uint64_t)cch);
    uint8_t *d = cb > 0 ? W32PN(w, ARG(4), (uint64_t)cb) : 0;
    if (!s || (cb > 0 && !d)) { w32_set_last_error(w, ERROR_INVALID_PARAMETER); RET(0); return; }
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
    int n = (int)(int32_t)ARG(2);
    const uint16_t *s = W32PN(w, ARG(1), n < 0 ? 2 : 2 * (uint64_t)n);
    if (!s) { w32_set_last_error(w, ERROR_INVALID_PARAMETER); RET(0); return; }
    if (n < 0) { n = 0; while (s[n]) n++; }
    uint16_t *o = W32PN(w, ARG(3), 2 * (uint64_t)n);
    if (!o) { w32_set_last_error(w, ERROR_INVALID_PARAMETER); RET(0); return; }
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

/* ---- memory ---- */
/* Host page size: the guest's 4 KB pages are a fiction on Apple silicon (16 KB),
 * and the kernel wants host-page-aligned ranges. */
#define hpage() w32_host_page()
static uint64_t valloc_(w32 *w, uint64_t addr, uint64_t size, uint32_t type) {
    if (w->is32) { addr = (uint32_t)addr; size = (uint32_t)size; }
    uint64_t r = 0;
    if (addr) {
        uint64_t a = addr & ~0xFFFull, end = (addr + size + 0xFFF) & ~0xFFFull;
        /* commit on an already-reserved range is the common case: reserve mapped the
         * pages RW already (we do not do reserve-without-commit), so leave them be --
         * a fresh MAP_FIXED would zero them and, on a 16 KB host, their neighbours */
        uint64_t hp = hpage(), ha = a & ~(hp - 1), he = (end + hp - 1) & ~(hp - 1);
        /* msync answers "is something mapped here?", and for a 64-bit guest
         * something is mapped at a great many host addresses that are not the
         * guest's. Asking the loader's table first keeps a commit at a host
         * address from being reported back as a successful allocation. */
        if ((type & 0x1000) && w32_mem_ok(w, ha, he - ha)
            && msync(W32P(w, ha), he - ha, MS_ASYNC) == 0) r = a;
        else r = w32_alloc_at(w, a, end - a, 1) ? a : (type & 0x1000 ? a : 0);
    } else r = w32_alloc(w, size, 1);
    if (!r) w32_set_last_error(w, ERROR_NOT_ENOUGH_MEMORY);
    return r;
}
static void k_VirtualAlloc(w32 *w) { RET(valloc_(w, ARG(0), ARG(1), (uint32_t)ARG(2))); }
static void k_VirtualAllocEx(w32 *w) { RET(valloc_(w, ARG(1), ARG(2), (uint32_t)ARG(3))); }
static void k_VirtualFree(w32 *w) {
    uint64_t addr = ARG(0), size = ARG(1); uint32_t type = (uint32_t)ARG(2);
    /* munmap and a MAP_FIXED mmap both take effect on whatever is at the
     * address, and for a 64-bit guest the address is a host address: freeing
     * a range that is not the guest's would take the host's own heap or a
     * loaded library out from under it. */
    if (size && !w32_mem_ok(w, addr, size)) {
        w32_set_last_error(w, ERROR_INVALID_ADDRESS); RET(0); return;
    }
    if (type == 0x8000 /* MEM_RELEASE */ || !w->is32) {
        /* we do not track sizes for VirtualAlloc; releasing a whole region without a
         * size is only possible for the last allocation -- accept and leak otherwise */
        if (size && !w->is32) munmap(W32P(w, addr & ~(hpage() - 1)), (size + hpage() - 1) & ~(hpage() - 1));
    } else if (size) {
        /* decommit: only the host pages the range covers entirely -- a MAP_FIXED
         * over a shared host page would take a neighbour's data with it */
        uint64_t a = (addr + hpage() - 1) & ~(hpage() - 1), e = (addr + size) & ~(hpage() - 1);
        if (e > a) mmap(W32P(w, a), e - a, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    }
    RET(1);
}
/* Guest memory is never executed by the host (see w32_alloc), so PAGE_EXECUTE_*
 * maps to the readable/writable part only. */
static int prot_of(uint32_t p) {
    switch (p & 0xFF) {
    case 0x01: return PROT_NONE;
    case 0x02: case 0x10: case 0x20: return PROT_READ;
    default:   return PROT_READ | PROT_WRITE;
    }
}
static void k_VirtualProtect(w32 *w) {
    uint64_t addr = ARG(0), size = ARG(1); uint32_t np = (uint32_t)ARG(2), old = ARG(3);
    /* This is the call the crash report came from. The old-protection
     * out-parameter is written through w32_write, which is checked; the range
     * itself is checked here, because mprotect on a 64-bit guest's address is
     * mprotect on a host address, and making the host's own text writable is
     * not something a guest should be able to ask for. Mappings begin and end
     * on host page boundaries, so a range inside one stays inside it once it
     * is rounded out below. */
    if (!w32_mem_ok(w, addr, size ? size : 1)) {
        w32_set_last_error(w, ERROR_INVALID_ADDRESS); RET(0); return;
    }
    uint64_t a = addr & ~(hpage() - 1), e = (addr + size + hpage() - 1) & ~(hpage() - 1);
    /* the JIT keeps code in the interpreter's block cache: a page that becomes writable may change */
    xc_cache_invalidate(a, e);
    mprotect(W32P(w, a), e - a, prot_of(np) | PROT_READ);
    if (old) w32_write(w, old, 4, 0x40);
    RET(1);
}
static void k_VirtualQuery(w32 *w) {
    uint64_t addr = ARG(0), out = ARG(1); int psz = (int)w32_ptrsize(w);
    if (w->is32) addr = (uint32_t)addr;
    uint64_t base = addr & ~0xFFFull, size = 0x1000, state = 0x1000 /* MEM_COMMIT */, prot = 0x40;
    if (addr >= w->image_base && addr < w->image_base + w->image_size) { base = w->image_base; size = w->image_size; }
    else if (addr >= w32_self()->stack_limit && addr < w32_self()->stack_base) { base = w32_self()->stack_limit; size = w32_self()->stack_base - w32_self()->stack_limit; prot = 0x04; }
    /* MEMORY_BASIC_INFORMATION: BaseAddress, AllocationBase, AllocationProtect, [pad], RegionSize, State, Protect, Type */
    void *mbi = W32PN(w, out, w->is32 ? 28 : 48);
    if (!mbi) { w32_set_last_error(w, ERROR_NOACCESS); RET(0); return; }
    memset(mbi, 0, w->is32 ? 28 : 48);
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
    RET(w32_read(w, w32_self()->teb + (w->is32 ? TEB32_TLS : TEB64_TLS) + (uint64_t)psz * i, psz));
}
static void k_TlsSetValue(w32 *w) {
    uint32_t i = (uint32_t)ARG(0); int psz = (int)w32_ptrsize(w);
    if (i >= W32_MAX_TLS) { w32_set_last_error(w, ERROR_INVALID_PARAMETER); RET(0); return; }
    w32_write(w, w32_self()->teb + (w->is32 ? TEB32_TLS : TEB64_TLS) + (uint64_t)psz * i, psz, ARG(1)); RET(1);
}
static void k_FlsAlloc(w32 *w) { k_TlsAlloc(w); }
static void k_FlsFree(w32 *w) { k_TlsFree(w); }
static void k_FlsGetValue(w32 *w) { k_TlsGetValue(w); }
static void k_FlsSetValue(w32 *w) { k_TlsSetValue(w); }
/* FlsGetValue2 is FlsGetValue that leaves the last error alone. The 2022
 * UCRT reaches its per-thread data through it on every call that has any,
 * so a made-up zero here is a null pointer a few instructions later -- which
 * is how a GameMaker game died at rip 0 after 222,476 calls. */
static void k_FlsGetValue2(w32 *w) {
    uint32_t i = (uint32_t)ARG(0); int psz = (int)w32_ptrsize(w);
    if (i >= W32_MAX_TLS) { RET(0); return; }
    RET(w32_read(w, w32_self()->teb + (w->is32 ? TEB32_TLS : TEB64_TLS) + (uint64_t)psz * i, psz));
}
static void k_DisableThreadLibraryCalls(w32 *w) {
    w32_module *m = w32_module_at(w, ARG(0));
    if (m) m->no_thread_calls = 1;
    RET(m ? 1 : 0);
}
static void k_nop_true(w32 *w) { RET(1); }
static void k_nop_void(w32 *w) { (void)w; }
static void k_nop_zero(w32 *w) { RET(0); }

/* ---- files and console ---- */
static void k_GetStdHandle(w32 *w) {
    uint32_t n = (uint32_t)ARG(0);
    RET(n == (uint32_t)-10 ? 4 : n == (uint32_t)-11 ? 8 : n == (uint32_t)-12 ? 12 : (w32_set_last_error(w, ERROR_INVALID_HANDLE), (uint64_t)(w->is32 ? 0xFFFFFFFFu : ~0ull)));
}
static void k_SetStdHandle(w32 *w) { RET(1); }
static void k_WriteFile(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0));
    uint32_t n = (uint32_t)ARG(2); uint64_t written = ARG(3);
    const void *buf = n ? W32PN(w, ARG(1), n) : 0;
    if (!h || h->type != H_FILE) { w32_set_last_error(w, ERROR_INVALID_HANDLE); RET(0); return; }
    /* ERROR_INVALID_USER_BUFFER is what Windows returns for a buffer it
     * cannot read, and it is the one the caller can act on. */
    if (n && !buf) { w32_set_last_error(w, ERROR_INVALID_USER_BUFFER); RET(0); return; }
    ssize_t r = n ? write(h->fd, buf, n) : 0;
    if (r < 0) { w32_set_last_error(w, ERROR_ACCESS_DENIED); RET(0); return; }
    if (written) w32_write(w, written, 4, (uint64_t)r);
    RET(1);
}
static void k_WriteConsoleA(w32 *w) { k_WriteFile(w); }
static void k_WriteConsoleW(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0)); uint32_t n = (uint32_t)ARG(2);
    const uint16_t *s = W32PN(w, ARG(1), 2 * (uint64_t)n);
    if (!h) { RET(0); return; }
    if (n && !s) { w32_set_last_error(w, ERROR_INVALID_USER_BUFFER); RET(0); return; }
    char *buf = malloc((size_t)n * 3 + 1); size_t o = 0;
    for (uint32_t i = 0; i < n; i++) { uint32_t c = s[i]; if (c < 0x80) buf[o++] = (char)c; else if (c < 0x800) { buf[o++] = (char)(0xC0 | c >> 6); buf[o++] = (char)(0x80 | (c & 0x3F)); } else { buf[o++] = (char)(0xE0 | c >> 12); buf[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); buf[o++] = (char)(0x80 | (c & 0x3F)); } }
    if (write(h->fd, buf, o) < 0) { /* console gone */ } free(buf);
    if (ARG(3)) w32_write(w, ARG(3), 4, n);
    RET(1);
}
static void k_ReadFile(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0)); uint32_t n = (uint32_t)ARG(2); uint64_t got = ARG(3);
    void *buf = n ? W32PN(w, ARG(1), n) : 0;
    if (!h || h->type != H_FILE) { w32_set_last_error(w, ERROR_INVALID_HANDLE); RET(0); return; }
    if (n && !buf) { w32_set_last_error(w, ERROR_INVALID_USER_BUFFER); RET(0); return; }
    ssize_t r = n ? read(h->fd, buf, n) : 0;
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
    if (w->is32) w32_write(w, w32_cpu(w)->gpr[XC_RSP] + 4, 4, tmp); else w32_cpu(w)->gpr[XC_RCX] = tmp;
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
    uint32_t n = (uint32_t)ARG(0); uint64_t buf = ARG(1);
    const char *d = g_cwd_win[0] ? g_cwd_win : "C:\\xcore";
    if (n <= strlen(d)) { RET(strlen(d) + 1); return; }
    void *o = W32PN(w, buf, strlen(d) + 1);
    if (!o) { w32_set_last_error(w, ERROR_NOACCESS); RET(0); return; }
    memcpy(o, d, strlen(d) + 1); RET(strlen(d));
}
static void k_GetCurrentDirectoryW(w32 *w) {
    uint32_t n = (uint32_t)ARG(0); uint64_t buf = ARG(1);
    const char *d = g_cwd_win[0] ? g_cwd_win : "C:\\xcore";
    size_t l = strlen(d);
    if (n <= l) { RET(l + 1); return; }
    uint16_t *o = W32PN(w, buf, 2 * (l + 1));
    if (!o) { w32_set_last_error(w, ERROR_NOACCESS); RET(0); return; }
    for (size_t i = 0; i < l; i++) o[i] = (uint8_t)d[i];
    o[l] = 0;
    RET(l);
}
/* Really change directory, and fail when the directory is not there. The
 * guest-visible form is kept alongside the host one because a program that
 * sets a directory and then asks for it expects its own spelling back. */
static int cwd_change(w32 *w, const char *win) {
    char host[4096];
    host_path(w, win, host, sizeof host);
    struct stat st;
    if (stat(host, &st) || !S_ISDIR(st.st_mode)) {
        w32_set_last_error(w, ERROR_FILE_NOT_FOUND); return 0;
    }
    snprintf(g_cwd_host, sizeof g_cwd_host, "%.*s", (int)sizeof g_cwd_host - 1, host);
    /* An absolute Windows path is reported verbatim; a relative one is
     * resolved against what we had. Built in a temporary first: the source of
     * the join is the destination of it, and snprintf may not overlap. */
    char next[sizeof g_cwd_win];
    const char *at = g_cwd_win[0] ? g_cwd_win : "C:\\xcore";
    int n = win[1] == ':' ? snprintf(next, sizeof next, "%s", win)
                          : snprintf(next, sizeof next, "%s\\%s", at, win);
    /* Truncating a path is worse than refusing one: the caller would go on to
     * open something it did not name. Nothing real is this long. */
    if (n < 0 || (size_t)n >= sizeof next) {
        w32_set_last_error(w, ERROR_FILENAME_EXCED_RANGE); return 0;
    }
    memcpy(g_cwd_win, next, (size_t)n + 1);
    return 1;
}
static void set_cwd(w32 *w, const char *win) { RET(cwd_change(w, win)); }
void w32_set_cwd_win(w32 *w, const char *win) { cwd_change(w, win); }
static void k_SetCurrentDirectoryA(w32 *w) { set_cwd(w, GSTR(ARG(0))); }
static void k_SetCurrentDirectoryW(w32 *w) {
    char s[1024]; w32_wtoa(w, ARG(0), s, sizeof s); set_cwd(w, s);
}
static void k_GetTempPathA(w32 *w) { uint32_t n = (uint32_t)ARG(0); const char *d = "C:\\Temp\\";
    if (n > strlen(d)) { void *o = W32PN(w, ARG(1), strlen(d) + 1);
                         if (!o) { w32_set_last_error(w, ERROR_NOACCESS); RET(0); return; }
                         memcpy(o, d, strlen(d) + 1); }
    RET(strlen(d)); }
static void k_GetFullPathNameA(w32 *w) {
    const char *s = GSTR(ARG(0)); uint32_t n = (uint32_t)ARG(1); uint64_t buf = ARG(2);
    char full[4096]; if (s[1] == ':') snprintf(full, sizeof full, "%s", s); else snprintf(full, sizeof full, "C:\\xcore\\%s", s);
    if (n <= strlen(full)) { RET(strlen(full) + 1); return; }
    void *o = W32PN(w, buf, strlen(full) + 1);
    if (!o) { w32_set_last_error(w, ERROR_NOACCESS); RET(0); return; }
    memcpy(o, full, strlen(full) + 1);
    if (ARG(3)) { const char *b = strrchr(full, '\\'); w32_write(w, ARG(3), (int)w32_ptrsize(w), buf + (b ? (uint64_t)(b - full + 1) : 0)); }
    RET(strlen(full));
}
static void k_FormatMessageA(w32 *w) {
    /* (flags, source, msgid, langid, buf, size, args) -> a generic text */
    uint64_t buf = ARG(4); uint32_t n = (uint32_t)ARG(5);
    char s[64]; snprintf(s, sizeof s, "Error %u", (unsigned)ARG(2));
    void *o = n > strlen(s) ? W32PN(w, buf, strlen(s) + 1) : 0;
    if (o) { memcpy(o, s, strlen(s) + 1); RET(strlen(s)); } else RET(0);
}

/* ---- threads: one, this one ---- */
static void k_GetThreadPriority(w32 *w) { RET(0); }
static void k_SetThreadPriority(w32 *w) { RET(1); }
static void k_GetExitCodeProcess(w32 *w) { w32_write(w, ARG(1), 4, 0); RET(1); }
static void k_GetProcessAffinityMask(w32 *w) { w32_write(w, ARG(1), (int)w32_ptrsize(w), 0xF); w32_write(w, ARG(2), (int)w32_ptrsize(w), 0xF); RET(1); }
static void k_SetErrorMode(w32 *w) { RET(0); }
static void k_RtlPcToFileHeader(w32 *w) { w32_write(w, ARG(1), (int)w32_ptrsize(w), w->image_base); RET(w->image_base); }
static void k_RtlLookupFunctionEntry(w32 *w) { RET(0); }
static void k_RtlVirtualUnwind(w32 *w) { RET(0); }
static void k_EncodePointer(w32 *w) { RET(ARG(0)); }
static void k_DecodePointer(w32 *w) { RET(ARG(0)); }
static void k_InitializeSListHead(w32 *w) { void *p = W32PN(w, ARG(0), 16); if (p) memset(p, 0, 16); }
static void k_GetStartupInfoW(w32 *w) { k_GetStartupInfoA(w); }
static void k_SetHandleCount(w32 *w) { RET(ARG(0)); }
static void k_GetEnvironmentVariableW(w32 *w) { RET(0); }
static void k_GetLogicalDrives(w32 *w) { RET(4); }
static void k_GetDriveTypeA(w32 *w) { RET(3); }
/* Both of these write a fixed six bytes and trust the caller's size word,
 * which is what they did before; the pointer is now the part that has to be
 * real. */
static void k_GetComputerNameA(w32 *w) { void *p = W32PN(w, ARG(0), 6);
    if (!p) { w32_set_last_error(w, ERROR_NOACCESS); RET(0); return; }
    memcpy(p, "XCORE", 6); w32_write(w, ARG(1), 4, 5); RET(1); }
static void k_GetUserNameA(w32 *w) { void *p = W32PN(w, ARG(0), 6);
    if (!p) { w32_set_last_error(w, ERROR_NOACCESS); RET(0); return; }
    memcpy(p, "xcore", 6); w32_write(w, ARG(1), 4, 6); RET(1); }
static void k_lstrlenA(w32 *w) { RET(strlen(GSTR(ARG(0)))); }
static void k_lstrlenW(w32 *w) { RET(w32_wcslen(w, ARG(0))); }
/* lstrcpy has no count, so the length comes from the source string and the
 * destination is checked for exactly that much. */
static void k_lstrcpyA(w32 *w) {
    const char *s = GSTR(ARG(1));
    char *d = W32PN(w, ARG(0), strlen(s) + 1);
    if (!d) { RET(0); return; }
    strcpy(d, s);
    RET(ARG(0));
}
/* The wide one copies UTF-16 units, terminator included. A Unicode installer
 * builds every path with it, and copying it as bytes would truncate at the
 * first character whose high byte is zero -- which is every ASCII one. */
static void k_lstrcpyW(w32 *w) {
    uint64_t d = ARG(0), s = ARG(1);
    if (!d || !s) { RET(0); return; }
    uint64_t i = 0;
    for (;; i += 2) {
        uint64_t c = w32_read(w, s + i, 2);
        w32_write(w, d + i, 2, c);
        if (!c) break;
        if (i > (1u << 20)) break;               /* an unterminated string is not a string */
    }
    RET(d);
}

/* GetFileTime, the read half of the pair whose write half was already here.
 * An installer compares the file it is about to replace against the one it
 * is carrying, and with no way to read a time it either always overwrites or
 * always skips -- both wrong, and both silent. */
static void k_GetFileTime(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0));
    if (!h || h->type != H_FILE || h->fd < 0) { w32_set_last_error(w, 6); RET(0); return; }
    struct stat st;
    if (fstat(h->fd, &st)) { w32_set_last_error(w, 5); RET(0); return; }
    /* FILETIME is 100-nanosecond ticks since 1601; the Unix epoch is
     * 11644473600 seconds later. */
    struct { uint64_t p; time_t t; } want[3] = {
        { ARG(1), st.st_ctime }, { ARG(2), st.st_atime }, { ARG(3), st.st_mtime },
    };
    for (int i = 0; i < 3; i++) {
        if (!want[i].p) continue;
        uint64_t ft = ((uint64_t)want[i].t + 11644473600ull) * 10000000ull;
        w32_write(w, want[i].p, 4, ft & 0xFFFFFFFFu);
        w32_write(w, want[i].p + 4, 4, ft >> 32);
    }
    RET(1);
}
static void k_lstrcmpiA(w32 *w) { RET((uint64_t)(int64_t)strcasecmp(GSTR(ARG(0)), GSTR(ARG(1)))); }
static void put_narrow_dir(w32 *w, const char *d) {
    if ((uint32_t)ARG(1) > strlen(d)) {
        void *o = W32PN(w, ARG(0), strlen(d) + 1);
        if (!o) { w32_set_last_error(w, ERROR_NOACCESS); RET(0); return; }
        memcpy(o, d, strlen(d) + 1);
    }
    RET(strlen(d));
}
static void k_GetSystemDirectoryA(w32 *w) { put_narrow_dir(w, "C:\\Windows\\System32"); }
static void k_GetWindowsDirectoryA(w32 *w) { put_narrow_dir(w, "C:\\Windows"); }
static void k_IsProcessorFeaturePresent(w32 *w) { uint32_t f = (uint32_t)ARG(0); RET(bool_(f == 6 || f == 10 || f == 13 || f == 17 || f == 23)); }   /* SSE, SSE2, SSE3, SSE4, fastfail */
/* ---- what an installer does ------------------------------------------------
 *
 * A setup program in silent mode is, almost entirely, a file copier with a
 * registry writer attached: it asks how much space is free, makes some
 * directories, copies files into them, sets a few attributes, writes an
 * uninstall key, and leaves. Every one of those was missing, so an installer
 * stopped at the first of them regardless of how well it had been unpacked.
 *
 * These are also what a *game* needs on a second run: a program that saved
 * settings on Tuesday expects to move and rename them on Wednesday.
 */

/* One copy, with the flag the API actually has: fail rather than overwrite. */
static int copy_one(const char *src, const char *dst, int fail_if_exists) {
    struct stat st;
    if (fail_if_exists && stat(dst, &st) == 0) { errno = EEXIST; return -1; }
    FILE *a = fopen(src, "rb");
    if (!a) return -1;
    FILE *b = fopen(dst, "wb");
    if (!b) { int e = errno; fclose(a); errno = e; return -1; }
    char buf[64 * 1024];
    size_t n;
    int bad = 0;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        if (fwrite(buf, 1, n, b) != n) { bad = 1; break; }
    if (ferror(a)) bad = 1;
    fclose(a);
    if (fclose(b)) bad = 1;
    if (bad) { remove(dst); return -1; }
    /* Carry the mode across, so a copied executable stays executable. Times
     * are not carried: an installer sets them itself when it cares, and
     * pretending a fresh copy is old confuses the drive diff. */
    if (stat(src, &st) == 0) (void)chmod(dst, st.st_mode & 07777);
    return 0;
}

static void copy_file(w32 *w, int wide, int fail_arg_is_bool) {
    char a[1024], b[1024], pa[4096], pb[4096];
    if (wide) { w32_wtoa(w, ARG(0), a, sizeof a); w32_wtoa(w, ARG(1), b, sizeof b); }
    else { snprintf(a, sizeof a, "%s", GSTR(ARG(0))); snprintf(b, sizeof b, "%s", GSTR(ARG(1))); }
    host_path(w, a, pa, sizeof pa);
    host_path(w, b, pb, sizeof pb);
    /* CopyFile's third argument is bFailIfExists; CopyFileEx's is a progress
     * callback and its *sixth* is a flags word with COPY_FILE_FAIL_IF_EXISTS
     * (1) in it. Same operation, different shape, and getting it the wrong way
     * round means silently refusing every overwrite. */
    int fail = fail_arg_is_bool ? (int)ARG(2) != 0 : ((uint32_t)ARG(5) & 1u) != 0;
    if (copy_one(pa, pb, fail)) {
        w32_set_last_error(w, errno == EEXIST ? ERROR_ALREADY_EXISTS
                            : errno == ENOENT ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
        RET(0); return;
    }
    RET(1);
}
static void k_CopyFileA(w32 *w) { copy_file(w, 0, 1); }
static void k_CopyFileW(w32 *w) { copy_file(w, 1, 1); }
static void k_CopyFileExA(w32 *w) { copy_file(w, 0, 0); }
static void k_CopyFileExW(w32 *w) { copy_file(w, 1, 0); }

/* MOVEFILE_REPLACE_EXISTING 1, MOVEFILE_COPY_ALLOWED 2,
 * MOVEFILE_DELAY_UNTIL_REBOOT 4. The third is what an installer uses for a
 * file that is in use; there is no reboot here, so it is done immediately,
 * which is the outcome the caller wanted a reboot for. */
static void move_file(w32 *w, int wide, int has_flags) {
    char a[1024], b[1024], pa[4096], pb[4096];
    if (wide) { w32_wtoa(w, ARG(0), a, sizeof a); w32_wtoa(w, ARG(1), b, sizeof b); }
    else { snprintf(a, sizeof a, "%s", GSTR(ARG(0))); snprintf(b, sizeof b, "%s", GSTR(ARG(1))); }
    host_path(w, a, pa, sizeof pa);
    uint32_t flags = has_flags ? (uint32_t)ARG(2) : 0;
    /* A null destination with DELAY_UNTIL_REBOOT means "delete this on the
     * way out", which is how an uninstaller removes a file it is holding. */
    if (!ARG(1)) { RET(bool_(unlink(pa) == 0 || errno == ENOENT)); return; }
    host_path(w, b, pb, sizeof pb);
    struct stat st;
    if (!(flags & 1u) && stat(pb, &st) == 0) {
        w32_set_last_error(w, ERROR_ALREADY_EXISTS); RET(0); return;
    }
    if (rename(pa, pb) == 0) { RET(1); return; }
    /* Across devices rename cannot work, and an installer moving out of a
     * temp directory hits that constantly. COPY_ALLOWED is the caller's
     * permission to do it the slow way; DELAY_UNTIL_REBOOT implies it. */
    if (errno == EXDEV && (flags & 6u)) {
        if (copy_one(pa, pb, 0) == 0 && unlink(pa) == 0) { RET(1); return; }
    }
    w32_set_last_error(w, errno == ENOENT ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
    RET(0);
}
static void k_MoveFileA(w32 *w)   { move_file(w, 0, 0); }
static void k_MoveFileW(w32 *w)   { move_file(w, 1, 0); }
static void k_MoveFileExA(w32 *w) { move_file(w, 0, 1); }
static void k_MoveFileExW(w32 *w) { move_file(w, 1, 1); }

static void make_dir(w32 *w, int wide) {
    char a[1024], pa[4096];
    if (wide) w32_wtoa(w, ARG(0), a, sizeof a); else snprintf(a, sizeof a, "%s", GSTR(ARG(0)));
    host_path(w, a, pa, sizeof pa);
    if (mkdir(pa, 0777) == 0) { RET(1); return; }
    w32_set_last_error(w, errno == EEXIST ? ERROR_ALREADY_EXISTS
                        : errno == ENOENT ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
    RET(0);
}
static void k_CreateDirectoryA(w32 *w) { make_dir(w, 0); }
static void k_CreateDirectoryW(w32 *w) { make_dir(w, 1); }
static void remove_dir(w32 *w, int wide) {
    char a[1024], pa[4096];
    if (wide) w32_wtoa(w, ARG(0), a, sizeof a); else snprintf(a, sizeof a, "%s", GSTR(ARG(0)));
    host_path(w, a, pa, sizeof pa);
    if (rmdir(pa) == 0) { RET(1); return; }
    w32_set_last_error(w, errno == ENOENT ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
    RET(0);
}
static void k_RemoveDirectoryA(w32 *w) { remove_dir(w, 0); }
static void k_RemoveDirectoryW(w32 *w) { remove_dir(w, 1); }

static void k_DeleteFileW(w32 *w) {
    char a[1024], pa[4096]; w32_wtoa(w, ARG(0), a, sizeof a);
    host_path(w, a, pa, sizeof pa);
    if (unlink(pa) == 0) { RET(1); return; }
    w32_set_last_error(w, ERROR_FILE_NOT_FOUND); RET(0);
}
static void k_GetFileAttributesW(w32 *w) {
    char a[1024], pa[4096]; w32_wtoa(w, ARG(0), a, sizeof a);
    host_path(w, a, pa, sizeof pa);
    struct stat st;
    if (stat(pa, &st)) { w32_set_last_error(w, ERROR_FILE_NOT_FOUND); RET(0xFFFFFFFFu); return; }
    RET(S_ISDIR(st.st_mode) ? FA_DIRECTORY : (st.st_mode & S_IWUSR) ? FA_NORMAL : (FA_NORMAL | FA_READONLY));
}
/* Only the read-only bit means anything on a POSIX filesystem. Hidden,
 * system and archive are accepted and dropped: refusing them would stop an
 * installer that is merely tidying up, and pretending to store them would be
 * a lie the next GetFileAttributes would expose. */
static void set_attrs(w32 *w, int wide) {
    char a[1024], pa[4096];
    if (wide) w32_wtoa(w, ARG(0), a, sizeof a); else snprintf(a, sizeof a, "%s", GSTR(ARG(0)));
    host_path(w, a, pa, sizeof pa);
    struct stat st;
    if (stat(pa, &st)) { w32_set_last_error(w, ERROR_FILE_NOT_FOUND); RET(0); return; }
    mode_t m = st.st_mode & 07777;
    if ((uint32_t)ARG(1) & FA_READONLY) m &= (mode_t)~(S_IWUSR | S_IWGRP | S_IWOTH);
    else m |= S_IWUSR;
    RET(bool_(chmod(pa, m) == 0));
}
static void k_SetFileAttributesA(w32 *w) { set_attrs(w, 0); }
static void k_SetFileAttributesW(w32 *w) { set_attrs(w, 1); }

/* How much room is there? An installer asks before it starts and refuses to
 * go on if the answer is zero or unavailable, so this has to be a real
 * number. It is the host filesystem's, which is the truth: the virtual C: is
 * a directory on it. */
static void disk_free_ex(w32 *w, int wide) {
    char a[1024], pa[4096];
    if (ARG(0)) {
        if (wide) w32_wtoa(w, ARG(0), a, sizeof a); else snprintf(a, sizeof a, "%s", GSTR(ARG(0)));
    } else snprintf(a, sizeof a, "C:\\");
    host_path(w, a, pa, sizeof pa);
    struct statvfs vfs;
    uint64_t avail = 4ull << 30, total = 32ull << 30;
    if (statvfs(pa, &vfs) == 0 || statvfs(g_drive_c[0] ? g_drive_c : ".", &vfs) == 0) {
        uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
        avail = (uint64_t)vfs.f_bavail * unit;
        total = (uint64_t)vfs.f_blocks * unit;
    }
    if (ARG(1)) w32_write(w, ARG(1), 8, avail);       /* free to the caller  */
    if (ARG(2)) w32_write(w, ARG(2), 8, total);       /* total               */
    if (ARG(3)) w32_write(w, ARG(3), 8, avail);       /* free on the volume  */
    RET(1);
}
static void k_GetDiskFreeSpaceExA(w32 *w) { disk_free_ex(w, 0); }
static void k_GetDiskFreeSpaceExW(w32 *w) { disk_free_ex(w, 1); }
/* The older call, in clusters. 512-byte sectors and 8 per cluster keeps the
 * arithmetic exact and the numbers inside 32 bits, which is what callers of
 * this version are assuming. */
static void disk_free_old(w32 *w) {
    struct statvfs vfs;
    uint64_t avail = 4ull << 30, total = 32ull << 30;
    if (statvfs(g_drive_c[0] ? g_drive_c : ".", &vfs) == 0) {
        uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
        avail = (uint64_t)vfs.f_bavail * unit;
        total = (uint64_t)vfs.f_blocks * unit;
    }
    const uint32_t cluster = 4096, sector = 512;
    uint64_t freec = avail / cluster, totc = total / cluster;
    if (freec > 0xFFFFFFFFull) freec = 0xFFFFFFFFull;
    if (totc  > 0xFFFFFFFFull) totc  = 0xFFFFFFFFull;
    if (ARG(1)) w32_write(w, ARG(1), 4, cluster / sector);
    if (ARG(2)) w32_write(w, ARG(2), 4, sector);
    if (ARG(3)) w32_write(w, ARG(3), 4, freec);
    if (ARG(4)) w32_write(w, ARG(4), 4, totc);
    RET(1);
}
static void k_GetDiskFreeSpaceA(w32 *w) { disk_free_old(w); }
static void k_GetDiskFreeSpaceW(w32 *w) { disk_free_old(w); }

/* NTFS, because an installer that finds FAT32 refuses to write a file over
 * 4 GB and some refuse to install at all. */
static void volume_info(w32 *w, int wide) {
    const char *label = "Winios", *fs = "NTFS";
    #define PUT(a, n, s) do { if ((a) && (uint32_t)(n) > strlen(s)) { \
        if (wide) { uint16_t *d = W32PN(w, (a), 2 * (strlen(s) + 1)); \
                    if (d) { size_t i = 0; \
                             while (s[i]) { d[i] = (uint8_t)s[i]; i++; } \
                             d[i] = 0; } } \
        else { void *d = W32PN(w, (a), strlen(s) + 1); \
               if (d) memcpy(d, s, strlen(s) + 1); } } } while (0)
    PUT(ARG(1), ARG(2), label);
    PUT(ARG(6), ARG(7), fs);
    #undef PUT
    if (ARG(3)) w32_write(w, ARG(3), 4, 0x1234ABCDu);   /* serial number */
    if (ARG(4)) w32_write(w, ARG(4), 4, 255);           /* max component length */
    /* CASE_PRESERVED_NAMES | UNICODE_ON_DISK | PERSISTENT_ACLS */
    if (ARG(5)) w32_write(w, ARG(5), 4, 0x2 | 0x4 | 0x8);
    RET(1);
}
static void k_GetVolumeInformationA(w32 *w) { volume_info(w, 0); }
static void k_GetVolumeInformationW(w32 *w) { volume_info(w, 1); }

/* A temp file name that does not already exist. The real one uses the process
 * id and a counter; a counter alone is enough here, and checking is what
 * makes it correct rather than merely unlikely. */
static void temp_name(w32 *w, int wide) {
    static unsigned seq;
    char dir[1024], base[8], host[4096], win[1200];
    if (wide) { w32_wtoa(w, ARG(0), dir, sizeof dir); w32_wtoa(w, ARG(1), base, sizeof base); }
    else { snprintf(dir, sizeof dir, "%s", GSTR(ARG(0))); snprintf(base, sizeof base, "%s", GSTR(ARG(1))); }
    if (!dir[0]) snprintf(dir, sizeof dir, "C:\\Temp");
    size_t dl = strlen(dir);
    while (dl && (dir[dl - 1] == '\\' || dir[dl - 1] == '/')) dir[--dl] = 0;
    uint32_t unique = (uint32_t)ARG(2);
    for (int tries = 0; tries < 4096; tries++) {
        uint32_t u = unique ? unique : ((++seq) & 0xFFFFu) | 0x1000u;
        snprintf(win, sizeof win, "%.*s\\%.3s%04x.tmp", P_DIR, dir, base[0] ? base : "tmp", u);
        host_path(w, win, host, sizeof host);
        struct stat st;
        if (unique || stat(host, &st) != 0) {
            /* A non-zero unique means "give me the name, do not create it";
             * zero means the name has to be reserved, or two callers a
             * microsecond apart get the same one. */
            if (!unique) { FILE *f = fopen(host, "wb"); if (!f) continue; fclose(f); }
            if (ARG(3)) {
                /* MAX_PATH is what the caller is documented to provide, but
                 * the name is what is actually written, so that is what is
                 * checked. */
                size_t wl = strlen(win);
                if (wide) { uint16_t *d = W32PN(w, ARG(3), 2 * (wl + 1));
                            if (d) { size_t i = 0; for (; win[i]; i++) d[i] = (uint8_t)win[i]; d[i] = 0; } }
                else { void *d = W32PN(w, ARG(3), wl + 1);
                       if (d) memcpy(d, win, wl + 1); }
            }
            RET(u); return;
        }
    }
    w32_set_last_error(w, ERROR_ACCESS_DENIED); RET(0);
}
static void k_GetTempFileNameA(w32 *w) { temp_name(w, 0); }
static void k_GetTempFileNameW(w32 *w) { temp_name(w, 1); }
static void k_GetTempPathW(w32 *w) {
    const char *d = "C:\\Temp\\"; size_t l = strlen(d);
    if ((uint32_t)ARG(0) > l) { uint16_t *o = W32PN(w, ARG(1), 2 * (l + 1));
        if (o) { for (size_t i = 0; i < l; i++) o[i] = (uint8_t)d[i]; o[l] = 0; } }
    RET(l);
}

static void k_SetEndOfFile(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0));
    if (!h || h->type != H_FILE) { w32_set_last_error(w, ERROR_INVALID_HANDLE); RET(0); return; }
    off_t at = lseek(h->fd, 0, SEEK_CUR);
    RET(bool_(at >= 0 && ftruncate(h->fd, at) == 0));
}

/* No short names here -- the drive is a POSIX directory and has none -- so
 * both of these return the path unchanged, which is what a filesystem
 * without 8.3 aliases does on real Windows too. */
static void path_identity(w32 *w, int wide) {
    char s[1024];
    if (wide) w32_wtoa(w, ARG(0), s, sizeof s); else snprintf(s, sizeof s, "%s", GSTR(ARG(0)));
    size_t l = strlen(s);
    uint32_t n = (uint32_t)ARG(2);
    if (n > l && ARG(1)) {
        if (wide) { uint16_t *d = W32PN(w, ARG(1), 2 * (l + 1));
                    if (!d) { w32_set_last_error(w, ERROR_NOACCESS); RET(0); return; }
                    for (size_t i = 0; i < l; i++) d[i] = (uint8_t)s[i];
                    d[l] = 0; }
        else { void *d = W32PN(w, ARG(1), l + 1);
               if (!d) { w32_set_last_error(w, ERROR_NOACCESS); RET(0); return; }
               memcpy(d, s, l + 1); }
        RET(l); return;
    }
    RET(l + 1);
}
static void k_GetShortPathNameA(w32 *w) { path_identity(w, 0); }
static void k_GetShortPathNameW(w32 *w) { path_identity(w, 1); }
static void k_GetLongPathNameA(w32 *w)  { path_identity(w, 0); }
static void k_GetLongPathNameW(w32 *w)  { path_identity(w, 1); }

/* There is one process at a time: the runtime's globals -- the block cache,
 * the code arena, the handle table -- are per process. What there can be is
 * a *next* process. CreateProcess records the program and winrun runs it after
 * this one ends, in the order they were asked for. The caller gets a process
 * handle that is already signalled and an exit code of 0, which is the truth
 * for the two shapes this exists for: an installer that runs a redistributable
 * and carries on, and a launcher that starts the game and quits. A parent that
 * needs the child's output while it is still running is not served, and the
 * run report lists what was queued so that is visible rather than guessed at. */
uint64_t w32_process_handle_new(w32 *w) {
    uint64_t h = w32_handle_new(w, H_PROCESS, -1);
    w32_handle *hh = w32_handle_get(w, h);
    if (hh) hh->flags = 1 | 2;                      /* signalled, and stays so */
    return h;
}
static int program_exists(w32 *w, const char *win, char *host, size_t hn) {
    w32_host_path(w, win, host, hn);
    if (access(host, R_OK) == 0) return 1;
    char withexe[1100];
    snprintf(withexe, sizeof withexe, "%s.exe", win);
    w32_host_path(w, withexe, host, hn);
    return access(host, R_OK) == 0;
}
/* The program a command line names: the quoted first token, or, unquoted,
 * the shortest prefix ending at a space that names a file -- "C:\Program
 * Files\Game\game.exe" without quotes is legal and common. */
static int resolve_program(w32 *w, const char *app, const char *cmd, char *host, size_t hn, const char **args_out) {
    char cand[1024];
    *args_out = "";
    if (app[0]) { *args_out = cmd; return program_exists(w, app, host, hn); }
    const char *p = cmd;
    while (*p == ' ') p++;
    if (*p == '"') {
        const char *e = strchr(p + 1, '"');
        size_t n = e ? (size_t)(e - p - 1) : strlen(p + 1);
        if (n >= sizeof cand) return 0;
        memcpy(cand, p + 1, n); cand[n] = 0;
        *args_out = e ? e + 1 : "";
        return program_exists(w, cand, host, hn);
    }
    for (const char *e = p; ; e++) {
        if (*e && *e != ' ') continue;
        size_t n = (size_t)(e - p);
        if (n >= sizeof cand) return 0;
        memcpy(cand, p, n); cand[n] = 0;
        if (program_exists(w, cand, host, hn)) { *args_out = e; return 1; }
        if (!*e) return 0;
    }
}
static uint32_t g_child_pid = 4300;
static void create_process(w32 *w, int wide) {
    char app[1024] = "", cmd[4096] = "", cwd[1024] = "", host[4096];
    if (ARG(0)) { if (wide) w32_wtoa(w, ARG(0), app, sizeof app); else snprintf(app, sizeof app, "%s", w32_str(w, ARG(0))); }
    if (ARG(1)) { if (wide) w32_wtoa(w, ARG(1), cmd, sizeof cmd); else snprintf(cmd, sizeof cmd, "%s", w32_str(w, ARG(1))); }
    if (ARG(7)) { if (wide) w32_wtoa(w, ARG(7), cwd, sizeof cwd); else snprintf(cwd, sizeof cwd, "%s", w32_str(w, ARG(7))); }
    const char *args = "";
    if (!resolve_program(w, app, cmd, host, sizeof host, &args)) {
        if (w->verbose) fprintf(stderr, "winrun: CreateProcess: no such program: %s%s%s\n", app, app[0] ? " " : "", cmd);
        w32_set_last_error(w, ERROR_FILE_NOT_FOUND); RET(0); return;
    }
    while (*args == ' ') args++;
    if (w32_launch_queue(w, host, args, cwd[0] ? cwd : g_cwd_win, "CreateProcess") < 0) {
        w32_set_last_error(w, 8); RET(0); return;     /* ERROR_NOT_ENOUGH_MEMORY: the queue is full */
    }
    /* PROCESS_INFORMATION: hProcess, hThread, dwProcessId, dwThreadId */
    uint64_t pi = ARG(9); int psz = (int)w32_ptrsize(w);
    if (pi && w32_mem_ok(w, pi, (uint64_t)psz * 2 + 8)) {
        uint32_t pid = ++g_child_pid;
        w32_write(w, pi, psz, w32_process_handle_new(w));
        w32_write(w, pi + psz, psz, w32_process_handle_new(w));
        w32_write(w, pi + 2 * psz, 4, pid);
        w32_write(w, pi + 2 * psz + 4, 4, pid + 1);
    }
    RET(1);
}
static void k_CreateProcessA(w32 *w) { create_process(w, 0); }
static void k_CreateProcessW(w32 *w) { create_process(w, 1); }

/* ---- .ini files ------------------------------------------------------------
 *
 * Older games keep their settings in one, and installers write them. The
 * format is simple enough that implementing it properly costs less than
 * explaining a stub: sections in brackets, key=value lines, last one wins,
 * comments after ; or #.
 */
static void ini_host(w32 *w, uint64_t p, int wide, char *out, size_t n) {
    char win[1024];
    if (!p) { snprintf(out, n, "%s", ""); return; }
    if (wide) w32_wtoa(w, p, win, sizeof win); else snprintf(win, sizeof win, "%s", w32_str(w, p));
    /* A bare name means the Windows directory on real Windows. Ours is on the
     * drive, which is also where a program that wrote one will look. */
    if (!strchr(win, '\\') && !strchr(win, '/') && win[0]) {
        char q[1200]; snprintf(q, sizeof q, "C:\\Windows\\%.*s", P_REST, win);
        host_path(w, q, out, n); return;
    }
    host_path(w, win, out, n);
}
static char *ini_read(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return 0; }
    long n = ftell(f);
    if (n < 0 || n > (8 << 20) || fseek(f, 0, SEEK_SET)) { fclose(f); return 0; }
    char *b = (char *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return 0; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = 0;
    *len = got;
    return b;
}
static void ini_trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = 0;
    size_t i = 0; while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, n - i + 1);
}
/* Walk the file, calling back for each key in `want_section`. Sharing the
 * walk between read, enumerate and write is what keeps their ideas of the
 * format from drifting apart. */
typedef int (*ini_fn)(void *ctx, const char *key, const char *val);
static void ini_walk(const char *text, const char *want_section, ini_fn fn, void *ctx) {
    char sec[256] = "", line[2048];
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, p, l); line[l] = 0;
        p = e ? e + 1 : p + strlen(p);
        ini_trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            char *close = strchr(line, ']');
            if (close) { *close = 0; snprintf(sec, sizeof sec, "%.*s", (int)sizeof sec - 1, line + 1); ini_trim(sec); }
            continue;
        }
        if (want_section && strcasecmp(sec, want_section)) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        ini_trim(key); ini_trim(val);
        if (fn(ctx, key, val)) return;
    }
}
typedef struct { const char *key; char val[1024]; int found; } ini_get_ctx;
static int ini_get_step(void *c, const char *key, const char *val) {
    ini_get_ctx *g = (ini_get_ctx *)c;
    if (strcasecmp(key, g->key)) return 0;
    snprintf(g->val, sizeof g->val, "%s", val);
    g->found = 1;
    return 0;                    /* keep going: the last one wins, as on Windows */
}
static void profile_string(w32 *w, int wide) {
    char app[256], key[256], def[1024], path[4096];
    if (wide) { w32_wtoa(w, ARG(0), app, sizeof app); w32_wtoa(w, ARG(1), key, sizeof key);
                w32_wtoa(w, ARG(2), def, sizeof def); }
    else { snprintf(app, sizeof app, "%s", ARG(0) ? GSTR(ARG(0)) : "");
           snprintf(key, sizeof key, "%s", ARG(1) ? GSTR(ARG(1)) : "");
           snprintf(def, sizeof def, "%s", ARG(2) ? GSTR(ARG(2)) : ""); }
    ini_host(w, ARG(5), wide, path, sizeof path);
    size_t tlen = 0; char *text = ini_read(path, &tlen);
    ini_get_ctx g; g.key = key; g.found = 0; g.val[0] = 0;
    if (text && ARG(0) && ARG(1)) ini_walk(text, app, ini_get_step, &g);
    free(text);
    const char *out = g.found ? g.val : def;
    size_t l = strlen(out);
    uint32_t n = (uint32_t)ARG(4);
    if (!n || !ARG(3)) { RET(0); return; }
    if (l + 1 > n) l = n - 1;
    if (wide) { uint16_t *d = W32PN(w, ARG(3), 2 * (l + 1));
                if (!d) { RET(0); return; }
                for (size_t i = 0; i < l; i++) d[i] = (uint8_t)out[i];
                d[l] = 0; }
    else { char *d = W32PN(w, ARG(3), l + 1);
           if (!d) { RET(0); return; }
           memcpy(d, out, l); d[l] = 0; }
    RET(l);
}
static void k_GetPrivateProfileStringA(w32 *w) { profile_string(w, 0); }
static void k_GetPrivateProfileStringW(w32 *w) { profile_string(w, 1); }
static void k_GetPrivateProfileIntA(w32 *w) {
    char app[256], key[256], path[4096];
    snprintf(app, sizeof app, "%s", ARG(0) ? GSTR(ARG(0)) : "");
    snprintf(key, sizeof key, "%s", ARG(1) ? GSTR(ARG(1)) : "");
    ini_host(w, ARG(3), 0, path, sizeof path);
    size_t tlen = 0; char *text = ini_read(path, &tlen);
    ini_get_ctx g; g.key = key; g.found = 0; g.val[0] = 0;
    if (text) ini_walk(text, app, ini_get_step, &g);
    free(text);
    RET(g.found ? (uint32_t)strtol(g.val, 0, 0) : (uint32_t)ARG(2));
}
/* Writing one means rewriting the file, because a value can change length.
 * Three cases and they are all here: the key exists (replace the line), the
 * section exists but not the key (insert at the end of the section, not the
 * end of the file -- putting it after a later section header would file it
 * under the wrong section), and neither exists (append both). */
/* The rewrite itself, shared by the A and W forms. An .ini is bytes on disk
 * either way, and two implementations would be two chances to get the
 * insert-at-the-end-of-the-section case wrong. `key` or `val` NULL means
 * delete, as the API defines it. */
int w32_ini_write(w32 *w, const char *app, const char *key, const char *val,
                  const char *file) {
    char path[4096];
    ini_host(w, 0, 0, path, sizeof path);        /* the default location... */
    if (file && *file) {                          /* ...unless one was named */
        if (!strchr(file, '\\') && !strchr(file, '/')) {
            char q[1200];
            snprintf(q, sizeof q, "C:\\Windows\\%.*s", (int)sizeof q - 12, file);
            host_path(w, q, path, sizeof path);
        } else host_path(w, file, path, sizeof path);
    }
    if (!app || !app[0]) return 0;
    const int has_key = key != 0, has_val = val != 0;
    if (!key) key = "";
    if (!val) val = "";

    size_t tlen = 0;
    char *text = ini_read(path, &tlen);
    /* A file that exists but could not be read -- too large for ini_read's
     * cap, or a permission problem -- must not be treated as absent: the
     * rewrite below would replace somebody's settings with a single key.
     * Absent is fine; unreadable is a failure. */
    if (!text) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            w32_set_last_error(w, ERROR_ACCESS_DENIED); return 0;
        }
    }
    const char *src = text ? text : "";
    size_t cap = tlen + strlen(key) + strlen(val) + strlen(app) + 64;
    char *out = (char *)malloc(cap);
    if (!out) { free(text); w32_set_last_error(w, ERROR_NOT_ENOUGH_MEMORY); return 0; }
    size_t o = 0;
    int in_sec = 0, wrote = 0, seen_sec = 0;
    /* snprintf returns what it *would* have written, so adding that to the
     * offset walks past the end of the buffer the moment anything truncates
     * -- and then `cap - o` underflows to an enormous size_t and the next
     * write is unbounded. Clamped, because the data driving it is an .ini
     * file the guest wrote. */
    #define EMIT(...) do { \
        if (o + 1 < cap) { \
            int n_ = snprintf(out + o, cap - o, __VA_ARGS__); \
            size_t room_ = cap - o - 1; \
            o += n_ < 0 ? 0 : ((size_t)n_ < room_ ? (size_t)n_ : room_); \
        } \
    } while (0)

    const char *p = src;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        /* A bounded copy for *parsing* -- a section name or key longer than
         * this is not real -- but the line is written back out from the
         * source bytes with an explicit length, so a long line is passed
         * through intact rather than silently truncated to 2047. Losing a
         * line of somebody's settings file is not an acceptable way to
         * handle one that is unusually long. */
        char trimmed[2048];
        size_t cl = l < sizeof trimmed - 1 ? l : sizeof trimmed - 1;
        memcpy(trimmed, p, cl); trimmed[cl] = 0;
        ini_trim(trimmed);
        const char *line = p;
        const int linelen = (int)l;
        p = e ? e + 1 : p + strlen(p);

        if (trimmed[0] == '[') {
            /* Leaving the section we wanted without having written the key:
             * it goes here, before the next header. */
            if (in_sec && !wrote && has_key) { EMIT("%s=%s\n", key, val); wrote = 1; }
            char name[256]; snprintf(name, sizeof name, "%.*s", (int)sizeof name - 1, trimmed + 1);
            char *close = strchr(name, ']'); if (close) *close = 0;
            ini_trim(name);
            in_sec = !strcasecmp(name, app);
            if (in_sec) seen_sec = 1;
            /* A null key with a null value deletes the whole section. */
            if (in_sec && !has_key && !has_val) { wrote = 1; continue; }
            EMIT("%.*s\n", linelen, line);
            continue;
        }
        if (in_sec && has_key) {
            char k2[2048]; snprintf(k2, sizeof k2, "%s", trimmed);
            char *eq = strchr(k2, '='); if (eq) *eq = 0;
            ini_trim(k2);
            if (!strcasecmp(k2, key)) {
                if (has_val) { EMIT("%s=%s\n", key, val); }   /* replace */
                wrote = 1;                                    /* null value deletes */
                continue;
            }
        }
        if (in_sec && !has_key && !has_val) continue;         /* dropping the section */
        EMIT("%.*s\n", linelen, line);
    }
    if (!wrote && has_key && has_val) {
        if (!seen_sec) EMIT("[%s]\n", app);
        EMIT("%s=%s\n", key, val);
    }
    #undef EMIT
    free(text);
    FILE *f = fopen(path, "wb");
    if (!f) { free(out); w32_set_last_error(w, ERROR_ACCESS_DENIED); return 0; }
    size_t put = fwrite(out, 1, o, f);
    int ok = (put == o) && (fclose(f) == 0);
    free(out);
    return ok;
}

/* The two entry points, which are now only argument marshalling. */
static void k_WritePrivateProfileStringA(w32 *w) {
    RET(bool_(w32_ini_write(w, ARG(0) ? GSTR(ARG(0)) : "",
                            ARG(1) ? GSTR(ARG(1)) : 0,
                            ARG(2) ? GSTR(ARG(2)) : 0,
                            ARG(3) ? GSTR(ARG(3)) : "")));
}

/* ---- where DLLs are looked for --------------------------------------------
 *
 * SetDefaultDllDirectories is the one an NSIS installer calls first, and it
 * calls it for security rather than for function: it narrows the search so a
 * planted DLL in the current directory cannot be picked up ahead of the real
 * one. Our loader already searches only the executable's own directory plus
 * whatever -L named, which is narrower than the default it is trying to get
 * rid of -- so agreeing is honest, and it is what lets the installer start.
 *
 * It was the *only* thing a 580 MB GameMaker installer asked for that was not
 * here, which is worth recording: the list of what a real installer needs is
 * much shorter than it looks from the outside.
 */
static void k_SetDefaultDllDirectories(w32 *w) { (void)w; RET(1); }

/* An extra directory to look in, which we can honour for real. NULL or an
 * empty string goes back to the default. */
static char g_extra_dll_dir[1024];
static void set_dll_dir(w32 *w, const char *win) {
    if (!win || !*win) { g_extra_dll_dir[0] = 0; w->dll_dir = 0; RET(1); return; }
    char host[900];
    host_path(w, win, host, sizeof host);
    /* The loader expects a trailing separator: it joins without adding one. */
    size_t n = strlen(host);
    snprintf(g_extra_dll_dir, sizeof g_extra_dll_dir, "%s%s", host,
             (n && host[n - 1] == '/') ? "" : "/");
    w->dll_dir = g_extra_dll_dir;
    RET(1);
}
static void k_SetDllDirectoryA(w32 *w) { set_dll_dir(w, ARG(0) ? GSTR(ARG(0)) : 0); }
static void k_SetDllDirectoryW(w32 *w) {
    if (!ARG(0)) { set_dll_dir(w, 0); return; }
    char s[1024]; w32_wtoa(w, ARG(0), s, sizeof s); set_dll_dir(w, s);
}
/* AddDllDirectory returns an opaque cookie, and the caller only ever passes
 * it back to RemoveDllDirectory. One search directory is kept, so the cookie
 * can be any non-NULL value -- but it has to be non-NULL, because that is how
 * the caller tests for failure. */
static void k_AddDllDirectory(w32 *w) {
    if (!ARG(0)) { w32_set_last_error(w, ERROR_INVALID_PARAMETER); RET(0); return; }
    char s[1024]; w32_wtoa(w, ARG(0), s, sizeof s);
    set_dll_dir(w, s);
    RET(1);          /* a cookie, not a boolean */
}
static void k_RemoveDllDirectory(w32 *w) { g_extra_dll_dir[0] = 0; w->dll_dir = 0; RET(1); }

/* lstrcat is the Win32 spelling of strcat, and it is what an installer built
 * without a CRT uses to join paths. */
static void k_lstrcatA(w32 *w) {
    uint64_t dst = ARG(0);
    const char *src = ARG(1) ? GSTR(ARG(1)) : "";
    /* One byte first, to establish that the destination is guest memory at
     * all; the length of what will be written is only known once its existing
     * terminator has been found, so the range is checked again after that. */
    char *d = W32PN(w, dst, 1);
    if (!d) { RET(0); return; }
    size_t at = strlen(d);
    if (!W32PN(w, dst, at + strlen(src) + 1)) { RET(0); return; }
    memcpy(d + at, src, strlen(src) + 1);
    RET(dst);
}
static void k_lstrcatW(w32 *w) {
    uint16_t *d = W32PN(w, ARG(0), 2);
    if (!d) { RET(0); return; }
    size_t at = 0; while (d[at]) at++;
    const uint16_t *sp = ARG(1) ? W32PN(w, ARG(1), 2) : 0;
    size_t sl = 0; if (sp) while (sp[sl]) sl++;
    if (!W32PN(w, ARG(0), 2 * (at + sl + 1))) { RET(0); return; }
    if (sp) { size_t i = 0; for (; i < sl; i++) d[at + i] = sp[i]; d[at + i] = 0; }
    RET(ARG(0));
}

/* ---- the wide half, which is what an installer actually calls --------------
 *
 * NSIS is a Unicode program: every path it touches goes through the W forms.
 * The A forms were here and the W forms were not, so a silent install stopped
 * on GetSystemDirectoryW and then, one function later, on wsprintfW -- two
 * calls, in a list that looked from the outside like seventy-five.
 *
 * (It looked like seventy-five because importing the installer *as a game*
 * reports everything in its import table, and most of that table is the
 * dialog it draws. /S never opens a window, so none of comctl32, the gdi32
 * font and brush calls, or the thirty-odd user32 dialog functions is ever
 * reached. The number that matters is what a run actually calls.)
 */

/* Write a narrow string into a guest wide buffer, bounded by `cch`
 * characters. Returns what the API family returns: the length written, or the
 * length needed when it does not fit. */
static uint32_t put_wide(w32 *w, uint64_t buf, uint32_t cch, const char *s) {
    size_t l = strlen(s);
    if (!buf || cch <= l) return (uint32_t)l + 1;      /* needed, including the NUL */
    uint16_t *d = W32PN(w, buf, 2 * (l + 1));
    if (!d) return 0;
    for (size_t i = 0; i < l; i++) d[i] = (uint8_t)s[i];
    d[l] = 0;
    return (uint32_t)l;
}

static void k_GetSystemDirectoryW(w32 *w) {
    RET(put_wide(w, ARG(0), (uint32_t)ARG(1), "C:\\Windows\\System32"));
}
static void k_GetWindowsDirectoryW(w32 *w) {
    RET(put_wide(w, ARG(0), (uint32_t)ARG(1), "C:\\Windows"));
}
static void k_GetSystemWow64DirectoryW(w32 *w) {
    RET(put_wide(w, ARG(0), (uint32_t)ARG(1), "C:\\Windows\\SysWOW64"));
}

/* GetFullPathNameW(name, cch, buf, &filepart). Relative names resolve against
 * the current directory, which is real now. */
static void k_GetFullPathNameW(w32 *w) {
    char s[1024]; w32_wtoa(w, ARG(0), s, sizeof s);
    char full[2400];
    if (s[1] == ':') snprintf(full, sizeof full, "%.*s", (int)sizeof full - 1, s);
    else snprintf(full, sizeof full, "%.*s\\%.*s", P_DIR,
                  g_cwd_win[0] ? g_cwd_win : "C:\\xcore", P_REST, s);
    uint32_t n = put_wide(w, ARG(2), (uint32_t)ARG(1), full);
    /* The file part points *into* the caller's buffer, after the last
     * separator -- a pointer to our own copy would dangle the moment we
     * returned. */
    if (ARG(3) && n && n <= (uint32_t)ARG(1)) {
        const char *slash = strrchr(full, '\\');
        uint64_t fp = slash ? ARG(2) + 2u * (uint32_t)(slash + 1 - full) : ARG(2);
        w32_write(w, ARG(3), w->is32 ? 4 : 8, fp);
    }
    RET(n);
}

/* SearchPathW(path, file, ext, cch, buf, &filepart). An installer uses it to
 * find a helper next to itself or in the system directory. */
static void k_SearchPathW(w32 *w) {
    char file[512], ext[64] = "";
    w32_wtoa(w, ARG(1), file, sizeof file);
    if (ARG(2)) w32_wtoa(w, ARG(2), ext, sizeof ext);
    char name[640];
    snprintf(name, sizeof name, "%.*s%.*s", (int)sizeof file - 1, file, (int)sizeof ext - 1,
             (strchr(file, '.') || !ext[0]) ? "" : ext);

    /* Where the real one looks, in order: the directory given, then the
     * program's own, then the system directory. */
    const char *dirs[3];
    char given[512] = "";
    int n = 0;
    if (ARG(0)) { w32_wtoa(w, ARG(0), given, sizeof given); dirs[n++] = given; }
    dirs[n++] = "C:\\xcore";
    dirs[n++] = "C:\\Windows\\System32";
    for (int i = 0; i < n; i++) {
        char win[1400], host[4096];
        snprintf(win, sizeof win, "%.*s\\%.*s", P_DIR, dirs[i],
                 (int)sizeof win - P_DIR - 2, name);
        host_path(w, win, host, sizeof host);
        struct stat st;
        if (stat(host, &st) || !S_ISREG(st.st_mode)) continue;
        uint32_t got = put_wide(w, ARG(4), (uint32_t)ARG(3), win);
        if (ARG(5) && got && got <= (uint32_t)ARG(3)) {
            const char *slash = strrchr(win, '\\');
            uint64_t fp = slash ? ARG(4) + 2u * (uint32_t)(slash + 1 - win) : ARG(4);
            w32_write(w, ARG(5), w->is32 ? 4 : 8, fp);
        }
        RET(got); return;
    }
    w32_set_last_error(w, ERROR_FILE_NOT_FOUND);
    RET(0);
}

/* ExpandEnvironmentStringsW: %NAME% substitution. An installer builds its
 * default directory out of %PROGRAMFILES%, so getting this wrong is how an
 * install lands somewhere strange. Unknown names are left as they are, which
 * is what the real one does. */
static void k_ExpandEnvironmentStringsW(w32 *w) {
    char src[2048]; w32_wtoa(w, ARG(0), src, sizeof src);
    char out[4096]; size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < sizeof out; ) {
        if (src[i] != '%') { out[o++] = src[i++]; continue; }
        const char *close = strchr(src + i + 1, '%');
        if (!close) { out[o++] = src[i++]; continue; }
        char name[128];
        size_t len = (size_t)(close - (src + i + 1));
        if (len >= sizeof name) { out[o++] = src[i++]; continue; }
        memcpy(name, src + i + 1, len); name[len] = 0;
        const char *val = w32_env_lookup(w, name);
        if (!val) { out[o++] = src[i++]; continue; }
        o += (size_t)snprintf(out + o, sizeof out - o, "%.*s", (int)(sizeof out - o - 1), val);
        i += len + 2;
    }
    out[o] = 0;
    RET(put_wide(w, ARG(1), (uint32_t)ARG(2), out));
}

static void k_SetEnvironmentVariableW(w32 *w) {
    char name[256], val[2048] = "";
    w32_wtoa(w, ARG(0), name, sizeof name);
    if (ARG(1)) w32_wtoa(w, ARG(1), val, sizeof val);
    RET(bool_(w32_env_set(name, ARG(1) ? val : 0)));
}

/* lstrcmp compares as strings; lstrcmpi ignores case. Wide, so compared a
 * code unit at a time -- these are used on paths, which are ASCII in
 * practice, and a full collation would be a different function. */
static void k_lstrcmpW(w32 *w) {
    const uint16_t *a = W32PN(w, ARG(0), 2), *b = W32PN(w, ARG(1), 2);
    if (!a || !b) { RET(0); return; }
    size_t i = 0;
    for (; a[i] && a[i] == b[i]; i++) { }
    RET((uint64_t)(int64_t)(a[i] < b[i] ? -1 : a[i] > b[i] ? 1 : 0));
}
static void k_lstrcmpiW(w32 *w) {
    const uint16_t *a = W32PN(w, ARG(0), 2), *b = W32PN(w, ARG(1), 2);
    if (!a || !b) { RET(0); return; }
    size_t i = 0;
    for (;;) {
        uint16_t x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (uint16_t)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (uint16_t)(y + 32);
        if (!x || x != y) { RET((uint64_t)(int64_t)(x < y ? -1 : x > y ? 1 : 0)); return; }
        i++;
    }
}
/* lstrcpyn copies at most cch-1 characters and always terminates -- the
 * always-terminates part is the difference from strncpy and the reason it is
 * used. */
static void k_lstrcpynW(w32 *w) {
    uint32_t cch = (uint32_t)ARG(2);
    if (!ARG(0) || !cch) { RET(0); return; }
    uint16_t *d = W32PN(w, ARG(0), 2 * (uint64_t)cch);
    const uint16_t *s = ARG(1) ? W32PN(w, ARG(1), 2) : 0;
    if (!d) { RET(0); return; }
    uint32_t i = 0;
    if (s) for (; i + 1 < cch && s[i]; i++) d[i] = s[i];
    d[i] = 0;
    RET(ARG(0));
}
static void k_lstrcpynA(w32 *w) {
    uint32_t cch = (uint32_t)ARG(2);
    if (!ARG(0) || !cch) { RET(0); return; }
    char *d = W32PN(w, ARG(0), cch);
    const char *s = ARG(1) ? GSTR(ARG(1)) : 0;
    if (!d) { RET(0); return; }
    uint32_t i = 0;
    if (s) for (; i + 1 < cch && s[i]; i++) d[i] = s[i];
    d[i] = 0;
    RET(ARG(0));
}

/* CompareFileTime: -1, 0 or 1. An installer uses it to decide whether the
 * file it is about to write is newer than the one already there. */
static void k_CompareFileTime(w32 *w) {
    uint64_t a = ARG(0) ? w32_read(w, ARG(0), 8) : 0;
    uint64_t b = ARG(1) ? w32_read(w, ARG(1), 8) : 0;
    RET((uint64_t)(int64_t)(a < b ? -1 : a > b ? 1 : 0));
}
static void k_SetFileTime(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0));
    if (!h || h->type != H_FILE) { w32_set_last_error(w, ERROR_INVALID_HANDLE); RET(0); return; }
    /* Only the write time is representable here, and only to the second.
     * Accepting the call and setting what can be set is right: an installer
     * that cannot stamp a file carries on, one that gets a failure may not. */
    uint64_t ft = ARG(3) ? w32_read(w, ARG(3), 8) : 0;
    if (ft) {
        struct timespec ts[2];
        ts[0].tv_sec = 0; ts[0].tv_nsec = UTIME_OMIT;
        ts[1].tv_sec = (time_t)(ft / 10000000ull - 11644473600ull);
        ts[1].tv_nsec = (long)((ft % 10000000ull) * 100);
        (void)futimens(h->fd, ts);
    }
    RET(1);
}

/* MulDiv(a, b, c) = a*b/c with rounding, in 64 bits so it cannot overflow
 * on the way. It is in kernel32 because 16-bit Windows had no 64-bit
 * arithmetic; installers still use it to scale dialog units. */
static void k_MulDiv(w32 *w) {
    int64_t a = (int32_t)ARG(0), b = (int32_t)ARG(1), c = (int32_t)ARG(2);
    if (!c) { RET((uint64_t)(int64_t)-1); return; }
    int64_t n = a * b;
    int64_t half = (c > 0) == (n >= 0) ? c / 2 : -(c / 2);
    RET((uint64_t)(int64_t)(int32_t)((n + half) / c));
}

/* GlobalLock/GlobalUnlock. Handles from GlobalAlloc here are already
 * pointers -- there is no moveable memory to pin -- so locking one is the
 * identity and unlocking it succeeds. */
static void k_GlobalLock(w32 *w)   { RET(ARG(0)); }
static void k_GlobalUnlock(w32 *w) { (void)w; RET(1); }
static void k_GlobalSize(w32 *w)   { RET(ARG(0) ? w32_heap_size(w, ARG(0)) : 0); }

static void k_WritePrivateProfileStringW(w32 *w) {
    /* The same work as the A form on narrowed arguments: an .ini is bytes on
     * disk either way, and having two implementations of the rewrite would
     * mean two chances to get the section-insert case wrong. */
    char app[256] = "", key[256] = "", val[1024] = "", file[1024] = "";
    if (ARG(0)) w32_wtoa(w, ARG(0), app, sizeof app);
    if (ARG(1)) w32_wtoa(w, ARG(1), key, sizeof key);
    if (ARG(2)) w32_wtoa(w, ARG(2), val, sizeof val);
    if (ARG(3)) w32_wtoa(w, ARG(3), file, sizeof file);
    RET(bool_(w32_ini_write(w, app, ARG(1) ? key : 0, ARG(2) ? val : 0, file)));
}

static void k_GetCurrentProcessorNumber(w32 *w) { RET(0); }

/* ------------------------------------------------------------- resources */

/* A program's own data, compiled into it: dialog templates, strings, icons,
 * and for an installer the compressed payload itself. FindResource returns
 * the address of the directory's data entry -- which is what Windows returns
 * too -- so Load and Lock have nothing left to do but hand it on, and
 * SizeofResource reads the length out of the same entry.
 *
 * A resource named by an integer arrives as a value below 0x10000 rather
 * than as a pointer, which is what MAKEINTRESOURCE does, so the argument is
 * passed through untouched and the finder decides which it is. */
static void k_FindResourceA(w32 *w) { RET(w32_find_resource(w, ARG(0), ARG(2), ARG(1), 0)); }
static void k_FindResourceW(w32 *w) { RET(w32_find_resource(w, ARG(0), ARG(2), ARG(1), 1)); }
static void k_FindResourceExA(w32 *w) { RET(w32_find_resource(w, ARG(0), ARG(1), ARG(2), 0)); }
static void k_FindResourceExW(w32 *w) { RET(w32_find_resource(w, ARG(0), ARG(1), ARG(2), 1)); }
static void k_LoadResource(w32 *w) { RET(w32_resource_data(w, ARG(1), 0)); }
/* The resource is already mapped, so locking it is returning the pointer --
 * and on Windows since 3.1 that is literally all LockResource does. */
static void k_LockResource(w32 *w) { RET(ARG(0)); }
static void k_FreeResource(w32 *w) { (void)w; RET(0); }
static void k_SizeofResource(w32 *w) {
    uint32_t n = 0;
    if (w32_resource_data(w, ARG(1), &n)) { RET(n); return; }
    RET(0);
}
/* A string resource is sixteen strings to a block, each a word of length
 * followed by that many UTF-16 characters, and the block holding id N is
 * (N / 16) + 1. Nothing about that is guessable from the API, which is why
 * a program that loads its messages this way otherwise gets nothing. */
int w32_load_string(w32 *w, uint64_t inst, uint32_t id, char *out, size_t cap) {
    out[0] = 0;
    uint64_t hr = w32_find_resource(w, inst, 6 /* RT_STRING */, (id / 16) + 1, 0);
    if (!hr) return 0;
    uint32_t size = 0;
    uint64_t p = w32_resource_data(w, hr, &size);
    if (!p) return 0;
    uint64_t at = p, end = p + size;
    for (uint32_t k = 0; k < 16; k++) {
        if (at + 2 > end) return 0;
        uint32_t len = (uint32_t)w32_read(w, at, 2);
        at += 2;
        if (k == (id % 16)) {
            size_t n = 0;
            for (uint32_t i = 0; i < len && at + 2 <= end && n + 1 < cap; i++, at += 2) {
                uint32_t ch = (uint32_t)w32_read(w, at, 2);
                out[n++] = ch < 128 ? (char)ch : '?';
            }
            out[n] = 0;
            return (int)n;
        }
        at += (uint64_t)len * 2;
    }
    return 0;
}
/* ---- what the C runtime does before main ---------------------------------
 *
 * A modern MSVC program initialises its locale before it runs a line of its
 * own code, and that init walks a handful of NLS calls. Every one of them
 * missing is a program that never reaches its entry point -- which is why a
 * GameMaker game that had 232 of its imports satisfied still did nothing.
 *
 * There is one locale here and it is the invariant one. That is a real
 * answer, not a placeholder: the drive is ASCII, the collation is ordinal,
 * and a program told so behaves consistently. What it is not is *the user's*
 * locale, so a game that formats a date will format it the American way.
 * Saying that plainly is better than inventing a locale we cannot support.
 */
enum { LOCALE_INVARIANT_ = 0x007F, LOCALE_USER_DEFAULT_ = 0x0400 };

/* GetLocaleInfoW(locale, type, buf, cch) -- the types a CRT actually asks
 * for on the way up. Anything else gets an empty string rather than a
 * failure, because a CRT that cannot read one field copes and a CRT that
 * gets an error from the whole call sometimes does not. */
static void locale_info(w32 *w, int wide) {
    uint32_t type = (uint32_t)ARG(1);
    uint64_t out = ARG(2);
    int cap = (int)(int32_t)(uint32_t)ARG(3);
    const char *v = "";
    char num[16];
    switch (type & 0xFFFF) {
    case 0x0002: v = "en-US";   break;   /* LOCALE_SLOCALIZEDDISPLAYNAME-ish */
    case 0x0003: v = "English"; break;   /* SENGLANGUAGE */
    case 0x0005: v = "eng";     break;   /* SABBREVLANGNAME */
    case 0x0006: v = "United States"; break;
    case 0x0007: v = "USA";     break;
    case 0x0009: v = "US";      break;   /* SISO3166CTRYNAME */
    case 0x000E: v = ".";       break;   /* SDECIMAL */
    case 0x000F: v = ",";       break;   /* STHOUSAND */
    case 0x0014: v = "2";       break;   /* IDIGITS */
    case 0x001B: v = ":";       break;   /* STIME */
    case 0x001D: v = "M/d/yyyy"; break;  /* SSHORTDATE */
    case 0x0020: v = "dddd, MMMM d, yyyy"; break;  /* SLONGDATE */
    case 0x0025: v = "AM";      break;
    case 0x0026: v = "PM";      break;
    case 0x0059: v = "/";       break;   /* SDATE */
    case 0x1004: snprintf(num, sizeof num, "%u", 1252); v = num; break;  /* IDEFAULTANSICODEPAGE */
    case 0x0059 + 1: v = ""; break;
    default: v = ""; break;
    }
    int n = (int)strlen(v) + 1;
    if (!out || cap == 0) { RET((uint64_t)(uint32_t)n); return; }   /* asking for the size */
    if (cap < n) { w32_set_last_error(w, 122); RET(0); return; }    /* ERROR_INSUFFICIENT_BUFFER */
    for (int i = 0; i < n; i++) {
        if (wide) w32_write(w, out + (unsigned)i * 2, 2, (uint8_t)v[i]);
        else      w32_write(w, out + (unsigned)i, 1, (uint8_t)v[i]);
    }
    RET((uint64_t)(uint32_t)n);
}
static void k_GetLocaleInfoW(w32 *w) { locale_info(w, 1); }
static void k_GetLocaleInfoA(w32 *w) { locale_info(w, 0); }
static void k_GetLocaleInfoEx(w32 *w) {
    /* (name, type, buf, cch): the locale is named rather than numbered, and
     * the answer is the same one. */
    uint64_t saved = ARG(0);
    (void)saved;
    locale_info(w, 1);
}
static void k_IsValidLocale(w32 *w) { (void)w; RET(1); }
/* EnumSystemLocales calls back once per locale. Calling back once, with the
 * invariant locale, is a truthful enumeration of what is here -- and a CRT
 * that gets zero callbacks concludes the system is broken. */
static void enum_locales(w32 *w, int wide) {
    uint64_t fn = ARG(0);
    if (!fn) { RET(0); return; }
    uint64_t name = w32_heap_alloc(w, 32);
    const char *s = "0409";
    for (int i = 0; i <= 4; i++) {
        if (wide) w32_write(w, name + (unsigned)i * 2, 2, (uint8_t)s[i]);
        else      w32_write(w, name + (unsigned)i, 1, (uint8_t)s[i]);
    }
    uint64_t args[1] = { name };
    w32_call_guest(w, fn, 1, args);
    RET(1);
}
static void k_EnumSystemLocalesW(w32 *w) { enum_locales(w, 1); }
static void k_EnumSystemLocalesA(w32 *w) { enum_locales(w, 0); }
static void k_EnumSystemLocalesEx(w32 *w) { enum_locales(w, 1); }

/* LCMapStringEx / LCMapStringW: the CRT's case folding and sort keys.
 * LCMAP_UPPERCASE is 0x200 and LCMAP_LOWERCASE 0x100; a sort key
 * (LCMAP_SORTKEY, 0x400) is asked for by collation and the ordinal answer
 * is the string itself, which is a consistent ordering even though it is not
 * a linguistic one. */
static void lcmap(w32 *w, uint32_t flags, uint64_t src, int srclen, uint64_t dst, int dstlen) {
    char buf[1024];
    if (srclen < 0) w32_wtoa(w, src, buf, sizeof buf);
    else w32_wtoa_n(w, src, srclen, buf, sizeof buf);
    size_t n = strlen(buf);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (flags & 0x200) buf[i] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
        else if (flags & 0x100) buf[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    int need = (int)n + 1;
    if (!dst || dstlen == 0) { RET((uint64_t)(uint32_t)need); return; }
    if (dstlen < need) { w32_set_last_error(w, 122); RET(0); return; }
    for (int i = 0; i < need; i++) w32_write(w, dst + (unsigned)i * 2, 2, (uint8_t)buf[i]);
    RET((uint64_t)(uint32_t)need);
}
static void k_LCMapStringEx(w32 *w) {
    /* (locale, flags, src, srclen, dst, dstlen, version, reserved, sortHandle) */
    lcmap(w, (uint32_t)ARG(1), ARG(2), (int)(int32_t)(uint32_t)ARG(3), ARG(4), (int)(int32_t)(uint32_t)ARG(5));
}
static void k_LCMapStringW(w32 *w) {
    lcmap(w, (uint32_t)ARG(1), ARG(2), (int)(int32_t)(uint32_t)ARG(3), ARG(4), (int)(int32_t)(uint32_t)ARG(5));
}
static void k_CompareStringW(w32 *w) {
    char a[512], b[512];
    w32_wtoa_n(w, ARG(2), (int)(int32_t)(uint32_t)ARG(3), a, sizeof a);
    w32_wtoa_n(w, ARG(4), (int)(int32_t)(uint32_t)ARG(5), b, sizeof b);
    int r = ((uint32_t)ARG(1) & 1) ? strcasecmp(a, b) : strcmp(a, b);   /* NORM_IGNORECASE */
    RET(r < 0 ? 1 : r > 0 ? 3 : 2);            /* CSTR_LESS/EQUAL/GREATER */
}
static void k_CompareStringEx(w32 *w) {
    char a[512], b[512];
    w32_wtoa_n(w, ARG(2), (int)(int32_t)(uint32_t)ARG(3), a, sizeof a);
    w32_wtoa_n(w, ARG(4), (int)(int32_t)(uint32_t)ARG(5), b, sizeof b);
    int r = ((uint32_t)ARG(1) & 1) ? strcasecmp(a, b) : strcmp(a, b);
    RET(r < 0 ? 1 : r > 0 ? 3 : 2);
}
static void k_CompareStringOrdinal(w32 *w) {
    char a[512], b[512];
    w32_wtoa_n(w, ARG(0), (int)(int32_t)(uint32_t)ARG(1), a, sizeof a);
    w32_wtoa_n(w, ARG(2), (int)(int32_t)(uint32_t)ARG(3), b, sizeof b);
    int r = ARG(4) ? strcasecmp(a, b) : strcmp(a, b);
    RET(r < 0 ? 1 : r > 0 ? 3 : 2);
}

/* ---- dates and times ---------------------------------------------------- */

/* FILETIME is 100-nanosecond ticks since 1601. The gap to the Unix epoch is
 * 11644473600 seconds, and it is the one number this whole area turns on. */
enum { FT_EPOCH_DELTA = 11644473600ll };

static void put_systemtime(w32 *w, uint64_t p, const struct tm *t, int ms) {
    if (!p) return;
    uint16_t v[8] = {
        (uint16_t)(t->tm_year + 1900), (uint16_t)(t->tm_mon + 1), (uint16_t)t->tm_wday,
        (uint16_t)t->tm_mday, (uint16_t)t->tm_hour, (uint16_t)t->tm_min,
        (uint16_t)t->tm_sec, (uint16_t)ms,
    };
    for (int i = 0; i < 8; i++) w32_write(w, p + (unsigned)i * 2, 2, v[i]);
}
static void k_FileTimeToSystemTime(w32 *w) {
    uint64_t f = ARG(0);
    if (!f) { RET(0); return; }
    uint64_t ft = w32_read(w, f, 4) | (w32_read(w, f + 4, 4) << 32);
    time_t secs = (time_t)(ft / 10000000ull) - FT_EPOCH_DELTA;
    int ms = (int)((ft / 10000ull) % 1000ull);
    struct tm tmv;
    if (!gmtime_r(&secs, &tmv)) { RET(0); return; }
    put_systemtime(w, ARG(1), &tmv, ms);
    RET(1);
}
static void k_SystemTimeToFileTime(w32 *w) {
    uint64_t p = ARG(0), out = ARG(1);
    if (!p || !out) { RET(0); return; }
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = (int)w32_read(w, p, 2) - 1900;
    tmv.tm_mon  = (int)w32_read(w, p + 2, 2) - 1;
    tmv.tm_mday = (int)w32_read(w, p + 6, 2);
    tmv.tm_hour = (int)w32_read(w, p + 8, 2);
    tmv.tm_min  = (int)w32_read(w, p + 10, 2);
    tmv.tm_sec  = (int)w32_read(w, p + 12, 2);
    time_t secs = timegm(&tmv);
    uint64_t ft = ((uint64_t)secs + FT_EPOCH_DELTA) * 10000000ull
                + (uint64_t)w32_read(w, p + 14, 2) * 10000ull;
    w32_write(w, out, 4, ft & 0xFFFFFFFFu);
    w32_write(w, out + 4, 4, ft >> 32);
    RET(1);
}
/* There is one time zone here and it is UTC. A game that shows a save's
 * timestamp will show it in UTC, which is wrong by an offset rather than
 * wrong in a way that breaks anything -- and inventing a zone would be worse
 * than being consistently one. */
static void k_SystemTimeToTzSpecificLocalTime(w32 *w) {
    uint64_t src = ARG(1), dst = ARG(2);
    if (!src || !dst) { RET(0); return; }
    for (int i = 0; i < 8; i++) w32_write(w, dst + (unsigned)i * 2, 2, w32_read(w, src + (unsigned)i * 2, 2));
    RET(1);
}
static void k_TzSpecificLocalTimeToSystemTime(w32 *w) { k_SystemTimeToTzSpecificLocalTime(w); }
static void k_FileTimeToLocalFileTime(w32 *w) {
    if (!ARG(0) || !ARG(1)) { RET(0); return; }
    w32_write(w, ARG(1), 4, w32_read(w, ARG(0), 4));
    w32_write(w, ARG(1) + 4, 4, w32_read(w, ARG(0) + 4, 4));
    RET(1);
}
static void k_LocalFileTimeToFileTime(w32 *w) { k_FileTimeToLocalFileTime(w); }

/* GetDateFormat / GetTimeFormat: a program shows a save's date with these.
 * The invariant locale's formats, which is what GetLocaleInfoW above says
 * they are, so the two agree. */
static void date_time_format(w32 *w, int is_date, int wide) {
    uint64_t stp = ARG(2), out = ARG(4);
    int cap = (int)(int32_t)(uint32_t)ARG(5);
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    if (stp) {
        tmv.tm_year = (int)w32_read(w, stp, 2) - 1900;
        tmv.tm_mon  = (int)w32_read(w, stp + 2, 2) - 1;
        tmv.tm_mday = (int)w32_read(w, stp + 6, 2);
        tmv.tm_hour = (int)w32_read(w, stp + 8, 2);
        tmv.tm_min  = (int)w32_read(w, stp + 10, 2);
        tmv.tm_sec  = (int)w32_read(w, stp + 12, 2);
    } else {
        time_t now = time(0);
        gmtime_r(&now, &tmv);
    }
    char buf[128];
    if (is_date) snprintf(buf, sizeof buf, "%d/%d/%04d", tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_year + 1900);
    else {
        int h = tmv.tm_hour % 12; if (!h) h = 12;
        snprintf(buf, sizeof buf, "%d:%02d:%02d %s", h, tmv.tm_min, tmv.tm_sec, tmv.tm_hour < 12 ? "AM" : "PM");
    }
    int n = (int)strlen(buf) + 1;
    if (!out || cap == 0) { RET((uint64_t)(uint32_t)n); return; }
    if (cap < n) { w32_set_last_error(w, 122); RET(0); return; }
    for (int i = 0; i < n; i++) {
        if (wide) w32_write(w, out + (unsigned)i * 2, 2, (uint8_t)buf[i]);
        else      w32_write(w, out + (unsigned)i, 1, (uint8_t)buf[i]);
    }
    RET((uint64_t)(uint32_t)n);
}
static void k_GetDateFormatW(w32 *w) { date_time_format(w, 1, 1); }
static void k_GetDateFormatA(w32 *w) { date_time_format(w, 1, 0); }
static void k_GetTimeFormatW(w32 *w) { date_time_format(w, 0, 1); }
static void k_GetTimeFormatA(w32 *w) { date_time_format(w, 0, 0); }

/* ---- the version gate --------------------------------------------------- */

/* VerifyVersionInfo is how a program asks "am I on Windows 7 or later". We
 * report Windows 10, and the honest way to answer a comparison against it is
 * to actually perform the comparison rather than always saying yes: a
 * program that checks it is *not* on something newer than it supports
 * deserves a real answer too. */
static void k_VerSetConditionMask(w32 *w) {
    /* (mask, typeBitMask, condition) -> mask with the condition packed in.
     * Three bits per type, and the type is a bit position; the packing is
     * only ever read back by VerifyVersionInfo, so it has to be consistent
     * with what that does and nothing more. */
    uint64_t mask = w->is32 ? (ARG(0) | (ARG(1) << 32)) : ARG(0);
    uint32_t type = (uint32_t)(w->is32 ? ARG(2) : ARG(1));
    uint32_t cond = (uint32_t)(w->is32 ? ARG(3) : ARG(2)) & 7;
    int slot = 0;
    for (uint32_t t = type; t && slot < 20; t >>= 1, slot++) if (t & 1) break;
    mask |= (uint64_t)cond << (slot * 3);
    w32_ret64(w, mask);
}
static void k_VerifyVersionInfoW(w32 *w) {
    /* OSVERSIONINFOEX: size, major, minor, build, platform, csd[128], ... */
    uint64_t p = ARG(0);
    uint32_t types = (uint32_t)ARG(1);
    if (!p) { RET(0); return; }
    uint32_t want_major = (uint32_t)w32_read(w, p + 4, 4);
    uint32_t want_minor = (uint32_t)w32_read(w, p + 8, 4);
    /* What we claim to be, which is what GetVersionEx says as well. */
    const uint32_t have_major = 10, have_minor = 0;
    /* VER_MAJORVERSION 0x0002, VER_MINORVERSION 0x0001 -- the only two a
     * game's "is this new enough" check uses. Anything else passes. */
    int ok = 1;
    if (types & 0x0002) {
        if (have_major < want_major) ok = 0;
        else if (have_major == want_major && (types & 0x0001) && have_minor < want_minor) ok = 0;
    }
    if (!ok) w32_set_last_error(w, 1150);      /* ERROR_OLD_WIN_VERSION */
    RET(ok);
}
static void k_VerifyVersionInfoA(w32 *w) { k_VerifyVersionInfoW(w); }

/* ---- the rest of what a game asks kernel32 for --------------------------- */

static void k_DebugBreak(w32 *w) {
    /* There is no debugger to break into. Saying so once is better than a
     * trap the run cannot continue from. */
    w32_note_refused(w, "kernel32!DebugBreak (no debugger attached)");
    RET(0);
}
static void k_GetConsoleWindow(w32 *w) { (void)w; RET(0); }   /* there is no console window */
static void k_ReadConsoleW(w32 *w) {
    /* Nothing is typed at a console that does not exist. Zero characters and
     * success is end-of-input, which every caller handles. */
    if (ARG(3)) w32_write(w, ARG(3), 4, 0);
    RET(1);
}
static void k_PeekNamedPipe(w32 *w) {
    for (int i = 2; i <= 4; i++) if (ARG((unsigned)i)) w32_write(w, ARG((unsigned)i), 4, 0);
    RET(0);
}
static void k_SetProcessInformation(w32 *w) { (void)w; RET(1); }
static void k_HeapWalk(w32 *w) { w32_set_last_error(w, 259); RET(0); }   /* ERROR_NO_MORE_ITEMS */
static void k_K32GetProcessMemoryInfo(w32 *w) {
    /* PROCESS_MEMORY_COUNTERS: cb, faults, then eight size_t counters. A
     * game reports these on a debug overlay; zeros with the right size are
     * an honest "we do not account for this" rather than an invented figure. */
    uint64_t p = ARG(1);
    uint32_t cb = (uint32_t)ARG(2);
    if (!p || cb < 8) { RET(0); return; }
    void *d = W32PN(w, p, cb);
    if (!d) { w32_set_last_error(w, ERROR_NOACCESS); RET(0); return; }
    memset(d, 0, cb);
    w32_write(w, p, 4, cb);
    RET(1);
}
static void k_FreeLibraryAndExitThread(w32 *w) { w32_thread_exit_self(w, (uint32_t)ARG(1)); }
/* Waitable timers: a timer object that a wait can block on. Built on the
 * event machinery, because that is what one is. */
static void k_CreateWaitableTimerW(w32 *w) { RET(w32_make_event(w, 1, 0)); }
static void k_CreateWaitableTimerA(w32 *w) { RET(w32_make_event(w, 1, 0)); }
static void k_CreateWaitableTimerExW(w32 *w) { RET(w32_make_event(w, 1, 0)); }
static void k_SetWaitableTimer(w32 *w) {
    /* A due time in the past, or none, means signalled now -- which is the
     * only case that matters here, because nothing else is going to fire it. */
    w32_set_event(w, ARG(0), 1);
    RET(1);
}
static void k_CancelWaitableTimer(w32 *w) { w32_set_event(w, ARG(0), 0); RET(1); }
static void k_CreateEventExA(w32 *w) {
    /* (attrs, name, flags, access): CREATE_EVENT_MANUAL_RESET is 1 and
     * CREATE_EVENT_INITIAL_SET is 2 -- not the same bits as CreateEvent's
     * arguments, which is exactly the sort of thing that silently makes an
     * auto-reset event manual. */
    uint32_t f = (uint32_t)ARG(2);
    RET(w32_make_event(w, (f & 1) != 0, (f & 2) != 0));
}
static void k_GetFileAttributesExW(w32 *w) {
    /* (name, level, out): WIN32_FILE_ATTRIBUTE_DATA is attributes, three
     * FILETIMEs, then the size as two words. */
    char win[1024], host[1024];
    w32_wtoa(w, ARG(0), win, sizeof win);
    w32_host_path(w, win, host, sizeof host);
    struct stat st;
    if (stat(host, &st)) { w32_set_last_error(w, 2); RET(0); return; }
    uint64_t p = ARG(2);
    if (!p) { RET(0); return; }
    uint32_t attr = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
    w32_write(w, p, 4, attr);
    uint64_t ft = ((uint64_t)st.st_mtime + FT_EPOCH_DELTA) * 10000000ull;
    for (int i = 0; i < 3; i++) {
        w32_write(w, p + 4 + (unsigned)i * 8, 4, ft & 0xFFFFFFFFu);
        w32_write(w, p + 8 + (unsigned)i * 8, 4, ft >> 32);
    }
    w32_write(w, p + 28, 4, (uint64_t)st.st_size >> 32);
    w32_write(w, p + 32, 4, (uint64_t)st.st_size & 0xFFFFFFFFu);
    RET(1);
}
static void k_GetFileAttributesExA(w32 *w) {
    char host[1024];
    w32_host_path(w, ARG(0) ? w32_str(w, ARG(0)) : "", host, sizeof host);
    struct stat st;
    if (stat(host, &st)) { w32_set_last_error(w, 2); RET(0); return; }
    uint64_t p = ARG(2);
    if (!p) { RET(0); return; }
    w32_write(w, p, 4, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
    uint64_t ft = ((uint64_t)st.st_mtime + FT_EPOCH_DELTA) * 10000000ull;
    for (int i = 0; i < 3; i++) {
        w32_write(w, p + 4 + (unsigned)i * 8, 4, ft & 0xFFFFFFFFu);
        w32_write(w, p + 8 + (unsigned)i * 8, 4, ft >> 32);
    }
    w32_write(w, p + 28, 4, (uint64_t)st.st_size >> 32);
    w32_write(w, p + 32, 4, (uint64_t)st.st_size & 0xFFFFFFFFu);
    RET(1);
}
/* BY_HANDLE_FILE_INFORMATION: attributes, three times, volume serial, the
 * size high/low, link count, and the file id high/low. The id has to be
 * stable and distinct per file, and an inode is exactly that. */
static void k_GetFileInformationByHandle(w32 *w) {
    w32_handle *h = w32_handle_get(w, ARG(0));
    uint64_t p = ARG(1);
    if (!h || h->type != H_FILE || h->fd < 0 || !p) { w32_set_last_error(w, 6); RET(0); return; }
    struct stat st;
    if (fstat(h->fd, &st)) { w32_set_last_error(w, 5); RET(0); return; }
    w32_write(w, p, 4, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
    uint64_t ft = ((uint64_t)st.st_mtime + FT_EPOCH_DELTA) * 10000000ull;
    for (int i = 0; i < 3; i++) {
        w32_write(w, p + 4 + (unsigned)i * 8, 4, ft & 0xFFFFFFFFu);
        w32_write(w, p + 8 + (unsigned)i * 8, 4, ft >> 32);
    }
    w32_write(w, p + 28, 4, 0x57494E32u);                    /* volume serial */
    w32_write(w, p + 32, 4, (uint64_t)st.st_size >> 32);
    w32_write(w, p + 36, 4, (uint64_t)st.st_size & 0xFFFFFFFFu);
    w32_write(w, p + 40, 4, (uint64_t)st.st_nlink);
    w32_write(w, p + 44, 4, (uint64_t)st.st_ino >> 32);
    w32_write(w, p + 48, 4, (uint64_t)st.st_ino & 0xFFFFFFFFu);
    RET(1);
}
static void k_GetFinalPathNameByHandleW(w32 *w) {
    /* There is no way back from a descriptor to a name on every host, and a
     * wrong name is worse than none: a program uses this to decide whether
     * two handles are the same file. */
    w32_note_refused(w, "kernel32!GetFinalPathNameByHandle (no path from a handle here)");
    w32_set_last_error(w, 1);
    RET(0);
}
static void k_GetDriveTypeW(w32 *w) {
    char b[64] = "";
    if (ARG(0)) w32_wtoa(w, ARG(0), b, sizeof b);
    /* One drive, and it is fixed. DRIVE_FIXED is 3; a game asks so it can
     * decide whether to warn about running from removable media. */
    RET((b[0] == 'C' || b[0] == 'c' || !b[0]) ? 3 : 1);
}
/* FormatMessage turns an error code into a sentence. A game puts that
 * sentence in a message box when something fails, so an empty one turns a
 * useful report into "an error occurred". */
static void format_message(w32 *w, int wide) {
    uint32_t flags = (uint32_t)ARG(0);
    uint32_t id = (uint32_t)ARG(2);
    uint64_t out = ARG(4);
    uint32_t cap = (uint32_t)ARG(5);
    const char *text;
    switch (id) {
    case 0:   text = "The operation completed successfully."; break;
    case 2:   text = "The system cannot find the file specified."; break;
    case 3:   text = "The system cannot find the path specified."; break;
    case 5:   text = "Access is denied."; break;
    case 6:   text = "The handle is invalid."; break;
    case 8:   text = "Not enough memory resources are available."; break;
    case 32:  text = "The process cannot access the file because it is being used by another process."; break;
    case 87:  text = "The parameter is incorrect."; break;
    case 112: text = "There is not enough space on the disk."; break;
    case 122: text = "The data area passed to a system call is too small."; break;
    case 183: text = "Cannot create a file when that file already exists."; break;
    case 1150: text = "The specified program requires a newer version of Windows."; break;
    default:  text = "An error occurred."; break;
    }
    size_t n = strlen(text);
    /* FORMAT_MESSAGE_ALLOCATE_BUFFER (0x100): the caller gets a pointer
     * written into its buffer argument rather than the text. */
    if (flags & 0x100) {
        uint64_t buf = w32_heap_alloc(w, (n + 1) * (wide ? 2 : 1));
        for (size_t i = 0; i <= n; i++) {
            if (wide) w32_write(w, buf + i * 2, 2, (uint8_t)text[i]);
            else      w32_write(w, buf + i, 1, (uint8_t)text[i]);
        }
        if (out) w32_write(w, out, (int)w32_ptrsize(w), buf);
        RET((uint64_t)(uint32_t)n);
        return;
    }
    if (!out || cap == 0) { RET(0); return; }
    if (n > cap - 1) n = cap - 1;
    for (size_t i = 0; i < n; i++) {
        if (wide) w32_write(w, out + i * 2, 2, (uint8_t)text[i]);
        else      w32_write(w, out + i, 1, (uint8_t)text[i]);
    }
    if (wide) w32_write(w, out + n * 2, 2, 0); else w32_write(w, out + n, 1, 0);
    RET((uint64_t)(uint32_t)n);
}
static void k_FormatMessageW(w32 *w) { format_message(w, 1); }
/* FindFirstFileEx is FindFirstFile with a filter it is allowed to ignore. */
static void k_FindFirstFileExW(w32 *w) {
    char b[1024];
    w32_wtoa(w, ARG(0), b, sizeof b);
    RET(find_first(w, b, ARG(2), 1));
}
static void k_FindFirstFileExA(w32 *w) {
    RET(find_first(w, ARG(0) ? w32_str(w, ARG(0)) : "", ARG(2), 0));
}
static void k_PathCchCombine(w32 *w) {
    /* (out, cchOut, dir, file) -- join two path pieces. It is in
     * api-ms-win-core-path, not kernel32, but it forwards here like every
     * other api-ms-win- name. */
    uint64_t out = ARG(0);
    uint32_t cap = (uint32_t)ARG(1);
    char dir[600] = "", file[600] = "";
    if (ARG(2)) w32_wtoa(w, ARG(2), dir, sizeof dir);
    if (ARG(3)) w32_wtoa(w, ARG(3), file, sizeof file);
    char joined[1216];
    if (!dir[0]) snprintf(joined, sizeof joined, "%s", file);
    else if (!file[0]) snprintf(joined, sizeof joined, "%s", dir);
    else if (file[0] == '\\' || (file[0] && file[1] == ':'))
        snprintf(joined, sizeof joined, "%s", file);       /* already absolute */
    else {
        size_t dl = strlen(dir);
        int sep = dl && (dir[dl - 1] == '\\' || dir[dl - 1] == '/');
        snprintf(joined, sizeof joined, "%s%s%s", dir, sep ? "" : "\\", file);
    }
    size_t n = strlen(joined);
    if (!out || cap <= n) { RET((uint64_t)(uint32_t)0x8007007Au); return; }  /* E_NOT_SUFFICIENT_BUFFER */
    for (size_t i = 0; i <= n; i++) w32_write(w, out + i * 2, 2, (uint8_t)joined[i]);
    RET(0);
}
/* Condition variables. One guest thread runs at a time (see thread.c), so a
 * sleep on a condition variable is a bounded wait: the lock is released, the
 * thread yields, and it comes back. Signalling is what wakes it early. */
static void k_SleepConditionVariableCS(w32 *w) { RET(w32_cond_sleep(w, ARG(0), ARG(1), (uint32_t)ARG(2), 0)); }
static void k_SleepConditionVariableSRW(w32 *w) { RET(w32_cond_sleep(w, ARG(0), ARG(1), (uint32_t)ARG(2), 1)); }

#define F(n, a)        { #n, a, 0, k_##n, 0 }
#define FN(n, a, impl) { #n, a, 0, impl, 0 }
const w32_api w32_kernel32[] = {
    F(ExitProcess, 1), F(TerminateProcess, 2), F(GetCurrentProcess, 0), F(GetCurrentProcessId, 0),
    F(GetCommandLineA, 0), F(GetCommandLineW, 0), F(GetLastError, 0), F(SetLastError, 1), F(GetStartupInfoA, 1), F(GetStartupInfoW, 1),
    F(GetEnvironmentStringsA, 0), F(GetEnvironmentStringsW, 0), FN(FreeEnvironmentStringsA, 1, k_FreeEnvironmentStrings), FN(FreeEnvironmentStringsW, 1, k_FreeEnvironmentStrings),
    FN(GetEnvironmentStrings, 0, k_GetEnvironmentStringsA), F(GetEnvironmentVariableA, 3), F(GetEnvironmentVariableW, 3),
    F(GetModuleHandleA, 1), F(GetModuleHandleW, 1), F(GetModuleHandleExW, 3), F(LoadLibraryA, 1), F(LoadLibraryW, 1), F(LoadLibraryExA, 3), F(LoadLibraryExW, 3),
    F(FreeLibrary, 1), F(GetProcAddress, 2), F(GetModuleFileNameA, 3), F(GetModuleFileNameW, 3),
    F(FindResourceA, 3), F(FindResourceW, 3), F(FindResourceExA, 4), F(FindResourceExW, 4),
    F(LoadResource, 2), F(LockResource, 1), F(FreeResource, 1), F(SizeofResource, 2),
    F(IsDebuggerPresent, 0), F(OutputDebugStringA, 1),
    F(GetSystemInfo, 1), F(GetNativeSystemInfo, 1), F(GetVersion, 0), F(GetVersionExA, 1), F(GetVersionExW, 1),
    F(GetTickCount, 0), F(GetTickCount64, 0), F(QueryPerformanceCounter, 1), F(QueryPerformanceFrequency, 1),
    F(GetSystemTimeAsFileTime, 1), F(GetSystemTimePreciseAsFileTime, 1), F(GetLocalTime, 1), F(GetSystemTime, 1), F(GetTimeZoneInformation, 1),
    F(GetACP, 0), F(GetOEMCP, 0), F(GetConsoleCP, 0), F(GetConsoleOutputCP, 0), F(IsValidCodePage, 1), F(GetCPInfo, 2),
    F(GetUserDefaultLocaleName, 2), FN(GetSystemDefaultLocaleName, 2, k_GetUserDefaultLocaleName), F(AreFileApisANSI, 0),
    F(GetUserDefaultLCID, 0), F(GetUserDefaultLangID, 0), F(GetSystemDefaultLCID, 0), F(GetThreadLocale, 0),
    F(IsDBCSLeadByteEx, 2), F(IsDBCSLeadByte, 1), F(MultiByteToWideChar, 6), F(WideCharToMultiByte, 8), F(GetStringTypeW, 4), F(LCMapStringW, 6), F(CompareStringW, 6),
    F(VirtualAlloc, 4), F(VirtualAllocEx, 5), F(VirtualFree, 3), F(VirtualProtect, 4), F(VirtualQuery, 3),
    F(GetProcessHeap, 0), F(HeapCreate, 3), F(HeapDestroy, 1), F(HeapAlloc, 3), F(HeapReAlloc, 4), F(HeapFree, 3), F(HeapSize, 3), F(HeapValidate, 3), F(HeapSetInformation, 4),
    F(LocalAlloc, 2), F(LocalFree, 1), F(GlobalAlloc, 2), F(GlobalFree, 1),
    F(TlsAlloc, 0), F(TlsFree, 1), F(TlsGetValue, 1), F(TlsSetValue, 2), F(FlsAlloc, 1), F(FlsFree, 1), F(FlsGetValue, 1), F(FlsSetValue, 2), F(FlsGetValue2, 1), F(DisableThreadLibraryCalls, 1),
    FN(InitializeSRWLock, 1, k_nop_void), FN(AcquireSRWLockExclusive, 1, k_nop_void), FN(ReleaseSRWLockExclusive, 1, k_nop_void),
    FN(AcquireSRWLockShared, 1, k_nop_void), FN(ReleaseSRWLockShared, 1, k_nop_void), FN(InitOnceExecuteOnce, 4, k_nop_true),
    FN(InitializeConditionVariable, 1, k_nop_void), FN(WakeAllConditionVariable, 1, k_nop_void), FN(WakeConditionVariable, 1, k_nop_void),
    F(GetStdHandle, 1), F(SetStdHandle, 2), F(WriteFile, 5), F(WriteConsoleA, 5), F(WriteConsoleW, 5), F(ReadFile, 5),
    F(CreateFileA, 7), F(CreateFileW, 7), F(CloseHandle, 1), F(GetFileType, 1), F(GetFileSize, 2), F(GetFileSizeEx, 2),
    F(FindFirstFileA, 2), F(FindFirstFileW, 2), F(FindNextFileA, 2), F(FindNextFileW, 2), F(FindClose, 1),
    F(CreateFileMappingA, 6), F(CreateFileMappingW, 6), F(MapViewOfFile, 5), F(MapViewOfFileEx, 6),
    F(UnmapViewOfFile, 1), F(FlushViewOfFile, 2),
    F(SetFilePointer, 4), F(SetFilePointerEx, 5), F(FlushFileBuffers, 1), F(GetConsoleMode, 2), F(SetConsoleMode, 2), F(GetConsoleScreenBufferInfo, 2), F(SetConsoleCtrlHandler, 2),
    F(GetFileAttributesA, 1), F(DeleteFileA, 1), F(GetCurrentDirectoryA, 2), F(SetCurrentDirectoryA, 1), F(GetTempPathA, 2), F(GetFullPathNameA, 4), F(FormatMessageA, 7),
    /* What an installer does, and what a game does on its second run: copy,
     * move, make and remove directories, set attributes, ask how much room is
     * left, and keep settings in an .ini. */
    F(GetFileAttributesW, 1), F(DeleteFileW, 1), F(GetCurrentDirectoryW, 2), F(SetCurrentDirectoryW, 1), F(GetTempPathW, 2),
    F(SetFileAttributesA, 2), F(SetFileAttributesW, 2),
    F(CopyFileA, 3), F(CopyFileW, 3), F(CopyFileExA, 6), F(CopyFileExW, 6),
    F(MoveFileA, 2), F(MoveFileW, 2), F(MoveFileExA, 3), F(MoveFileExW, 3),
    F(CreateDirectoryA, 2), F(CreateDirectoryW, 2), F(RemoveDirectoryA, 1), F(RemoveDirectoryW, 1),
    F(GetDiskFreeSpaceA, 5), F(GetDiskFreeSpaceW, 5), F(GetDiskFreeSpaceExA, 4), F(GetDiskFreeSpaceExW, 4),
    F(GetVolumeInformationA, 8), F(GetVolumeInformationW, 8),
    F(GetTempFileNameA, 4), F(GetTempFileNameW, 4), F(SetEndOfFile, 1),
    F(GetShortPathNameA, 3), F(GetShortPathNameW, 3), F(GetLongPathNameA, 3), F(GetLongPathNameW, 3),
    F(GetPrivateProfileStringA, 6), F(GetPrivateProfileStringW, 6), F(GetPrivateProfileIntA, 4),
    F(WritePrivateProfileStringA, 4),
    /* Implemented, and refuses: there is one guest process and the runtime's
     * globals are per process. It reports itself in the run report so a
     * program that needed a child is not a silent mystery. */
    F(CreateProcessA, 10), F(CreateProcessW, 10),
    /* Where DLLs are searched for. SetDefaultDllDirectories is what an NSIS
     * installer calls before anything else. */
    /* The wide half. NSIS is a Unicode program: every path it touches goes
     * through these, which is why a silent install stopped on
     * GetSystemDirectoryW with the A form sitting right beside it. */
    F(GetSystemDirectoryW, 2), F(GetWindowsDirectoryW, 2), F(GetSystemWow64DirectoryW, 2),
    F(GetFullPathNameW, 4), F(SearchPathW, 6),
    F(ExpandEnvironmentStringsW, 3), F(SetEnvironmentVariableW, 2),
    F(SetEnvironmentVariableA, 2),
    F(lstrcmpW, 2), F(lstrcmpiW, 2), F(lstrcpynW, 3), F(lstrcpynA, 3),
    F(CompareFileTime, 2), F(SetFileTime, 4), F(GetFileTime, 4), F(MulDiv, 3),
    /* What the C runtime walks before it reaches main. Every one of these
     * missing is a program that never runs a line of its own code. */
    F(GetLocaleInfoW, 4), F(GetLocaleInfoA, 4), F(GetLocaleInfoEx, 4),
    F(IsValidLocale, 2),
    F(EnumSystemLocalesW, 2), F(EnumSystemLocalesA, 2), F(EnumSystemLocalesEx, 4),
    F(LCMapStringEx, 9), F(LCMapStringW, 6),
    F(CompareStringW, 6), F(CompareStringEx, 9), F(CompareStringOrdinal, 5),
    /* dates and times */
    F(FileTimeToSystemTime, 2), F(SystemTimeToFileTime, 2),
    F(SystemTimeToTzSpecificLocalTime, 3), F(TzSpecificLocalTimeToSystemTime, 3),
    F(FileTimeToLocalFileTime, 2), F(LocalFileTimeToFileTime, 2),
    F(GetDateFormatW, 6), F(GetDateFormatA, 6), F(GetTimeFormatW, 6), F(GetTimeFormatA, 6),
    /* the version gate a game checks before it starts */
    F(VerSetConditionMask, 3), F(VerifyVersionInfoW, 4), F(VerifyVersionInfoA, 4),
    /* files, in the shapes a game asks about them */
    F(GetFileAttributesExW, 3), F(GetFileAttributesExA, 3),
    F(GetFileInformationByHandle, 2), F(GetFinalPathNameByHandleW, 4),
    F(FindFirstFileExW, 6), F(FindFirstFileExA, 6),
    F(GetDriveTypeW, 1), F(PathCchCombine, 4),
    F(FormatMessageW, 7),
    /* threads and waiting */
    F(SleepConditionVariableCS, 3), F(SleepConditionVariableSRW, 4),
    F(CreateWaitableTimerW, 3), F(CreateWaitableTimerA, 3), F(CreateWaitableTimerExW, 4),
    F(SetWaitableTimer, 6), F(CancelWaitableTimer, 1),
    F(CreateEventExA, 4), F(FreeLibraryAndExitThread, 2),
    /* the rest */
    F(DebugBreak, 0), F(GetConsoleWindow, 0), F(ReadConsoleW, 5), F(PeekNamedPipe, 6),
    F(SetProcessInformation, 4), F(HeapWalk, 2), F(K32GetProcessMemoryInfo, 3),
    F(GlobalLock, 1), F(GlobalUnlock, 1), F(GlobalSize, 1),
    F(WritePrivateProfileStringW, 4),
    F(SetDefaultDllDirectories, 1), F(SetDllDirectoryA, 1), F(SetDllDirectoryW, 1),
    F(AddDllDirectory, 1), F(RemoveDllDirectory, 1),
    F(lstrcatA, 2), F(lstrcatW, 2),
    F(GetThreadPriority, 1), F(SetThreadPriority, 2), F(GetExitCodeProcess, 2),
    F(GetProcessAffinityMask, 3), F(SetErrorMode, 1),
    /* RaiseException, RtlCaptureContext, RtlUnwind, the vectored handlers and
     * the unhandled filter are in win32/seh.c, which kernel32 pulls in as its
     * second table. What is left here is the 64-bit table-driven unwinder,
     * which is not implemented. */
    F(RtlPcToFileHeader, 2), F(RtlLookupFunctionEntry, 3), F(RtlVirtualUnwind, 8),
    F(EncodePointer, 1), F(DecodePointer, 1), F(InitializeSListHead, 1), F(SetHandleCount, 1), F(GetLogicalDrives, 0), F(GetDriveTypeA, 1),
    F(GetComputerNameA, 2), F(GetUserNameA, 2), F(lstrlenA, 1), F(lstrlenW, 1), F(lstrcpyA, 2), F(lstrcpyW, 2), F(lstrcmpiA, 2),
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
/* winmm.dll -- the timer a game reads every frame.
 *
 * timeGetTime is milliseconds since the process started, not since boot: a
 * game subtracts two readings, and starting from zero keeps the arithmetic
 * away from the 32-bit wrap for the length of any session. timeBeginPeriod
 * asks for a finer scheduler tick, which is not ours to give and not needed
 * when the clock is already a nanosecond one. */
static void m_timeGetTime(w32 *w) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    static uint64_t base;
    uint64_t now = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
    if (!base) base = now;
    RET((uint32_t)(now - base));
}
static void m_timeBeginPeriod(w32 *w) { (void)w; RET(0); }
static void m_timeEndPeriod(w32 *w) { (void)w; RET(0); }
static void m_timeGetDevCaps(w32 *w) {
    if (ARG(0)) { w32_write(w, ARG(0), 4, 1); w32_write(w, ARG(0) + 4, 4, 1000000); }
    RET(0);
}
/* Joysticks, through the interface that predates XInput. Nothing is
 * attached to this one -- a controller reaches the guest through XInput, and
 * a game that finds no legacy joystick falls back to that or to the
 * keyboard. JOYERR_UNPLUGGED is the documented way to say "that port is
 * empty", which is true of all of them. */
enum { JOYERR_UNPLUGGED_ = 167, JOYERR_PARMS_ = 165 };
static void m_joyGetNumDevs(w32 *w) { (void)w; RET(0); }
static void m_joyGetPos(w32 *w)     { (void)w; RET(JOYERR_UNPLUGGED_); }
static void m_joyGetPosEx(w32 *w)   { (void)w; RET(JOYERR_UNPLUGGED_); }
static void m_joyGetDevCapsA(w32 *w) { (void)w; RET(JOYERR_UNPLUGGED_); }
static void m_joyGetDevCapsW(w32 *w) { (void)w; RET(JOYERR_UNPLUGGED_); }
static void m_joySetCapture(w32 *w) { (void)w; RET(JOYERR_UNPLUGGED_); }
static void m_joyReleaseCapture(w32 *w) { (void)w; RET(JOYERR_UNPLUGGED_); }
/* PlaySound(sound, hmod, flags): a WAV from memory or a file, through the
 * mixer. One at a time, as on Windows -- a new one stops the last -- and
 * without SND_ASYNC the call waits for it to finish, bounded. A sound kept in
 * the program's resources (SND_RESOURCE) is not looked up yet, and says so. */
enum { SND_ASYNC_ = 1, SND_MEMORY_ = 4, SND_LOOP_ = 8, SND_PURGE_ = 0x40, SND_RESOURCE_ = 0x40004 };
static int g_playsound = -1;
static void play_sound(w32 *w, int wide) {
    uint64_t p = ARG(0); uint32_t fl = (uint32_t)ARG(2);
    if (g_playsound >= 0) { w32_audio_src_remove(g_playsound); g_playsound = -1; }
    if (!p || (fl & SND_PURGE_)) { RET(1); return; }
    if ((fl & SND_RESOURCE_) == SND_RESOURCE_) {
        w32_note_refused(w, "winmm!PlaySound (SND_RESOURCE: a sound from the program's own resources is not looked up yet)");
        RET(0); return;
    }
    uint8_t *data = 0; size_t n = 0;
    if (fl & SND_MEMORY_) {
        const uint8_t *h = W32PN(w, p, 12);
        if (!h || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { RET(0); return; }
        uint32_t len = ((uint32_t)h[4] | (uint32_t)h[5] << 8 | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 24) + 8;
        const uint8_t *all = len < (64u << 20) ? W32PN(w, p, len) : 0;
        if (!all) { RET(0); return; }
        data = malloc(len); if (!data) { RET(0); return; }
        memcpy(data, all, len); n = len;
    } else {
        char name[1024], host[4096];
        if (wide) w32_wtoa(w, p, name, sizeof name); else snprintf(name, sizeof name, "%s", w32_str(w, p));
        host_path(w, name, host, sizeof host);
        FILE *f = fopen(host, "rb");
        if (!f) { w32_set_last_error(w, ERROR_FILE_NOT_FOUND); RET(0); return; }
        fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
        if (len <= 0 || len > (64L << 20) || !(data = malloc((size_t)len))) { fclose(f); RET(0); return; }
        n = fread(data, 1, (size_t)len, f); fclose(f);
    }
    w32_audio_src s;
    if (!w32_audio_parse_wav(data, n, &s)) { free(data); RET(0); return; }
    s.owner = data; s.playing = 1; s.looping = (fl & SND_LOOP_) != 0;
    g_playsound = w32_audio_src_add(&s);
    if (g_playsound < 0) { free(data); RET(0); return; }
    w32_audio_open();
    if (!(fl & SND_ASYNC_) && !s.looping) {
        uint64_t ms = (uint64_t)s.size * 1000 / (s.bps ? s.bps : 1);
        if (ms > 30000) ms = 30000;
        for (uint64_t t = 0; t < ms && w32_audio_src_playing(g_playsound); t += 10) {
            struct timespec ts = { 0, 10 * 1000000L }; nanosleep(&ts, 0);
        }
    }
    RET(1);
}
static void m_PlaySoundW(w32 *w) { play_sound(w, 1); }
static void m_PlaySoundA(w32 *w) { play_sound(w, 0); }
static void m_mciSendStringW(w32 *w) { (void)w; RET(1); }
static void m_mciSendStringA(w32 *w) { (void)w; RET(1); }

const w32_api w32_winmm[] = {
    { "timeGetTime", 0, 0, m_timeGetTime, 0 },
    { "joyGetNumDevs", 0, 0, m_joyGetNumDevs, 0 },
    { "joyGetPos", 2, 0, m_joyGetPos, 0 },
    { "joyGetPosEx", 2, 0, m_joyGetPosEx, 0 },
    { "joyGetDevCapsA", 3, 0, m_joyGetDevCapsA, 0 },
    { "joyGetDevCapsW", 3, 0, m_joyGetDevCapsW, 0 },
    { "joySetCapture", 4, 0, m_joySetCapture, 0 },
    { "joyReleaseCapture", 1, 0, m_joyReleaseCapture, 0 },
    { "PlaySoundW", 3, 0, m_PlaySoundW, 0 },
    { "PlaySoundA", 3, 0, m_PlaySoundA, 0 },
    { "mciSendStringW", 4, 0, m_mciSendStringW, 0 },
    { "mciSendStringA", 4, 0, m_mciSendStringA, 0 },
    { "timeBeginPeriod", 1, 0, m_timeBeginPeriod, 0 },
    { "timeEndPeriod", 1, 0, m_timeEndPeriod, 0 },
    { "timeGetDevCaps", 2, 0, m_timeGetDevCaps, 0 },
    { 0, 0, 0, 0, 0 }
};
