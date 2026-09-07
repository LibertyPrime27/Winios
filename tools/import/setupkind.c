/* Identify an installer by its contents. See setupkind.h for why.
 *
 * Every family here is recognised by a marker string or signature that the
 * tool which built the installer puts in the file -- a loader window class, a
 * self-extractor's own name, an archive header. None of it is heuristic about
 * *purpose*: either the bytes are there or they are not. What is heuristic is
 * the fallback, and it says so.
 *
 * Two places are searched: the first few megabytes, where a builder's own
 * strings and the PE resources live, and the last 64 KB, where an appended
 * archive's trailer sits. A zip self-extractor is the reason for the second
 * one -- its only reliable signature is the end-of-central-directory record,
 * which is at the end of the file by definition.
 */
#include "setupkind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HEAD_MAX = 4u << 20, TAIL_MAX = 64u << 10 };

/* memmem is a GNU extension and this has to build on Darwin too. */
static const unsigned char *find(const unsigned char *h, size_t hn,
                                 const void *nv, size_t nn) {
    const unsigned char *n = (const unsigned char *)nv;
    if (!nn || hn < nn) return 0;
    for (size_t i = 0; i + nn <= hn; i++)
        if (h[i] == n[0] && !memcmp(h + i, n, nn)) return h + i;
    return 0;
}
static int has(const unsigned char *h, size_t hn, const char *s) {
    return find(h, hn, s, strlen(s)) != 0;
}
/* Case-insensitive, for markers that appear with varying capitalisation
 * across versions of the same builder. */
static int has_ci(const unsigned char *h, size_t hn, const char *s) {
    size_t nn = strlen(s);
    if (!nn || hn < nn) return 0;
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t j = 0;
        for (; j < nn; j++) {
            unsigned char a = h[i + j], b = (unsigned char)s[j];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
            if (a != b) break;
        }
        if (j == nn) return 1;
    }
    return 0;
}
/* A marker stored as UTF-16LE, which is how a resource string or a wide
 * literal appears. Only ASCII markers are handled, which is all of them. */
static int has_w(const unsigned char *h, size_t hn, const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 64) return 0;
    unsigned char w[130];
    for (size_t i = 0; i < n; i++) { w[2 * i] = (unsigned char)s[i]; w[2 * i + 1] = 0; }
    return find(h, hn, w, n * 2) != 0;
}
static int hasx(const unsigned char *h, size_t hn, const char *s) {
    return has(h, hn, s) || has_w(h, hn, s);
}

/* --- the families, and their silent-install arguments -----------------------
 *
 * The flags are not interchangeable and the differences are not cosmetic:
 *
 *  - Inno takes the directory as its own `/DIR="..."` argument, and needs
 *    `/SP-` as well or it opens a "This will install..." prompt before it
 *    looks at /SILENT. /NORESTART matters because the alternative is an
 *    installer trying to reboot a phone.
 *  - NSIS takes `/D=<dir>` with no quotes and it **must be the last
 *    argument**; anything after it is treated as part of the path. Quoting it
 *    is the single most common way a scripted NSIS install goes wrong.
 *  - InstallShield's own switches are a wrapper around msiexec's, so the
 *    directory goes through `/v` as a property.
 *  - 7-Zip and WinRAR self-extractors are archive tools, so their arguments
 *    are archive arguments: an output directory and "do not ask".
 */
static const sk_info K[] = {
 { SK_INNO, "Inno Setup",
   { "/SILENT", "/SP-", "/NORESTART", "/DIR=%DIR%" }, 4, 1,
   "Inno's silent mode skips its window entirely, which is what makes it the "
   "family most likely to work here." },

 { SK_NSIS, "NSIS (Nullsoft)",
   { "/S", "/D=%DIR%" }, 2, 1,
   "NSIS silent mode. /D must come last and must not be quoted -- that is the "
   "usual reason a scripted NSIS install lands in the wrong folder." },

 { SK_INSTALLSHIELD, "InstallShield",
   { "/s", "/v/qn", "/v INSTALLDIR=%DIR%" }, 3, 0,
   "InstallShield hands its payload to the Windows Installer service, which "
   "does not exist here. Expect this to get as far as unpacking and then stop." },

 { SK_MSI_WRAPPER, "MSI bootstrapper",
   { "/quiet", "/norestart" }, 2, 0,
   "The payload is a .msi, which msiexec installs -- a Windows service, not a "
   "library, so there is nothing here to run it. Look for a portable build." },

 { SK_SFX_7Z, "7-Zip self-extracting archive",
   { "-y", "-o%DIR%" }, 2, 1,
   "This is an archive with an unpacker glued to the front, so it only has to "
   "copy files out. No registry, no services -- the good case." },

 { SK_SFX_RAR, "WinRAR self-extracting archive",
   { "-s", "-d%DIR%" }, 2, 1,
   "An archive with an unpacker in front of it. It only copies files out." },

 { SK_SFX_ZIP, "zip self-extracting archive",
   { 0 }, 0, 1,
   "The payload is an ordinary zip, so it is extracted directly rather than "
   "run -- nothing has to be emulated at all." },

 { SK_WISE, "Wise Installation System",
   { "/S" }, 1, 1,
   "Wise silent mode. Common in games from around 2000, and it does little "
   "more than copy files and write a few registry keys." },

 { SK_SETUPFACTORY, "Setup Factory",
   { "/S" }, 1, 1,
   "Setup Factory silent mode." },

 { SK_PLAIN, "not an installer",
   { 0 }, 0, 0,
   "No installer markers: this looks like a program, not a setup. Import it "
   "as a game." },

 { SK_UNKNOWN, "installer, family not recognised",
   { "/S", "/SILENT", "/VERYSILENT", "/quiet", "-y" }, 5, 0,
   "It behaves like a setup program but matches no family we know. The listed "
   "flags are what the common families accept; one of them may be silent, and "
   "an unrecognised flag is usually ignored." },

 { SK_NOT_PE, "not a Windows executable",
   { 0 }, 0, 0,
   "This is not a PE file. If it is an archive, import it as a game and it "
   "will be extracted." },
};

static sk_info of(sk_kind k) {
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) if (K[i].kind == k) return K[i];
    return K[sizeof K / sizeof K[0] - 1];
}

sk_info sk_identify_mem(const unsigned char *buf, size_t len) {
    return sk_identify_split(buf, len, 0, 0);
}

/* The real work. Head and tail are separate because a self-extractor's
 * trailer is at the end of a file that may be a gigabyte long, and reading a
 * gigabyte to identify it would be absurd. */
sk_info sk_identify_split(const unsigned char *head, size_t hn,
                          const unsigned char *tail, size_t tn) {
    if (hn < 0x40 || head[0] != 'M' || head[1] != 'Z') return of(SK_NOT_PE);

    /* Inno. The loader's window class is the strongest marker -- it is in
     * every version -- and the setup-data tag confirms it. */
    if (hasx(head, hn, "TSetupLdrWindow") || hasx(head, hn, "Inno Setup")
        || has(head, hn, "JR.Inno.Setup") || has(head, hn, "rdlptidhSetupLdr"))
        return of(SK_INNO);

    /* NSIS. "Nullsoft Install System" is in the version info; the firstheader
     * signature is the four-byte tag NSIS writes before its compressed data. */
    { static const unsigned char nsis_sig[] = { 0xEF, 0xBE, 0xAD, 0xDE,
                                                'N','u','l','l','s','o','f','t','I','n','s','t' };
      if (hasx(head, hn, "Nullsoft Install System") || hasx(head, hn, "NSIS Error")
          || find(head, hn, nsis_sig, sizeof nsis_sig))
          return of(SK_NSIS); }

    /* InstallShield before the MSI check: an InstallShield setup.exe usually
     * *contains* an .msi, and the outer wrapper is what we would be running. */
    if (hasx(head, hn, "InstallShield") || hasx(head, hn, "ISSetupPrerequisites")
        || hasx(head, hn, "_isuser") || hasx(head, hn, "ISSetup.dll"))
        return of(SK_INSTALLSHIELD);

    if (hasx(head, hn, "Wise Installation") || has(head, hn, "WiseMain")
        || hasx(head, hn, "WISE_SETUP"))
        return of(SK_WISE);

    if (hasx(head, hn, "Setup Factory") || hasx(head, hn, "irsetup.exe"))
        return of(SK_SETUPFACTORY);

    /* 7-Zip's SFX module names itself, and the config block that follows it
     * carries a tag of its own. */
    if (has(head, hn, "!@Install@!UTF-8!") || has(head, hn, "7zS.sfx")
        || has(head, hn, "7zSfx") || (has(head, hn, "7-Zip") && has(head, hn, "37z\xBC\xAF\x27\x1C")))
        return of(SK_SFX_7Z);

    /* WinRAR: the archive header signature, appended after the loader. */
    { static const unsigned char rar4[] = { 'R','a','r','!', 0x1A, 0x07, 0x00 };
      static const unsigned char rar5[] = { 'R','a','r','!', 0x1A, 0x07, 0x01, 0x00 };
      if (find(head, hn, rar4, sizeof rar4) || find(head, hn, rar5, sizeof rar5)
          || hasx(head, hn, "WinRAR self-extracting archive"))
          return of(SK_SFX_RAR); }

    /* A zip self-extractor. The end-of-central-directory record is the only
     * dependable marker and it is at the end of the file, which is what the
     * tail is for. Falling back to the head covers a small SFX read whole. */
    { static const unsigned char eocd[] = { 'P','K', 0x05, 0x06 };
      static const unsigned char lfh[]  = { 'P','K', 0x03, 0x04 };
      const unsigned char *e = tn ? find(tail, tn, eocd, 4) : 0;
      if (!e && !tail) e = find(head, hn, eocd, 4);
      if (e && find(head, hn, lfh, 4)) return of(SK_SFX_ZIP); }

    /* An MSI bootstrapper: it imports the installer API or names msiexec. */
    if (hasx(head, hn, "MsiInstallProduct") || hasx(head, hn, "msiexec")
        || hasx(head, hn, "Windows Installer XML") || hasx(head, hn, "WixBundle"))
        return of(SK_MSI_WRAPPER);

    /* Nothing matched. Distinguish "a program" from "a setup we cannot place",
     * because the advice differs and getting it wrong wastes the user's time.
     * These *are* guesses and the note says so.
     *
     * The signals: it talks about installing, it wants to elevate (a game
     * does not), or it asks for the shell folders a setup writes into. */
    if (has_ci(head, hn, "requireAdministrator") || hasx(head, hn, "SHGetSpecialFolderPath")
        || has_ci(head, hn, "setup.exe") || has_ci(head, hn, "installer")
        || hasx(head, hn, "Uninstall") )
        return of(SK_UNKNOWN);

    return of(SK_PLAIN);
}

sk_info sk_identify(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { sk_info i = of(SK_NOT_PE); i.note = "could not be opened"; return i; }

    unsigned char *head = (unsigned char *)malloc(HEAD_MAX);
    unsigned char *tail = (unsigned char *)malloc(TAIL_MAX);
    if (!head || !tail) { free(head); free(tail); fclose(f);
                          sk_info i = of(SK_NOT_PE); i.note = "out of memory"; return i; }

    size_t hn = fread(head, 1, HEAD_MAX, f);
    size_t tn = 0;
    if (!fseek(f, 0, SEEK_END)) {
        long end = ftell(f);
        if (end > (long)TAIL_MAX) {
            if (!fseek(f, end - (long)TAIL_MAX, SEEK_SET)) tn = fread(tail, 1, TAIL_MAX, f);
        }
        /* A file shorter than the tail window is entirely in `head` already,
         * so leaving tn at 0 is right rather than reading it twice. */
    }
    fclose(f);

    sk_info r = sk_identify_split(head, hn, tn ? tail : 0, tn);
    free(head); free(tail);
    return r;
}

int sk_silent_argv(const sk_info *info, const char *dir,
                   const char **out, char *scratch, size_t scratch_len) {
    if (!info || !out) return 0;
    size_t used = 0;
    int n = 0;
    for (int i = 0; i < info->nargs && i < SK_MAX_ARGS; i++) {
        const char *a = info->args[i];
        if (!a) break;
        const char *pc = strstr(a, "%DIR%");
        if (!pc) { out[n++] = a; continue; }
        if (!dir) continue;                    /* a directory-bearing flag with no directory */
        size_t need = strlen(a) - 5 + strlen(dir) + 1;
        if (used + need > scratch_len) break;
        char *p = scratch + used;
        snprintf(p, scratch_len - used, "%.*s%s%s", (int)(pc - a), a, dir, pc + 5);
        used += need;
        out[n++] = p;
    }
    return n;
}
