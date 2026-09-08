/* shell32.dll -- where a program is allowed to put things.
 *
 * An installer's first question, before it copies anything, is where the
 * standard directories are: Program Files for the program, AppData for
 * settings, the Start Menu for a shortcut. It asks shell32, and until now
 * shell32 was not here at all, so a silent install stopped before it had
 * created a single file.
 *
 * A game asks the same questions later and for a better reason: saves. A
 * modern Windows game does not write into its own folder -- it writes into
 * `Documents\My Games\<name>` or `AppData\Roaming\<name>`, because its own
 * folder may not be writable. Both come from here.
 *
 * The folders are the ones w32_drive_init() makes, so an answer is a real
 * directory rather than a plausible string. CSIDL_FLAG_CREATE is honoured by
 * making it, which is exactly what the flag asks for and what a caller that
 * passes it is relying on.
 */
#define _GNU_SOURCE
#include "w32.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The CSIDL constants, as documented. Only the ones a program plausibly asks
 * for on the way to installing or saving; anything else falls through to
 * failure, which is better than a wrong directory. */
enum {
    CSIDL_DESKTOP            = 0x00,
    CSIDL_PROGRAMS           = 0x02,   /* Start Menu\Programs             */
    CSIDL_PERSONAL           = 0x05,   /* Documents                       */
    CSIDL_STARTUP            = 0x07,
    CSIDL_STARTMENU          = 0x0B,
    CSIDL_MYMUSIC            = 0x0D,
    CSIDL_MYVIDEO            = 0x0E,
    CSIDL_DESKTOPDIRECTORY   = 0x10,
    CSIDL_APPDATA            = 0x1A,   /* AppData\Roaming                 */
    CSIDL_LOCAL_APPDATA      = 0x1C,   /* AppData\Local                   */
    CSIDL_COMMON_APPDATA     = 0x23,   /* ProgramData                     */
    CSIDL_WINDOWS            = 0x24,
    CSIDL_SYSTEM             = 0x25,
    CSIDL_PROGRAM_FILES      = 0x26,
    CSIDL_MYPICTURES         = 0x27,
    CSIDL_PROFILE            = 0x28,
    CSIDL_SYSTEMX86          = 0x29,
    CSIDL_PROGRAM_FILESX86   = 0x2A,
    CSIDL_PROGRAM_FILES_COMMON = 0x2B,
    CSIDL_COMMON_DOCUMENTS   = 0x2E,
    CSIDL_FLAG_CREATE        = 0x8000,
    CSIDL_FLAG_MASK          = 0x00FF,
};
enum { S_OK_ = 0, E_FAIL_ = (int)0x80004005, E_INVALIDARG_ = (int)0x80070057 };

/* One user, called `winios`. A single-user device does not need more, and a
 * fixed name means a save written today is found tomorrow -- a profile path
 * built from something variable would silently orphan saves. */
static const char *csidl_path(uint32_t id) {
    switch (id & CSIDL_FLAG_MASK) {
    case CSIDL_DESKTOP:
    case CSIDL_DESKTOPDIRECTORY:     return "C:\\Users\\Winios\\Desktop";
    case CSIDL_PERSONAL:             return "C:\\Users\\Winios\\Documents";
    case CSIDL_MYMUSIC:              return "C:\\Users\\Winios\\Music";
    case CSIDL_MYVIDEO:              return "C:\\Users\\Winios\\Videos";
    case CSIDL_MYPICTURES:           return "C:\\Users\\Winios\\Pictures";
    case CSIDL_APPDATA:              return "C:\\Users\\Winios\\AppData\\Roaming";
    case CSIDL_LOCAL_APPDATA:        return "C:\\Users\\Winios\\AppData\\Local";
    case CSIDL_PROFILE:              return "C:\\Users\\Winios";
    case CSIDL_STARTMENU:            return "C:\\Users\\Winios\\Start Menu";
    case CSIDL_PROGRAMS:             return "C:\\Users\\Winios\\Start Menu\\Programs";
    case CSIDL_STARTUP:              return "C:\\Users\\Winios\\Start Menu\\Programs\\Startup";
    case CSIDL_COMMON_APPDATA:       return "C:\\ProgramData";
    case CSIDL_COMMON_DOCUMENTS:     return "C:\\Users\\Public\\Documents";
    case CSIDL_WINDOWS:              return "C:\\Windows";
    case CSIDL_SYSTEM:               return "C:\\Windows\\System32";
    case CSIDL_SYSTEMX86:            return "C:\\Windows\\SysWOW64";
    case CSIDL_PROGRAM_FILES:        return "C:\\Program Files";
    case CSIDL_PROGRAM_FILESX86:     return "C:\\Program Files (x86)";
    case CSIDL_PROGRAM_FILES_COMMON: return "C:\\Program Files\\Common Files";
    default:                         return 0;
    }
}

/* Make a Windows path, one component at a time, on the host side. Used both
 * for CSIDL_FLAG_CREATE and for SHCreateDirectoryEx, which is the API an
 * installer calls to make its own install directory. */
static int make_win_dirs(w32 *w, const char *win) {
    char part[1024];
    size_t n = strlen(win);
    if (n >= sizeof part) return 0;
    memcpy(part, win, n + 1);
    /* Start after the drive letter: "C:" is not a directory to create. */
    size_t i = (part[1] == ':') ? 3 : 0;
    for (; i <= n; i++) {
        if (part[i] && part[i] != '\\' && part[i] != '/') continue;
        char save = part[i];
        part[i] = 0;
        if (part[0]) {
            char host[4096];
            w32_host_path(w, part, host, sizeof host);
            struct stat st;
            if (stat(host, &st)) { if (mkdir(host, 0777) && stat(host, &st)) { part[i] = save; return 0; } }
        }
        part[i] = save;
        if (!save) break;
    }
    return 1;
}

static void put_path(w32 *w, uint64_t buf, int wide, const char *s) {
    if (!buf) return;
    size_t l = strlen(s);
    if (wide) {
        uint16_t *d = W32PN(w, buf, 2 * (l + 1));
        if (d) { for (size_t i = 0; i < l; i++) d[i] = (uint8_t)s[i]; d[l] = 0; }
    } else {
        char *d = W32PN(w, buf, l + 1);
        if (d) { memcpy(d, s, l); d[l] = 0; }
    }
}

/* SHGetFolderPath(hwnd, csidl, hToken, dwFlags, pszPath) -> HRESULT */
static void get_folder_path(w32 *w, int wide) {
    uint32_t id = (uint32_t)ARG(1);
    const char *p = csidl_path(id);
    if (!p) { RET((uint64_t)(uint32_t)E_FAIL_); return; }
    if (id & CSIDL_FLAG_CREATE) make_win_dirs(w, p);
    put_path(w, ARG(4), wide, p);
    RET(S_OK_);
}
static void s_SHGetFolderPathA(w32 *w) { get_folder_path(w, 0); }
static void s_SHGetFolderPathW(w32 *w) { get_folder_path(w, 1); }

/* SHGetSpecialFolderPath(hwnd, pszPath, csidl, fCreate) -> BOOL.
 * Same answer, different argument order and a boolean return -- and the
 * `fCreate` flag is a separate argument here rather than a bit in the id. */
static void get_special_folder(w32 *w, int wide) {
    uint32_t id = (uint32_t)ARG(2);
    const char *p = csidl_path(id);
    if (!p) { RET(0); return; }
    if (ARG(3) || (id & CSIDL_FLAG_CREATE)) make_win_dirs(w, p);
    put_path(w, ARG(1), wide, p);
    RET(1);
}
static void s_SHGetSpecialFolderPathA(w32 *w) { get_special_folder(w, 0); }
static void s_SHGetSpecialFolderPathW(w32 *w) { get_special_folder(w, 1); }

/* SHCreateDirectoryEx(hwnd, pszPath, psa) -> int (a Win32 error code, not an
 * HRESULT: 0 for success, ERROR_ALREADY_EXISTS for a path that was there). */
static void create_dir_ex(w32 *w, int wide) {
    char win[1024];
    if (wide) w32_wtoa(w, ARG(1), win, sizeof win);
    else snprintf(win, sizeof win, "%s", ARG(1) ? w32_str(w, ARG(1)) : "");
    if (!win[0]) { RET(87); return; }               /* ERROR_INVALID_PARAMETER */
    char host[4096];
    w32_host_path(w, win, host, sizeof host);
    struct stat st;
    int existed = stat(host, &st) == 0;
    if (!make_win_dirs(w, win)) { RET(5); return; } /* ERROR_ACCESS_DENIED */
    RET(existed ? 183u : 0u);                       /* ERROR_ALREADY_EXISTS */
}
static void s_SHCreateDirectoryExA(w32 *w) { create_dir_ex(w, 0); }
static void s_SHCreateDirectoryExW(w32 *w) { create_dir_ex(w, 1); }

/* ShellExecute is how an installer opens a readme, a URL, or the game it just
 * installed. There is no second process and no browser, so it fails -- and
 * says which target it wanted, in the run report, because "it tried to launch
 * something" is a fact worth having. Returning success would leave the caller
 * waiting for a window that will never appear. */
static void shell_execute(w32 *w, int wide) {
    char what[512] = "";
    if (ARG(2)) { if (wide) w32_wtoa(w, ARG(2), what, sizeof what);
                  else snprintf(what, sizeof what, "%s", w32_str(w, ARG(2))); }
    w32_note_refused(w, "shell32!ShellExecute (nothing here can launch a second program)");
    if (w->verbose && what[0]) fprintf(stderr, "winrun: ShellExecute refused: %s\n", what);
    w32_set_last_error(w, 120);                     /* ERROR_CALL_NOT_IMPLEMENTED */
    RET(31);                                        /* SE_ERR_NOASSOC */
}
static void s_ShellExecuteA(w32 *w) { shell_execute(w, 0); }
static void s_ShellExecuteW(w32 *w) { shell_execute(w, 1); }
static void s_ShellExecuteExA(w32 *w) {
    w32_note_refused(w, "shell32!ShellExecute (nothing here can launch a second program)");
    w32_set_last_error(w, 120); RET(0);
}

/* ---- SHFileOperation ------------------------------------------------------
 *
 * Bulk copy, move and delete, over a list of paths that is double-null
 * terminated -- "a\0b\0c\0\0" -- and may name directories, which are handled
 * recursively. This is what an uninstaller uses to remove what it installed,
 * and what a few installers use instead of CopyFile, so a refusal here means
 * an uninstall that reports success and removes nothing.
 *
 * The structure has to be read by offset because it is not the same shape in
 * both bitnesses: a pointer, then a UINT, then two pointers, then a WORD --
 * which pads differently at 4 and 8 byte alignment.
 */
enum { FO_MOVE = 1, FO_COPY = 2, FO_DELETE = 3, FO_RENAME = 4 };
enum { DE_OPCANCELLED = 0x75, DE_ERROR_MAX = 0xB7 };

/* One path out of the list at `p`, advancing past its terminator. Returns 0
 * at the end of the list -- an empty entry, which is the second null. */
static int zz_next(w32 *w, uint64_t *p, int wide, char *out, size_t n) {
    if (!*p) return 0;
    if (wide) {
        size_t i = 0;
        for (;; i++) {
            uint16_t c = (uint16_t)w32_read(w, *p + i * 2, 2);
            if (!c) break;
            if (i + 1 < n) out[i] = c < 128 ? (char)c : '?';
        }
        out[i < n ? i : n - 1] = 0;
        *p += (i + 1) * 2;
        return i > 0;
    }
    const char *s = w32_str(w, *p);
    snprintf(out, n, "%.*s", (int)n - 1, s);
    size_t len = strlen(s);
    *p += len + 1;
    return len > 0;
}

/* Recursive copy and remove, on host paths. Depth is bounded because a
 * directory tree that contains itself through a symlink would otherwise not
 * terminate, and an installer's payload is never deep. */
static int host_copy_tree(const char *from, const char *to, int depth);
static int host_remove_tree(const char *path, int depth);

static int copy_one_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return 0;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return 0; }
    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    if (ferror(in)) ok = 0;
    fclose(in);
    if (fclose(out)) ok = 0;
    if (!ok) remove(to);
    return ok;
}

static int host_copy_tree(const char *from, const char *to, int depth) {
    if (depth > 32) return 0;
    struct stat st;
    if (lstat(from, &st)) return 0;
    if (!S_ISDIR(st.st_mode)) return copy_one_file(from, to);
    if (mkdir(to, 0777) && errno != EEXIST) return 0;
    DIR *d = opendir(from);
    if (!d) return 0;
    int ok = 1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[2048], b[2048];
        snprintf(a, sizeof a, "%s/%s", from, e->d_name);
        snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        if (!host_copy_tree(a, b, depth + 1)) ok = 0;
    }
    closedir(d);
    return ok;
}

static int host_remove_tree(const char *path, int depth) {
    if (depth > 32) return 0;
    struct stat st;
    if (lstat(path, &st)) return 0;
    if (!S_ISDIR(st.st_mode)) return remove(path) == 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    int ok = 1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[2048];
        snprintf(a, sizeof a, "%s/%s", path, e->d_name);
        if (!host_remove_tree(a, depth + 1)) ok = 0;
    }
    closedir(d);
    if (rmdir(path)) ok = 0;
    return ok;
}

/* Where one source lands. With several sources the destination is a
 * directory and each keeps its own name; with one it may be either, and a
 * destination that already exists as a directory takes the name too. */
static void dest_for(const char *to_dir, const char *from, int many, char *out, size_t n) {
    struct stat st;
    int to_is_dir = !stat(to_dir, &st) && S_ISDIR(st.st_mode);
    if (!many && !to_is_dir) { snprintf(out, n, "%s", to_dir); return; }
    const char *base = strrchr(from, '/');
    base = base ? base + 1 : from;
    snprintf(out, n, "%s/%s", to_dir, base);
}

static void file_operation(w32 *w, int wide) {
    uint64_t op = ARG(0);
    if (!op) { RET(DE_OPCANCELLED); return; }
    int ps = (int)w32_ptrsize(w);
    /* hwnd, wFunc, pFrom, pTo, fFlags -- laid out for the bitness. */
    uint64_t o_func = (uint64_t)ps;
    uint64_t o_from = w->is32 ? 8 : 16;
    uint64_t o_to   = w->is32 ? 12 : 24;
    uint64_t o_abort = w->is32 ? 20 : 36;
    uint32_t func = (uint32_t)w32_read(w, op + o_func, 4);
    uint64_t from = w32_read(w, op + o_from, ps);
    uint64_t to   = w32_read(w, op + o_to, ps);

    char to_host[2048] = "";
    if (to) {
        uint64_t tp = to;
        char win[1024];
        if (zz_next(w, &tp, wide, win, sizeof win)) w32_host_path(w, win, to_host, sizeof to_host);
    }
    /* More than one source means the destination has to be a directory. */
    int count = 0;
    { uint64_t fp = from; char win[1024]; while (fp && zz_next(w, &fp, wide, win, sizeof win)) count++; }

    int failed = 0, did = 0;
    uint64_t fp = from;
    char win[1024];
    while (fp && zz_next(w, &fp, wide, win, sizeof win)) {
        char src[2048];
        w32_host_path(w, win, src, sizeof src);
        /* A wildcard would need a directory walk of its own; nothing here
         * expands one, and doing half of it silently would delete the wrong
         * things. */
        if (strchr(src, '*') || strchr(src, '?')) { failed = 1; continue; }
        did++;
        if (func == FO_DELETE) {
            if (!host_remove_tree(src, 0)) failed = 1;
            continue;
        }
        if (!to_host[0]) { failed = 1; continue; }
        char dst[2400];
        if (func == FO_RENAME) snprintf(dst, sizeof dst, "%s", to_host);
        else dest_for(to_host, src, count > 1, dst, sizeof dst);
        if (func == FO_MOVE || func == FO_RENAME) {
            if (rename(src, dst) == 0) continue;
            if (errno != EXDEV) { failed = 1; continue; }
            /* Across devices rename cannot work, so it becomes copy then
             * remove -- which is what the shell does too. */
            if (!host_copy_tree(src, dst, 0)) { failed = 1; continue; }
            if (!host_remove_tree(src, 0)) failed = 1;
        } else if (func == FO_COPY) {
            if (!host_copy_tree(src, dst, 0)) failed = 1;
        } else failed = 1;
    }
    if (w32_read(w, op + o_abort, 4) || 1) w32_write(w, op + o_abort, 4, failed ? 1 : 0);
    if (!did) { RET(DE_OPCANCELLED); return; }
    RET(failed ? 0x71 : 0);                          /* DE_MANYSRC1DEST on failure */
}
static void s_SHFileOperationA(w32 *w) { file_operation(w, 0); }
static void s_SHFileOperationW(w32 *w) { file_operation(w, 1); }

/* SHBrowseForFolder puts up a folder picker. There is nowhere to put one and
 * nobody to answer it, so it is cancelled -- which every caller handles,
 * because a person pressing Cancel is the normal case. */
static void s_SHBrowseForFolderA(w32 *w) {
    w32_note_refused(w, "shell32!SHBrowseForFolder (no folder picker; treated as cancelled)");
    RET(0);
}
static void s_SHBrowseForFolderW(w32 *w) { s_SHBrowseForFolderA(w); }

/* SHGetFileInfo is asked for an icon or a display name. Neither exists here.
 * Zero is a documented failure and callers check it. */
static void s_SHGetFileInfoA(w32 *w) { (void)w; RET(0); }
static void s_SHGetFileInfoW(w32 *w) { (void)w; RET(0); }

/* ---- the older way of asking, which is the way installers ask -------------
 *
 * SHGetFolderPath is the convenient call. The older pair is
 * SHGetSpecialFolderLocation, which hands back an *item ID list*, and
 * SHGetPathFromIDList, which turns one into a path -- and an NSIS installer
 * resolves $SMPROGRAMS, $DESKTOP and $APPDATA through that pair. Both were
 * here as stubs that returned failure, which is worse than being absent: the
 * import resolves, so nothing reports it, and the installer just cannot find
 * anywhere to put a shortcut.
 *
 * A PIDL is opaque to whoever receives it -- the only defined thing to do
 * with one is pass it back to the shell -- so ours is simply a marker and the
 * folder id. That is a complete implementation of the contract rather than a
 * pretence at one: every path the caller can take through it works.
 */
enum { PIDL_MAGIC = 0x50494432u };   /* "PID2" */

static void s_SHGetSpecialFolderLocation2(w32 *w) {
    uint32_t id = (uint32_t)ARG(1);
    if (!ARG(2)) { RET((uint64_t)(uint32_t)E_INVALIDARG_); return; }
    if (!csidl_path(id)) { w32_write(w, ARG(2), w->is32 ? 4 : 8, 0);
                           RET((uint64_t)(uint32_t)E_FAIL_); return; }
    /* 12 bytes: the marker, the folder id, and the two zero bytes that
     * terminate an item ID list. */
    uint64_t p = w32_heap_alloc(w, 12);
    if (!p) { RET((uint64_t)(uint32_t)E_FAIL_); return; }
    w32_write(w, p + 0, 4, PIDL_MAGIC);
    w32_write(w, p + 4, 4, id);
    w32_write(w, p + 8, 4, 0);
    w32_write(w, ARG(2), w->is32 ? 4 : 8, p);
    RET(S_OK_);
}

static void path_from_idlist(w32 *w, int wide) {
    uint64_t pidl = ARG(0);
    if (!pidl || !ARG(1)) { RET(0); return; }
    uint32_t magic = (uint32_t)w32_read(w, pidl, 4);
    if (magic != PIDL_MAGIC) { RET(0); return; }
    const char *path = csidl_path((uint32_t)w32_read(w, pidl + 4, 4));
    if (!path) { RET(0); return; }
    /* The caller is about to write a shortcut into it, so it has to exist. */
    make_win_dirs(w, path);
    put_path(w, ARG(1), wide, path);
    RET(1);
}
static void s_SHGetPathFromIDListA2(w32 *w) { path_from_idlist(w, 0); }
static void s_SHGetPathFromIDListW(w32 *w)  { path_from_idlist(w, 1); }
/* Freeing one. An item ID list is documented as the caller's to free with
 * CoTaskMemFree or ILFree; both end up here. */
static void s_ILFree(w32 *w) { if (ARG(0)) w32_heap_free(w, ARG(0)); RET(0); }
/* Telling the shell something changed, when there is no shell. Doing nothing
 * is the correct and complete implementation. */
static void s_SHChangeNotify(w32 *w) { (void)w; RET(0); }
/* An installer asks whether it is running elevated so it can decide between
 * Program Files and the user's own folder. There are no privileges here and
 * every path is writable, so the honest answer is yes. */
static void s_IsUserAnAdmin(w32 *w) { (void)w; RET(1); }
static void s_SHGetKnownFolderPath(w32 *w) { (void)w; RET((uint64_t)(uint32_t)E_FAIL_); }

#define F(n, a)        { #n, a, 0, s_##n, 0 }
#define FN(n, a, impl) { #n, a, 0, impl, 0 }
const w32_api w32_shell32[] = {
    F(SHGetFolderPathA, 5), F(SHGetFolderPathW, 5),
    F(SHGetSpecialFolderPathA, 4), F(SHGetSpecialFolderPathW, 4),
    F(SHCreateDirectoryExA, 3), F(SHCreateDirectoryExW, 3),
    F(ShellExecuteA, 6), F(ShellExecuteW, 6),
    F(ShellExecuteExA, 1), FN(ShellExecuteExW, 1, s_ShellExecuteExA),
    F(SHFileOperationA, 1), F(SHFileOperationW, 1),
    F(SHBrowseForFolderA, 1), F(SHBrowseForFolderW, 1),
    F(SHGetFileInfoA, 5), F(SHGetFileInfoW, 5),
    F(SHChangeNotify, 4), F(IsUserAnAdmin, 0),
    /* The older folder pair, which is the one installers use. */
    FN(SHGetSpecialFolderLocation, 3, s_SHGetSpecialFolderLocation2),
    FN(SHGetPathFromIDList, 2, s_SHGetPathFromIDListA2),
    FN(SHGetPathFromIDListA, 2, s_SHGetPathFromIDListA2),
    F(SHGetPathFromIDListW, 2), F(ILFree, 1),
    F(SHGetKnownFolderPath, 4),
    { 0, 0, 0, 0, 0 },
};
#undef F
#undef FN
