/* What kind of installer is this, and how do you tell it not to open a window?
 *
 * An installer's GUI is the part we cannot run: real Windows dialogs are
 * comctl32 controls, and comctl32 is not implemented here. Every installer
 * family in wide use has a silent mode, though -- it exists so that IT
 * departments can deploy without a human clicking Next -- and in silent mode
 * an installer is a file copier with a registry writer attached, which is a
 * shape this runtime can already be honest about.
 *
 * So the question "will this installer work" mostly reduces to "which family
 * is it, and what are its flags". That is what this answers, from the bytes
 * of the file rather than from its name: `setup.exe` says nothing, and a
 * repackaged installer is routinely renamed to the game's title.
 */
#ifndef SETUPKIND_H
#define SETUPKIND_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SK_UNKNOWN = 0,   /* a PE we could not attribute to any family        */
    SK_NOT_PE,        /* not a Windows executable at all                  */
    SK_PLAIN,         /* a PE with no installer markers: probably a game  */
    SK_INNO,          /* Inno Setup                                      */
    SK_NSIS,          /* Nullsoft Scriptable Install System               */
    SK_INSTALLSHIELD, /* InstallShield (setup.exe wrapping a .cab/.msi)   */
    SK_MSI_WRAPPER,   /* a bootstrapper whose payload is a Windows .msi   */
    SK_SFX_7Z,        /* 7-Zip self-extracting archive                    */
    SK_SFX_RAR,       /* WinRAR self-extracting archive                   */
    SK_SFX_ZIP,       /* a zip SFX, or a plain zip renamed to .exe        */
    SK_WISE,          /* Wise Installation System (older games)           */
    SK_SETUPFACTORY,  /* Setup Factory                                    */
} sk_kind;

/* Up to this many argv entries are suggested, not counting the NULL. */
enum { SK_MAX_ARGS = 8 };

typedef struct {
    sk_kind kind;
    const char *name;          /* human-readable family name              */
    /* The silent-install arguments this family takes. `%DIR%` in any entry
     * is replaced by the caller with the install directory it has chosen --
     * the families differ on whether the directory is a separate argument,
     * glued to the flag, or must come last, so this is a template and not a
     * list of independent flags. */
    const char *args[SK_MAX_ARGS];
    int nargs;
    /* Whether a silent install of this family is expected to work at all.
     * MSI wrappers get 0: the payload is handed to msiexec, which is a
     * Windows service we do not have and cannot fake -- saying so up front
     * is more use than a run that fails somewhere confusing. */
    int silent_supported;
    /* One line for the person doing the importing, saying what will happen. */
    const char *note;
} sk_info;

/* Identify the file at `path`. Never fails: an unreadable or non-PE file
 * comes back as SK_NOT_PE with a note saying so. */
sk_info sk_identify(const char *path);

/* The same, over bytes already in memory (`len` may be short -- the markers
 * live in the first ~200 KB of every family here, and a truncated buffer
 * simply misses ones that were not read). */
sk_info sk_identify_mem(const unsigned char *buf, size_t len);

/* The same, when the head and the tail of a large file were read separately.
 * A self-extracting archive is identified by a trailer at the very end, and
 * reading a gigabyte of payload to find it would be absurd. `tail` may be
 * NULL, in which case only the head is searched. */
sk_info sk_identify_split(const unsigned char *head, size_t head_len,
                          const unsigned char *tail, size_t tail_len);

/* Fill `out` with the silent argv for `info`, substituting `dir` for %DIR%.
 * Returns the number of arguments written. `out` holds pointers into
 * `scratch`, which must be at least SK_MAX_ARGS * 1024 bytes. */
int sk_silent_argv(const sk_info *info, const char *dir,
                   const char **out, char *scratch, size_t scratch_len);

#ifdef __cplusplus
}
#endif

#endif
