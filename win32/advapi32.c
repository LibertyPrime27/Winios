/* advapi32.dll -- the registry.
 *
 * An installer writes it and the program it installed reads it back, which is
 * the whole reason it has to persist: a game asks HKEY_LOCAL_MACHINE where it
 * was installed and takes the answer as gospel. So this is a real store, not a
 * set of stubs that return success -- values written in one run are there in
 * the next.
 *
 * It is deliberately flat. A registry is a tree, but every operation a program
 * performs names a key by its full path, so a sorted list of
 * (key, name) -> (type, bytes) answers all of them, and "does this key exist"
 * becomes "is any entry's key this path or below it". No node objects, no
 * parent pointers, nothing to keep consistent.
 *
 * On disk it is one line per value: path|name|type|hex. Text, because being
 * able to read and fix the thing by hand is worth more than a few bytes, and
 * because a corrupt binary blob would take a game's install path with it.
 */
#include "w32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum {
    ERROR_SUCCESS_ = 0, ERROR_FILE_NOT_FOUND_ = 2, ERROR_MORE_DATA_ = 234,
    ERROR_NO_MORE_ITEMS_ = 259, ERROR_INVALID_HANDLE_ = 6,
};
enum { REG_NONE_ = 0, REG_SZ_ = 1, REG_EXPAND_SZ_ = 2, REG_BINARY_ = 3, REG_DWORD_ = 4 };

/* The predefined roots. A guest passes these as HKEYs without opening them. */
static const struct { uint32_t h; const char *name; } g_roots[] = {
    { 0x80000000u, "HKCR" }, { 0x80000001u, "HKCU" }, { 0x80000002u, "HKLM" },
    { 0x80000003u, "HKU"  }, { 0x80000005u, "HKCC" },
};

typedef struct { char *key, *name; uint32_t type; uint8_t *data; uint32_t len; } reg_val;
enum { MAX_VALS = 4096, MAX_OPEN = 128 };
static reg_val g_vals[MAX_VALS];
static int g_nvals;
static int g_dirty, g_loaded;

/* An open key is a path and nothing else -- there is no node to point at.
 * KEY_HALF bounds each half of a "parent\\child" join so the result always
 * fits in the slot below. */
enum { KEY_SLOT = 512, KEY_HALF = (KEY_SLOT - 2) / 2 };
static struct { int used; char path[KEY_SLOT]; } g_open[MAX_OPEN];

static void reg_path(char *out, size_t n) {
    const char *c = w32_drive_c();
    if (c && *c) snprintf(out, n, "%s/registry.txt", c);
    else snprintf(out, n, "registry.txt");
}

static int hexval(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void reg_load(void) {
    if (g_loaded) return;
    g_loaded = 1;
    char path[1024]; reg_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[8192];
    while (fgets(line, sizeof line, f) && g_nvals < MAX_VALS) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *p1 = strchr(line, '|'); if (!p1) continue;
        char *p2 = strchr(p1 + 1, '|'); if (!p2) continue;
        char *p3 = strchr(p2 + 1, '|'); if (!p3) continue;
        *p1 = *p2 = *p3 = 0;
        const char *hex = p3 + 1;
        size_t hl = strlen(hex);
        reg_val *v = &g_vals[g_nvals++];
        v->key = strdup(line);
        v->name = strdup(p1 + 1);
        v->type = (uint32_t)strtoul(p2 + 1, 0, 10);
        v->len = (uint32_t)(hl / 2);
        v->data = v->len ? malloc(v->len) : 0;
        for (uint32_t i = 0; i < v->len; i++) {
            int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
            v->data[i] = (uint8_t)((hi < 0 ? 0 : hi) << 4 | (lo < 0 ? 0 : lo));
        }
    }
    fclose(f);
}

void w32_registry_flush(void) {
    if (!g_dirty) return;
    char path[1024]; reg_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < g_nvals; i++) {
        fprintf(f, "%s|%s|%u|", g_vals[i].key, g_vals[i].name, g_vals[i].type);
        for (uint32_t k = 0; k < g_vals[i].len; k++) fprintf(f, "%02x", g_vals[i].data[k]);
        fputc('\n', f);
    }
    fclose(f);
    g_dirty = 0;
}

void w32_registry_reset(void) {
    for (int i = 0; i < g_nvals; i++) { free(g_vals[i].key); free(g_vals[i].name); free(g_vals[i].data); }
    g_nvals = 0; g_dirty = 0; g_loaded = 0;
    memset(g_open, 0, sizeof g_open);
}

/* --- paths --- */

/* The full path an HKEY plus a subkey names. Handles are 0x40000000 + index so
 * they cannot be mistaken for a predefined root or for NULL. */
enum { HK_BASE = 0x40000000u };
static int key_path(w32 *w, uint64_t hkey, const char *sub, char *out, size_t n) {
    uint32_t h = (uint32_t)hkey;
    const char *base = 0;
    for (size_t i = 0; i < sizeof g_roots / sizeof g_roots[0]; i++)
        if (g_roots[i].h == h) base = g_roots[i].name;
    if (!base) {
        uint32_t idx = h - HK_BASE;
        if (h < HK_BASE || idx >= MAX_OPEN || !g_open[idx].used) return 0;
        base = g_open[idx].path;
    }
    /* Bounded on purpose: an open key holds its path in a fixed slot, so the
     * longest path this can build has to fit there. Registry paths are short;
     * anything near this is a program doing something odd, and truncating is
     * better than a silent overflow. */
    if (sub && *sub) snprintf(out, n, "%.*s\\%.*s", KEY_HALF, base, KEY_HALF, sub);
    else snprintf(out, n, "%.*s", 2 * KEY_HALF, base);
    /* normalise: no trailing slash, forward slashes are the same thing */
    for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
    size_t l = strlen(out);
    while (l && out[l - 1] == '\\') out[--l] = 0;
    (void)w;
    return 1;
}

static reg_val *find_val(const char *key, const char *name) {
    for (int i = 0; i < g_nvals; i++)
        if (!strcasecmp(g_vals[i].key, key) && !strcasecmp(g_vals[i].name, name)) return &g_vals[i];
    return 0;
}
/* A key exists if anything is stored at it or under it. */
static int key_exists(const char *key) {
    size_t l = strlen(key);
    for (int i = 0; i < g_nvals; i++) {
        if (!strncasecmp(g_vals[i].key, key, l) &&
            (g_vals[i].key[l] == 0 || g_vals[i].key[l] == '\\')) return 1;
    }
    return 0;
}
static uint32_t open_key(const char *path) {
    for (int i = 0; i < MAX_OPEN; i++) {
        if (g_open[i].used) continue;
        g_open[i].used = 1;
        snprintf(g_open[i].path, sizeof g_open[i].path, "%.*s", KEY_SLOT - 1, path);
        return HK_BASE + (uint32_t)i;
    }
    return 0;
}
static void set_val(const char *key, const char *name, uint32_t type, const uint8_t *data, uint32_t len) {
    reg_val *v = find_val(key, name);
    if (!v) {
        if (g_nvals >= MAX_VALS) return;
        v = &g_vals[g_nvals++];
        v->key = strdup(key);
        v->name = strdup(name);
        v->data = 0;
    }
    free(v->data);
    v->type = type;
    v->len = len;
    v->data = len ? malloc(len) : 0;
    if (len) memcpy(v->data, data, len);
    g_dirty = 1;
}

/* --- the API --- */

static void wide_or_narrow(w32 *w, uint64_t p, int wide, char *out, size_t n) {
    if (!p) { out[0] = 0; return; }
    if (wide) w32_wtoa(w, p, out, n);
    else snprintf(out, n, "%.*s", (int)n - 1, w32_str(w, p));
}

/* RegOpenKeyEx(hKey, subKey, options, desired, phkResult) */
static void reg_open(w32 *w, int wide) {
    char sub[512], path[1024];
    wide_or_narrow(w, ARG(1), wide, sub, sizeof sub);
    reg_load();
    if (!key_path(w, ARG(0), sub, path, sizeof path)) { RET(ERROR_INVALID_HANDLE_); return; }
    if (!key_exists(path)) { RET(ERROR_FILE_NOT_FOUND_); return; }
    uint32_t h = open_key(path);
    if (!h) { RET(ERROR_INVALID_HANDLE_); return; }
    if (ARG(4)) w32_write(w, ARG(4), (int)w32_ptrsize(w), h);
    RET(ERROR_SUCCESS_);
}
static void a_RegOpenKeyExA(w32 *w) { reg_open(w, 0); }
static void a_RegOpenKeyExW(w32 *w) { reg_open(w, 1); }
/* RegOpenKey(hKey, subKey, phkResult) -- the old three-argument form */
static void a_RegOpenKeyA(w32 *w) {
    char sub[512], path[1024];
    wide_or_narrow(w, ARG(1), 0, sub, sizeof sub);
    reg_load();
    if (!key_path(w, ARG(0), sub, path, sizeof path)) { RET(ERROR_INVALID_HANDLE_); return; }
    if (!key_exists(path)) { RET(ERROR_FILE_NOT_FOUND_); return; }
    uint32_t h = open_key(path);
    if (ARG(2)) w32_write(w, ARG(2), (int)w32_ptrsize(w), h);
    RET(h ? ERROR_SUCCESS_ : ERROR_INVALID_HANDLE_);
}

/* RegCreateKeyEx(hKey, subKey, reserved, class, options, desired, sa, phkResult, disposition).
 * Creating a key with no values in it needs something to mark its existence,
 * so it gets one empty unnamed value -- the same trick a filesystem plays with
 * an empty directory. */
static void reg_create(w32 *w, int wide) {
    char sub[512], path[1024];
    wide_or_narrow(w, ARG(1), wide, sub, sizeof sub);
    reg_load();
    if (!key_path(w, ARG(0), sub, path, sizeof path)) { RET(ERROR_INVALID_HANDLE_); return; }
    int existed = key_exists(path);
    if (!existed) set_val(path, "", REG_NONE_, 0, 0);
    uint32_t h = open_key(path);
    if (!h) { RET(ERROR_INVALID_HANDLE_); return; }
    if (ARG(7)) w32_write(w, ARG(7), (int)w32_ptrsize(w), h);
    if (ARG(8)) w32_write(w, ARG(8), 4, existed ? 2 : 1);      /* OPENED / CREATED_NEW_KEY */
    RET(ERROR_SUCCESS_);
}
static void a_RegCreateKeyExA(w32 *w) { reg_create(w, 0); }
static void a_RegCreateKeyExW(w32 *w) { reg_create(w, 1); }

/* RegQueryValueEx(hKey, name, reserved, ptype, pdata, pcbData).
 * pcbData in and out, and a NULL pdata means "tell me how big it is" -- which
 * every caller does first, so getting the size-only path right matters as much
 * as the data path. */
static void reg_query(w32 *w, int wide) {
    char name[512], path[1024];
    wide_or_narrow(w, ARG(1), wide, name, sizeof name);
    reg_load();
    if (!key_path(w, ARG(0), "", path, sizeof path)) { RET(ERROR_INVALID_HANDLE_); return; }
    reg_val *v = find_val(path, name);
    if (!v || v->type == REG_NONE_) { RET(ERROR_FILE_NOT_FOUND_); return; }

    /* a wide query wants a wide string back */
    uint8_t wbuf[2048];
    const uint8_t *data = v->data;
    uint32_t len = v->len;
    if (wide && (v->type == REG_SZ_ || v->type == REG_EXPAND_SZ_)) {
        uint32_t k = 0;
        for (; k < v->len && k * 2 + 1 < sizeof wbuf; k++) { wbuf[2 * k] = v->data[k]; wbuf[2 * k + 1] = 0; }
        data = wbuf; len = k * 2;
    }
    if (ARG(3)) w32_write(w, ARG(3), 4, v->type);
    uint64_t pcb = ARG(5);
    uint32_t room = pcb ? (uint32_t)w32_read(w, pcb, 4) : 0;
    if (pcb) w32_write(w, pcb, 4, len);
    if (!ARG(4)) { RET(ERROR_SUCCESS_); return; }              /* size query */
    if (room < len) { RET(ERROR_MORE_DATA_); return; }
    if (len) memcpy(W32P(w, ARG(4)), data, len);
    RET(ERROR_SUCCESS_);
}
static void a_RegQueryValueExA(w32 *w) { reg_query(w, 0); }
static void a_RegQueryValueExW(w32 *w) { reg_query(w, 1); }

/* RegSetValueEx(hKey, name, reserved, type, data, cbData) */
static void reg_set(w32 *w, int wide) {
    char name[512], path[1024];
    wide_or_narrow(w, ARG(1), wide, name, sizeof name);
    reg_load();
    if (!key_path(w, ARG(0), "", path, sizeof path)) { RET(ERROR_INVALID_HANDLE_); return; }
    uint32_t type = (uint32_t)ARG(3), len = (uint32_t)ARG(5);
    const uint8_t *src = ARG(4) ? W32P(w, ARG(4)) : 0;
    /* a wide string is stored narrow, so a program that writes W and reads A
     * (or the reverse -- installers do both) sees the same value */
    uint8_t nbuf[2048];
    if (wide && (type == REG_SZ_ || type == REG_EXPAND_SZ_) && src) {
        uint32_t k = 0;
        for (; k * 2 + 1 < len && k + 1 < sizeof nbuf; k++) nbuf[k] = src[2 * k];
        nbuf[k] = 0;
        src = nbuf; len = k + 1;
    }
    set_val(path, name, type, src, src ? len : 0);
    w32_registry_flush();
    RET(ERROR_SUCCESS_);
}
static void a_RegSetValueExA(w32 *w) { reg_set(w, 0); }
static void a_RegSetValueExW(w32 *w) { reg_set(w, 1); }

static void a_RegCloseKey(w32 *w) {
    uint32_t h = (uint32_t)ARG(0);
    if (h >= HK_BASE && h - HK_BASE < MAX_OPEN) g_open[h - HK_BASE].used = 0;
    RET(ERROR_SUCCESS_);
}

/* RegEnumValue(hKey, index, name, pcchName, reserved, ptype, pdata, pcbData) */
static void a_RegEnumValueA(w32 *w) {
    char path[1024];
    reg_load();
    if (!key_path(w, ARG(0), "", path, sizeof path)) { RET(ERROR_INVALID_HANDLE_); return; }
    uint32_t want = (uint32_t)ARG(1), seen = 0;
    for (int i = 0; i < g_nvals; i++) {
        if (strcasecmp(g_vals[i].key, path) || g_vals[i].type == REG_NONE_) continue;
        if (seen++ != want) continue;
        uint64_t pn = ARG(3);
        uint32_t room = pn ? (uint32_t)w32_read(w, pn, 4) : 0;
        uint32_t nl = (uint32_t)strlen(g_vals[i].name);
        if (ARG(2) && room > nl) memcpy(W32P(w, ARG(2)), g_vals[i].name, nl + 1);
        if (pn) w32_write(w, pn, 4, nl);
        if (ARG(5)) w32_write(w, ARG(5), 4, g_vals[i].type);
        uint64_t pcb = ARG(7);
        uint32_t cap = pcb ? (uint32_t)w32_read(w, pcb, 4) : 0;
        if (pcb) w32_write(w, pcb, 4, g_vals[i].len);
        if (ARG(6) && cap >= g_vals[i].len && g_vals[i].len)
            memcpy(W32P(w, ARG(6)), g_vals[i].data, g_vals[i].len);
        RET(ERROR_SUCCESS_);
        return;
    }
    RET(ERROR_NO_MORE_ITEMS_);
}

/* RegEnumKeyEx(hKey, index, name, pcchName, reserved, class, pcchClass, ftime).
 * The immediate children of a path, deduplicated -- the flat store means they
 * have to be derived rather than looked up. */
static void a_RegEnumKeyExA(w32 *w) {
    char path[1024];
    reg_load();
    if (!key_path(w, ARG(0), "", path, sizeof path)) { RET(ERROR_INVALID_HANDLE_); return; }
    size_t pl = strlen(path);
    uint32_t want = (uint32_t)ARG(1), seen = 0;
    char seen_names[64][128]; int nseen = 0;
    for (int i = 0; i < g_nvals; i++) {
        if (strncasecmp(g_vals[i].key, path, pl) || g_vals[i].key[pl] != '\\') continue;
        const char *child = g_vals[i].key + pl + 1;
        const char *slash = strchr(child, '\\');
        char name[128];
        snprintf(name, sizeof name, "%.*s", slash ? (int)(slash - child) : 127, child);
        int dup = 0;
        for (int k = 0; k < nseen; k++) if (!strcasecmp(seen_names[k], name)) dup = 1;
        if (dup) continue;
        if (nseen < 64) snprintf(seen_names[nseen++], 128, "%s", name);
        if (seen++ != want) continue;
        uint64_t pn = ARG(3);
        uint32_t room = pn ? (uint32_t)w32_read(w, pn, 4) : 0;
        uint32_t nl = (uint32_t)strlen(name);
        if (ARG(2) && room > nl) memcpy(W32P(w, ARG(2)), name, nl + 1);
        if (pn) w32_write(w, pn, 4, nl);
        RET(ERROR_SUCCESS_);
        return;
    }
    RET(ERROR_NO_MORE_ITEMS_);
}

static void a_RegDeleteValueA(w32 *w) {
    char name[512], path[1024];
    wide_or_narrow(w, ARG(1), 0, name, sizeof name);
    reg_load();
    if (!key_path(w, ARG(0), "", path, sizeof path)) { RET(ERROR_INVALID_HANDLE_); return; }
    reg_val *v = find_val(path, name);
    if (!v) { RET(ERROR_FILE_NOT_FOUND_); return; }
    free(v->data); free(v->key); free(v->name);
    *v = g_vals[--g_nvals];
    g_dirty = 1;
    w32_registry_flush();
    RET(ERROR_SUCCESS_);
}
static void a_RegDeleteKeyA(w32 *w) {
    char sub[512], path[1024];
    wide_or_narrow(w, ARG(1), 0, sub, sizeof sub);
    reg_load();
    if (!key_path(w, ARG(0), sub, path, sizeof path)) { RET(ERROR_INVALID_HANDLE_); return; }
    size_t pl = strlen(path);
    int removed = 0;
    for (int i = 0; i < g_nvals; ) {
        if (!strncasecmp(g_vals[i].key, path, pl) && (g_vals[i].key[pl] == 0 || g_vals[i].key[pl] == '\\')) {
            free(g_vals[i].data); free(g_vals[i].key); free(g_vals[i].name);
            g_vals[i] = g_vals[--g_nvals];
            removed = 1;
        } else i++;
    }
    if (!removed) { RET(ERROR_FILE_NOT_FOUND_); return; }
    g_dirty = 1;
    w32_registry_flush();
    RET(ERROR_SUCCESS_);
}

/* RegQueryInfoKey(hKey, class, pcchClass, reserved, pcSubKeys, pcbMaxSubKeyLen,
 *                 pcbMaxClassLen, pcValues, pcbMaxValueNameLen,
 *                 pcbMaxValueLen, pcbSecurityDescriptor, ftime) */
static void a_RegQueryInfoKeyA(w32 *w) {
    char path[1024];
    reg_load();
    if (!key_path(w, ARG(0), "", path, sizeof path)) { RET(ERROR_INVALID_HANDLE_); return; }
    size_t pl = strlen(path);
    uint32_t nvals = 0, nsub = 0, maxname = 0, maxval = 0, maxsub = 0;
    /* subkeys are distinct immediate children, not entries below the key --
     * one child holding three values is still one subkey */
    char child_names[64][128]; int nchild = 0;
    for (int i = 0; i < g_nvals; i++) {
        if (!strcasecmp(g_vals[i].key, path)) {
            if (g_vals[i].type == REG_NONE_) continue;
            nvals++;
            uint32_t nl = (uint32_t)strlen(g_vals[i].name);
            if (nl > maxname) maxname = nl;
            if (g_vals[i].len > maxval) maxval = g_vals[i].len;
        } else if (!strncasecmp(g_vals[i].key, path, pl) && g_vals[i].key[pl] == '\\') {
            const char *child = g_vals[i].key + pl + 1;
            const char *slash = strchr(child, '\\');
            char name[128];
            snprintf(name, sizeof name, "%.*s", slash ? (int)(slash - child) : 127, child);
            int dup = 0;
            for (int k = 0; k < nchild; k++) if (!strcasecmp(child_names[k], name)) dup = 1;
            if (dup) continue;
            if (nchild < 64) snprintf(child_names[nchild++], 128, "%s", name);
            nsub++;
            if (strlen(name) > maxsub) maxsub = (uint32_t)strlen(name);
        }
    }
    if (ARG(4)) w32_write(w, ARG(4), 4, nsub);
    if (ARG(5)) w32_write(w, ARG(5), 4, maxsub);
    if (ARG(7)) w32_write(w, ARG(7), 4, nvals);
    if (ARG(8)) w32_write(w, ARG(8), 4, maxname);
    if (ARG(9)) w32_write(w, ARG(9), 4, maxval);
    RET(ERROR_SUCCESS_);
}

/* Enough of the rest of advapi32 that a program asking "who am I" gets an
 * answer instead of a stub report. */
static void a_GetUserNameA(w32 *w) {
    const char *u = "xcore";
    uint32_t room = ARG(1) ? (uint32_t)w32_read(w, ARG(1), 4) : 0;
    if (ARG(1)) w32_write(w, ARG(1), 4, (uint32_t)strlen(u) + 1);
    if (ARG(0) && room > strlen(u)) memcpy(W32P(w, ARG(0)), u, strlen(u) + 1);
    RET(room > strlen(u));
}

#define F(n, a)  { #n, a, 0, a_##n, 0 }
const w32_api w32_advapi32[] = {
    F(RegOpenKeyA, 3), F(RegOpenKeyExA, 5), F(RegOpenKeyExW, 5),
    F(RegCreateKeyExA, 9), F(RegCreateKeyExW, 9),
    F(RegQueryValueExA, 6), F(RegQueryValueExW, 6),
    F(RegSetValueExA, 6), F(RegSetValueExW, 6),
    F(RegEnumValueA, 8), F(RegEnumKeyExA, 8),
    F(RegDeleteValueA, 2), F(RegDeleteKeyA, 2),
    F(RegQueryInfoKeyA, 12),
    F(RegCloseKey, 1),
    F(GetUserNameA, 2),
    { 0, 0, 0, 0, 0 },
};
