/* win32 -- the Windows side of xcore: a PE loader and the host-implemented
 * DLL surface a Windows executable calls into.
 *
 * The idea is the one Wine proved: the guest's own code runs unchanged (on
 * xcore), and every import it names resolves to a function we implement on
 * the host. Here an import is a 16-byte *stub* in a guest-visible page whose
 * first byte is `int3`. Calling it stops the CPU with RIP just past the int3;
 * the runtime looks the stub up, reads the arguments where the calling
 * convention put them (rcx/rdx/r8/r9 + stack on x64, the stack on x86),
 * runs the host implementation, writes the result to rax, and returns to the
 * guest's return address -- popping the arguments too for stdcall.
 *
 * Both bitnesses share every implementation: `w32_arg(i)` and `w32_ret()`
 * hide the convention, `W32P()` hides the memory model (identity for 64-bit,
 * arena base + zext32 for 32-bit), and sizes such as FILE or lconv are
 * computed from `w->is32`. Guest callbacks (TLS callbacks, _initterm, atexit)
 * run through w32_call_guest(), which pushes a return-to-host stub and
 * re-enters the run loop until it is hit.
 */
#ifndef W32_H
#define W32_H

#include "xcore/cpu.h"
#include <stddef.h>
#include <stdint.h>

enum { W32_MAX_STUBS = 2048, W32_MAX_HANDLES = 256, W32_MAX_TLS = 64, W32_MAX_MODULES = 64 };

typedef struct w32 w32;
typedef void (*w32_fn)(w32 *w);

/* One host-implemented export. conv: 0 = stdcall/x64, callee pops (x86);
 * 1 = cdecl, caller pops. data_size > 0: not a function but an exported
 * variable of that many bytes (the IAT gets its address). */
typedef struct {
    const char *name;
    int         nargs;
    int         conv;
    w32_fn      fn;
    int         data_size;
} w32_api;

typedef struct {
    const char    *name;         /* "kernel32.dll" */
    const w32_api *apis;         /* terminated by name == NULL */
    uint64_t       hmodule;      /* fake module base */
} w32_dll;

/* One loaded PE image -- the executable, or a guest DLL beside it. Host DLLs
 * (kernel32 and friends) are not modules: they have no image, and their
 * handles are the fake pages stubs_init makes. */
typedef struct {
    char     name[64];           /* lowercased basename with extension, "engine.dll" */
    char     path[512];
    uint64_t base, size;
    uint64_t entry;              /* DllMain, or the executable's entry point; 0 if none */
    uint32_t exp_rva, exp_size;  /* export directory, 0 if the image exports nothing */
    uint64_t tls_callbacks;
    int      is_exe;
    int      refs;               /* LoadLibrary count; nothing is ever unmapped */
    int      attached;           /* DllMain(DLL_PROCESS_ATTACH) has run */
    int      seq;                /* order this image *finished* loading: dependency order */
} w32_module;

typedef enum { H_NONE = 0, H_FILE, H_PROCESS, H_THREAD, H_HEAP, H_EVENT, H_MUTEX } w32_htype;
typedef struct { w32_htype type; int fd; int flags; } w32_handle;

struct w32 {
    xc_cpu   *c;
    xc_mem   *mem;
    int       is32;
    uint8_t  *base;              /* arena base (32-bit), NULL for identity */
    int       verbose;

    /* the image */
    const char *exe_path;
    uint64_t  image_base, image_size, entry;
    uint32_t  tls_index;
    uint64_t  tls_callbacks;     /* guest address of the TLS callback array, 0 if none */

    /* process state in guest memory */
    uint64_t  teb, peb, cmdline, cmdline_w, env_block, env_block_w;
    uint64_t  stack_base, stack_limit;
    uint64_t  stub_base;         /* the int3 stubs */
    int       nstubs;
    struct { const w32_dll *dll; const w32_api *api; const char *missing; } stubs[W32_MAX_STUBS];
    uint64_t  data_exports[W32_MAX_STUBS];   /* guest address per stub for data exports */

    /* heap: bump allocator plus a size header per block */
    uint64_t  heap_cur, heap_end;
    uint64_t  tls_slots[W32_MAX_TLS]; uint64_t tls_used;
    uint64_t  tls_array;         /* TEB.ThreadLocalStoragePointer: one entry per module with TLS */
    int       ntls;

    /* loaded images */
    w32_module mods[W32_MAX_MODULES];
    int        nmods, nloaded;
    const char *dll_dir;         /* extra directory to search for guest DLLs (-L) */
    w32_handle handles[W32_MAX_HANDLES];
    uint32_t  last_error;

    /* run state */
    int       exited, exit_code;
    int       depth;
    uint64_t  atexit_fns[64]; int natexit;

    int       argc; char **argv;
};

/* memory */
void    *W32P(w32 *w, uint64_t addr);                      /* guest -> host, NULL for guest NULL */
uint64_t w32_alloc(w32 *w, uint64_t size, int exec);        /* fresh zeroed pages */
uint64_t w32_alloc_at(w32 *w, uint64_t addr, uint64_t size, int exec);   /* 0 on failure */
uint64_t w32_host_page(void);                               /* host page size (16 KB on Apple silicon) */
uint64_t w32_heap_alloc(w32 *w, uint64_t size);
uint64_t w32_heap_realloc(w32 *w, uint64_t addr, uint64_t size);
void     w32_heap_free(w32 *w, uint64_t addr);
uint64_t w32_heap_size(w32 *w, uint64_t addr);
uint64_t w32_strdup(w32 *w, const char *s);                 /* into the heap */
uint64_t w32_wstrdup(w32 *w, const char *s);                /* UTF-16 */
size_t   w32_wcslen(w32 *w, uint64_t p);
const char *w32_str(w32 *w, uint64_t addr);                 /* host pointer to a guest C string ("" for NULL) */
void     w32_wtoa(w32 *w, uint64_t wp, char *out, size_t n);   /* UTF-16 -> ASCII-ish */

/* calling convention */
uint64_t w32_arg(w32 *w, int i);
double   w32_farg(w32 *w, int i);                           /* i-th float/double argument (x64: xmm0-3) */
void     w32_ret(w32 *w, uint64_t v);
void     w32_ret64(w32 *w, uint64_t v);                     /* 64-bit result (edx:eax on x86) */
void     w32_fret(w32 *w, double v);
uint64_t w32_read(w32 *w, uint64_t addr, int bytes);
void     w32_write(w32 *w, uint64_t addr, int bytes, uint64_t v);
uint64_t w32_ptrsize(w32 *w);
uint64_t w32_call_guest(w32 *w, uint64_t fn, int nargs, const uint64_t *args);
void     w32_exit(w32 *w, int code);
void     w32_set_last_error(w32 *w, uint32_t e);

/* handles */
uint64_t w32_handle_new(w32 *w, w32_htype t, int fd);
w32_handle *w32_handle_get(w32 *w, uint64_t h);
void     w32_handle_close(w32 *w, uint64_t h);

/* stubs / modules */
uint64_t w32_stub_for(w32 *w, const char *dll, const char *name);   /* resolves or makes a "missing" stub */
uint64_t w32_module_handle(w32 *w, const char *dll);                /* host-implemented DLLs only */

/* loader (pe.c) */
int      w32_load_pe(w32 *w, const char *path);                     /* the executable */
uint64_t w32_load_library(w32 *w, const char *name);                /* a guest DLL, or a host module handle; 0 if unknown */
w32_module *w32_module_at(w32 *w, uint64_t base);                   /* the loaded image with that base, or NULL */
uint64_t w32_module_export(w32 *w, uint64_t hmodule, const char *name, int ordinal);
uint64_t w32_import_addr(w32 *w, const char *dll, const char *name, int ordinal, int depth);
void     w32_attach_modules(w32 *w);                                /* DllMain(DLL_PROCESS_ATTACH) for every new DLL */

/* the DLLs */
extern const w32_api w32_kernel32[];
extern const w32_api w32_msvcrt[];
extern const w32_api w32_ntdll[];
extern const w32_api w32_user32[];

/* msvcrt.c: drop every cached guest address, so a second process can start
 * in the same host process (see winrun_main). */
void w32_reset_statics(void);

/* runtime (winrun.c) */
int w32_run(w32 *w);

/* TEB/PEB offsets that both bitnesses need */
#define TEB64_LASTERROR 0x68
#define TEB64_TLS       0x1480
#define TEB64_PEB       0x60
#define TEB64_TLSPTR    0x58
#define TEB32_LASTERROR 0x34
#define TEB32_TLS       0xE10
#define TEB32_PEB       0x30
#define TEB32_TLSPTR    0x2C

#define W32_STD_INPUT  ((uint64_t)(int32_t)-10)
#define W32_STD_OUTPUT ((uint64_t)(int32_t)-11)
#define W32_STD_ERROR  ((uint64_t)(int32_t)-12)

/* helpers for implementations */
#define ARG(i)  w32_arg(w, (i))
#define RET(v)  w32_ret(w, (uint64_t)(v))
#define GSTR(a) w32_str(w, (a))

#endif
