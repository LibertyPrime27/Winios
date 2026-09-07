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

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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
    case CSIDL_DESKTOPDIRECTORY:     return "C:\\Users\\winios\\Desktop";
    case CSIDL_PERSONAL:             return "C:\\Users\\winios\\Documents";
    case CSIDL_MYMUSIC:              return "C:\\Users\\winios\\Music";
    case CSIDL_MYVIDEO:              return "C:\\Users\\winios\\Videos";
    case CSIDL_MYPICTURES:           return "C:\\Users\\winios\\Pictures";
    case CSIDL_APPDATA:              return "C:\\Users\\winios\\AppData\\Roaming";
    case CSIDL_LOCAL_APPDATA:        return "C:\\Users\\winios\\AppData\\Local";
    case CSIDL_PROFILE:              return "C:\\Users\\winios";
    case CSIDL_STARTMENU:            return "C:\\Users\\winios\\Start Menu";
    case CSIDL_PROGRAMS:             return "C:\\Users\\winios\\Start Menu\\Programs";
    case CSIDL_STARTUP:              return "C:\\Users\\winios\\Start Menu\\Programs\\Startup";
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
        uint16_t *d = W32P(w, buf);
        if (d) { for (size_t i = 0; i < l; i++) d[i] = (uint8_t)s[i]; d[l] = 0; }
    } else {
        memcpy(W32P(w, buf), s, l);
        w32_write(w, buf + l, 1, 0);
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

/* SHFileOperation does bulk copy/move/delete from a double-null-terminated
 * list of paths. Implementing it properly means implementing the whole list
 * format and the recursion; installers that use it are the minority and they
 * use it for the same things CopyFile does. It reports itself rather than
 * claiming a copy happened, because a caller told "done" would then look for
 * files that are not there. */
static void s_SHFileOperationA(w32 *w) {
    w32_note_refused(w, "shell32!SHFileOperation (bulk copy/move/delete)");
    RET(0x75);                                      /* DE_OPCANCELLED */
}

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
    F(ShellExecuteA, 6), F(ShellExecuteW, 6), F(ShellExecuteExA, 1),
    F(SHFileOperationA, 1),
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
