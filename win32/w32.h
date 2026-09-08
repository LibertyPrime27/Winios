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
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

enum { W32_MAX_STUBS = 2048, W32_MAX_HANDLES = 256, W32_MAX_TLS = 64, W32_MAX_MODULES = 64,
       W32_MAX_THREADS = 64 };

typedef struct w32 w32;
typedef void (*w32_fn)(w32 *w);

/* One guest thread.
 *
 * Registers, TEB, stack, last error and TLS values are per thread; guest
 * memory, the heap, handles and modules are per process. Every guest thread
 * is a real host thread, but only one executes guest instructions at a time
 * -- see the top of win32/thread.c for why that trade was made. */
typedef struct w32_thread {
    xc_cpu   *c;                 /* this thread's registers */
    xc_cpu    cpu;               /* ...owned here, except for thread 0's */
    uint64_t  teb;
    uint64_t  stack_base, stack_limit;
    uint64_t  tls_array;         /* TEB.ThreadLocalStoragePointer: one entry per module with TLS */
    uint64_t  entry, param;
    uint64_t  handle;            /* the HANDLE that waits on it */
    uint32_t  id;                /* GetCurrentThreadId */
    uint32_t  exit_code;
    int       depth;             /* w32_call_guest nesting, on this thread */
    int       used, running, finished;
    pthread_t host;
} w32_thread;

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

/* One host-implemented DLL. `apis` is a small array because a single DLL's
 * exports genuinely live in several files -- kernel32's are split across
 * kernel32.c, seh.c and thread.c -- and threading them together at the lookup
 * is less trouble than one file that owns everything. Unused slots are NULL. */
enum { W32_DLL_TABLES = 4 };
typedef struct {
    const char    *name;         /* "kernel32.dll" */
    const w32_api *apis[W32_DLL_TABLES];   /* each terminated by name == NULL */
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
    uint32_t res_rva, res_size;  /* resource directory: dialogs, strings, icons */
    uint64_t tls_callbacks;
    int      is_exe;
    int      refs;               /* LoadLibrary count; nothing is ever unmapped */
    int      attached;           /* DllMain(DLL_PROCESS_ATTACH) has run */
    int      seq;                /* order this image *finished* loading: dependency order */
} w32_module;

/* A COM interface: methods in vtable order, `nmethods` of them, with a NULL
 * name for a slot we have not implemented. `nargs` counts `this`, which is
 * argument 0 in both bitnesses. See com.c. */
typedef struct {
    const char    *name;         /* "IDirect3DDevice9" */
    const w32_api *methods;
    int            nmethods;
    int            tag;          /* identifies the class in an object header */
    uint64_t       vtable;       /* guest address, built on first use */
    w32_dll        dll;          /* so the dispatcher can name the method */
} w32_com_class;

typedef enum { H_NONE = 0, H_FILE, H_PROCESS, H_THREAD, H_HEAP, H_EVENT, H_MUTEX,
               H_FIND,        /* a directory walk: FindFirstFile/FindNextFile */
               H_MAPPING      /* a file mapping: CreateFileMapping/MapViewOfFile */
             } w32_htype;
/* `p` and `u1`/`u2` are for the handle kinds that need more than a descriptor:
 * a directory walk carries its DIR* and the pattern it is matching, a mapping
 * carries its size. Kept in the handle rather than a side table so closing one
 * is still just closing one. */
typedef struct { w32_htype type; int fd; int flags; void *p; uint64_t u1, u2; } w32_handle;

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

    /* process state in guest memory. The TEB and the stack are *not* here:
     * they are per thread, in w32_thread -- see thread.c. */
    uint64_t  peb, cmdline, cmdline_w, env_block, env_block_w;
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
    int        imports_only;     /* -imports: load, report what is missing, do not run */
    int        dump_frame;      /* -frame: print what was drawn on the way out */
    int        keep_going;       /* -k: an unimplemented import returns a lie instead of ending the run */
    uint64_t   fake_page;        /* zero-filled, handed back by an unimplemented function so a
                                  * caller that dereferences the result reads rather than faults */
    uint32_t   faked_returns;    /* how many times that happened, for the report */

    /* what was called that we do not implement, and how often. A run in
     * keep_going mode produces this whole list instead of stopping at the
     * first name, which is the difference between one clue and a work queue. */
    struct { const char *name; uint32_t calls; } unimpl[128];
    int        nunimpl;
    uint32_t   unimpl_dropped;   /* distinct names past the end of the table */
    uint64_t   deadline_ns;      /* stop after this (0 = no limit) */
    const char *stop_reason;     /* why the run ended, for the crash report */
    w32_handle handles[W32_MAX_HANDLES];

    /* run state */
    int       exited, exit_code;
    int       redirected;        /* a host implementation set rip/rsp itself:
                                  * the stub dispatcher must not return for it.
                                  * Process-wide and safe: it is set and
                                  * consumed inside one hold of the guest
                                  * lock, so no two threads can be between
                                  * the two halves at once. */
    uint64_t  atexit_fns[64]; int natexit;

    int       argc; char **argv;
};

/* memory */
void    *W32P(w32 *w, uint64_t addr);                      /* guest -> host, NULL for guest NULL */
uint64_t w32_alloc(w32 *w, uint64_t size, int exec);        /* fresh zeroed pages */
uint64_t w32_alloc_at(w32 *w, uint64_t addr, uint64_t size, int exec);   /* 0 on failure */
/* Guest pages backed by a file. Falls back to ordinary pages the caller can
 * read into when the offset is not host-page aligned or mmap refuses, so it
 * never fails outright -- returns 0 only when there is no guest space. `mapped`
 * says whether the file is really mapped or the caller must fill it. */
uint64_t w32_map_file(w32 *w, int fd, uint64_t offset, uint64_t size, int writable, int *mapped);
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
void     w32_wtoa_n(w32 *w, uint64_t wp, int chars, char *out, size_t n);  /* counted run */
/* A Windows path as the host sees it: under the drive root for an absolute
 * one, under the current directory for a relative one. Every file call goes
 * through this, so anything outside kernel32.c that touches a path has to use
 * it too rather than growing a second set of rules. */
void     w32_host_path(w32 *w, const char *win, char *out, size_t n);

/* calling convention */
uint64_t w32_arg(w32 *w, int i);
double   w32_farg(w32 *w, int i);                           /* i-th float/double argument (x64: xmm0-3) */
void     w32_ret(w32 *w, uint64_t v);
void     w32_ret64(w32 *w, uint64_t v);                     /* 64-bit result (edx:eax on x86) */
void     w32_fret(w32 *w, double v);
uint64_t w32_read(w32 *w, uint64_t addr, int bytes);
void     w32_write(w32 *w, uint64_t addr, int bytes, uint64_t v);
/* Is this guest range actually mapped? A 64-bit guest's addresses are host
 * addresses, so a pointer a program got wrong is a host pointer unless
 * something checks -- and one of those took the whole app down on a phone,
 * without a signal any handler could report. */
int      w32_mem_ok(w32 *w, uint64_t addr, uint64_t len);
/* W32P, but NULL when `len` bytes are not mapped. Use this wherever the host
 * is about to memset or memcpy through an address the guest supplied. */
void    *W32PN(w32 *w, uint64_t addr, uint64_t len);
uint64_t w32_ptrsize(w32 *w);
uint64_t w32_call_guest(w32 *w, uint64_t fn, int nargs, const uint64_t *args);
void     w32_exit(w32 *w, int code);
void     w32_set_last_error(w32 *w, uint32_t e);
/* Record that a call was made that we cannot honour, so it appears in the run
 * report alongside the imports that were never implemented at all. For a
 * function that exists here and still has to fail -- CreateProcess, with no
 * second process to create -- which is more useful in the report than a
 * silent zero. */
void     w32_note_refused(w32 *w, const char *name);
/* Create the directories a Windows installer expects to find (Program Files,
 * Windows\System32, Temp, a user profile) under the current C:. Called before
 * an install, not before an ordinary run: a directory walk of C:\ is
 * observable and an empty drive is what the tests recorded. */
void     w32_drive_init(void);

/* What the guest is told the display is.
 *
 * A game asks GetSystemMetrics or EnumDisplaySettings before it chooses a
 * backbuffer, so this is the number that decides how many pixels it draws --
 * and until the Metal path exists those pixels go through a software
 * rasterizer, which makes it the largest performance lever there is after the
 * dynarec itself. Settable so the app can offer it.
 *
 * user32.c and d3d9.c have to agree, or a game picks a mode the device then
 * reports differently; they used to agree by having the same constant typed
 * into both, kept in step by a comment. */
/* user32's wsprintfA/W, implemented in msvcrt.c next to the formatter and the
 * guest-memory variadic walk they share with sprintf. */
void     w32_do_wsprintf(w32 *w, int wide);
void     w32_do_wvsprintf(w32 *w, int wide);   /* the same, given a va_list */

/* The environment, and the .ini rewrite. Both are shared between the A and W
 * entry points and between DLLs, so they live where the implementation is
 * rather than being written twice. */
const char *w32_env_lookup(w32 *w, const char *name);
int      w32_env_set(const char *name, const char *val);   /* NULL removes */
int      w32_ini_write(w32 *w, const char *app, const char *key,
                       const char *val, const char *file);

void     w32_set_screen_size(int cx, int cy);
void     w32_screen_size(int *cx, int *cy);

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

/* COM (com.c) */
uint64_t w32_com_vtable(w32 *w, w32_com_class *cls);
uint64_t w32_com_new(w32 *w, w32_com_class *cls, uint32_t nfields);
uint64_t w32_com_get(w32 *w, uint64_t obj, int n);          /* object field n (64-bit slots) */
void     w32_com_set(w32 *w, uint64_t obj, int n, uint64_t v);
int      w32_com_tag(w32 *w, uint64_t obj);
void     w32_com_reset(void);                               /* a new process: forget every built vtable */
void     w32_com_QueryInterface(w32 *w);                    /* IUnknown, shared by every interface */
void     w32_com_AddRef(w32 *w);
void     w32_com_Release(w32 *w);

/* structured exception handling (seh.c) */
void     w32_context_save(w32 *w, uint64_t ctx);            /* CPU -> guest CONTEXT */
void     w32_context_load(w32 *w, uint64_t ctx);            /* guest CONTEXT -> CPU */
/* Raise an exception in the guest. 1 if a handler took it and the CPU is set
 * up to carry on, 0 if nothing did -- then the caller ends the run. */
int      w32_raise(w32 *w, uint32_t code, uint32_t flags, uint64_t exc_addr,
                   int nparams, const uint64_t *params);
int      w32_fault_to_exception(w32 *w);                    /* a CPU fault, as the exception Windows would raise */
const char *w32_exception_name(uint32_t code);
uint32_t w32_last_exception(uint64_t *addr);                /* what ended the run, for the report */
void     w32_seh_reset(void);
/* one int3 stub bound to `api`, or a named "not implemented" stub when it is NULL */
uint64_t w32_stub_alloc(w32 *w, const w32_dll *dll, const w32_api *api, char *missing);

/* the DLLs */
extern const w32_api w32_kernel32[];
extern const w32_api w32_seh_kernel32[];
extern const w32_api w32_seh_ntdll[];
extern const w32_api w32_msvcrt[];
extern const w32_api w32_ntdll[];
extern const w32_api w32_user32[];
extern const w32_api w32_winmm[];
extern const w32_api w32_dsound[];
extern const w32_api w32_xinput[];
extern const w32_api w32_thread_api[];      /* kernel32's threads and synchronisation */
extern const w32_api w32_thread_crt[];      /* msvcrt's _beginthread* */

/* thread.c ---------------------------------------------------------------
 *
 * The calling thread's state, and its registers. Every host thread that runs
 * guest code has a slot; anything else gets thread 0's, which is the only
 * answer that can be right before the process has started.
 */
w32_thread *w32_self(void);
xc_cpu     *w32_cpu(const w32 *w);
/* Held while executing guest instructions. One thread at a time, handed over
 * between execution slices and at every blocking call, which is what keeps
 * the block cache, the code arena, the heap and the handle table
 * single-threaded. */
void w32_guest_lock(void);
void w32_guest_unlock(void);
void w32_guest_yield(void);
int  w32_exiting(void);                     /* ExitProcess: unwind every thread */
void w32_thread_exit_all(void);
int  w32_thread_count(void);
void w32_thread_reset(void);
/* Register the thread the process started on, before any guest code runs. */
w32_thread *w32_thread_main(w32 *w, xc_cpu *c, uint64_t teb, uint64_t lo, uint64_t hi);
/* Leave the innermost w32_call_guest as returning from the guest function
 * would. ExitThread uses it rather than unwinding the host stack. */
void w32_return_to_host(w32 *w);
extern const w32_api w32_d3d9[];
extern const w32_api w32_advapi32[];
extern const w32_api w32_shell32[];
extern const w32_api w32_ole32[];
extern const w32_api w32_gdi32[];
extern const w32_api w32_comctl32[];
/* misc_dlls.c: the long tail a game engine links and rarely calls */
extern const w32_api w32_imm32[];
extern const w32_api w32_dwmapi[];
extern const w32_api w32_uxtheme[];
extern const w32_api w32_avrt[];
extern const w32_api w32_version[];
extern const w32_api w32_rpcrt4[];
extern const w32_api w32_gdiplus[];
extern const w32_api w32_comdlg32[];
extern const w32_api w32_dbghelp[];
extern const w32_api w32_iphlpapi[];
extern const w32_api w32_propsys[];
/* net_dlls.c: networking, present so that games start without it */
extern const w32_api w32_ws2_32[];
extern const w32_api w32_wininet[];
const char *w32_ws2_ordinal_name(int ordinal);
/* --- Direct3D 11 -----------------------------------------------------------
 *
 * The rasterizer d3d11.c draws through, in d3d11_raster.c. Integer
 * arithmetic throughout, so a frame is the same on every machine.
 */
typedef struct {
    uint32_t *pixels;
    int w, h, pitch_px;                 /* pitch in pixels, not bytes */
    int clip_x, clip_y, clip_w, clip_h; /* the viewport */
} d3d11_target;

typedef struct {
    const uint32_t *pixels;
    int w, h, pitch_px;
} d3d11_texture;

/* A vertex after the fixed-function interpretation of the vertex stage: a
 * position already in pixels, a texture coordinate, and a colour. */
typedef struct {
    float x, y, z, w;
    float u, v;
    uint32_t color;                     /* 0xAARRGGBB */
} d3d11_vertex;

enum { D3D11_BLEND_NONE_ = 0, D3D11_BLEND_OVER_ = 1, D3D11_BLEND_ADD_ = 2 };

void w32_d3d11_triangle(const d3d11_target *t, const d3d11_vertex *v0,
                        const d3d11_vertex *v1, const d3d11_vertex *v2,
                        const d3d11_texture *tex, int wrap, int blend_mode);
void w32_d3d11_clear(const d3d11_target *t, uint32_t argb);
void w32_d3d11_reset(void);

/* d3d11.c: see the file for what is and is not implemented */
extern const w32_api w32_d3d11[];
extern const w32_api w32_d3d10[];
extern const w32_api w32_d3d12[];
extern const w32_api w32_dxgi[];

/* --- the screen, and the windows on it -----------------------------------
 *
 * gdi32 draws into a surface user32 owns, and user32 draws its controls with
 * gdi32's primitives, so the two files need each other. Rather than let
 * either reach into the other's tables, everything that crosses is here.
 */
uint32_t *w32_desktop_bits(int *cx, int *cy);            /* the screen surface, BGRA */
const uint32_t *w32_desktop_peek(int *cx, int *cy);
void      w32_desktop_damaged(void);                     /* something drew: present it */
int       w32_window_area(uint64_t hwnd, int whole, int *x, int *y, int *cx, int *cy);
int       w32_window_parent_area(uint64_t hwnd, int *x, int *y, int *cx, int *cy);
int       w32_dialog_pump(w32 *w);                       /* one pass for a host-owned loop */

/* Windows, made from the host side: a dialog template creates controls, and
 * it has no guest stack to read the arguments from. */
uint64_t  w32_new_window(w32 *w, const char *cls, const char *text,
                         uint32_t style, uint32_t exstyle, int x, int y, int cw, int ch,
                         uint64_t parent, uint64_t id, uint64_t inst, uint64_t param,
                         int run_wm_create);
void      w32_destroy_window(w32 *w, uint64_t hwnd);
void      w32_set_window_text(w32 *w, uint64_t hwnd, const char *s);
void      w32_get_window_text(w32 *w, uint64_t hwnd, char *out, size_t n);
void      w32_register_control_class(const char *name);  /* InitCommonControls */
uint32_t  w32_sys_color(int index);
/* comctl32's subclass chain, which is really a property of the window */
void      w32_window_subclass(w32 *w, uint64_t hwnd, uint64_t proc, uint64_t ref);
void      w32_window_unsubclass(w32 *w, uint64_t hwnd, uint64_t proc);
uint64_t  w32_window_defproc(w32 *w, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp);

/* gdi32, for user32 */
uint64_t  w32_dc_for_window(w32 *w, uint64_t hwnd, int whole_window);
void      w32_dc_release(uint64_t hdc);
uint64_t  w32_stock_object(int index);
uint64_t  w32_make_solid_brush(uint32_t colorref);
uint64_t  w32_make_font(int height, const char *face);
void      w32_gdi_fill_rect(uint64_t hdc, int l, int t, int r, int b, uint32_t colorref);
void      w32_gdi_frame_rect(uint64_t hdc, int l, int t, int r, int b, uint32_t colorref);
void      w32_gdi_line(uint64_t hdc, int x0, int y0, int x1, int y1, uint32_t colorref);
int       w32_gdi_text_at(uint64_t hdc, int x, int y, const char *s, int len);
void      w32_gdi_text_extent(uint64_t hdc, const char *s, int len, int *cx, int *cy);
int       w32_gdi_average_width(uint64_t hdc);   /* what a dialog's units are defined in */
/* How dense we claim the display is. A desktop installer was drawn for 96
 * DPI; on a tablet panel that dialog is a postage stamp, so the app raises
 * this and every dialog lays itself out proportionally bigger -- which is
 * exactly what Windows' own DPI scaling does. */
void      w32_set_ui_dpi(int dpi);
int       w32_ui_dpi(void);
int       w32_points_to_pixels(int points);
int       w32_gdi_line_height(uint64_t hdc);
uint32_t  w32_gdi_brush_color(uint64_t hbrush, int *is_null);

/* Pictures, made on the gdi32 side because that is where the object table
 * lives, but created by user32's LoadImage/LoadIcon and drawn by its window
 * painting. `px` is 0xAARRGGBB and is taken over by the object.
 *
 * An icon and a cursor are the same thing here as they are on Windows: a
 * bitmap with an alpha channel and a hot spot. Keeping them one type is why
 * DrawIconEx can draw either. */
uint64_t  w32_gdi_make_bitmap(int cx, int cy, uint32_t *px);
uint64_t  w32_gdi_make_icon(int cx, int cy, uint32_t *px, int hx, int hy);
int       w32_gdi_icon_size(uint64_t h, int *cx, int *cy, int *hx, int *hy);
const uint32_t *w32_gdi_icon_bits(uint64_t h);
void      w32_gdi_delete_object(uint64_t h);
/* Alpha-blend an icon or bitmap into a DC, scaled to cx by cy (0 for its own
 * size). This is what a title bar, a message box and DrawIconEx all use. */
void      w32_gdi_draw_image(uint64_t hdc, uint64_t himg, int x, int y, int cx, int cy);
void      w32_gdi_set_text_color(uint64_t hdc, uint32_t colorref);
void      w32_gdi_set_bk_color(uint64_t hdc, uint32_t colorref);
void      w32_gdi_set_bk_mode(uint64_t hdc, int mode);
uint64_t  w32_gdi_set_font(uint64_t hdc, uint64_t hfont);
void      w32_gdi_clip_to(uint64_t hdc, int l, int t, int r, int b);

/* pe.c: the resource directory, which is where a dialog template lives */
uint64_t  w32_find_resource(w32 *w, uint64_t module, uint64_t type, uint64_t name, int wide);
uint64_t  w32_resource_data(w32 *w, uint64_t hrsrc, uint32_t *size);
/* thread.c: what kernel32 builds waitable timers and condition variables on */
int       w32_cond_sleep(w32 *w, uint64_t cv, uint64_t lock, uint32_t ms, int srw);
uint64_t  w32_make_event(w32 *w, int manual, int set);
void      w32_set_event(w32 *w, uint64_t h, int on);
void      w32_thread_exit_self(w32 *w, uint32_t code);
/* kernel32.c: a string out of the RT_STRING blocks, for user32's LoadString */
int       w32_load_string(w32 *w, uint64_t inst, uint32_t id, char *out, size_t cap);
/* comctl32.c: what a new run has to start without */
void      w32_comctl32_reset(void);
/* comctl32.c: the documented ordinal-only exports, by number */
const char *w32_ordinal_name(const char *dll, int ordinal);
/* winrun.c: give the host a moment while a modal loop waits for input */
void      w32_host_idle(void);

/* advapi32.c: the registry is persisted next to the guest's C: drive. Called
 * when a run ends so an installer's writes survive to the next launch. */
void w32_registry_flush(void);
void w32_registry_reset(void);

/* user32.c: input from the host.
 *
 * The app's keyboard, pointer and touch handlers call these from whatever
 * thread they run on, while the guest runs on its own -- so nothing here
 * touches guest memory, and the state and message queue behind them are
 * locked. Each event does two things: it updates the state a frame loop polls
 * with GetAsyncKeyState/GetCursorPos, and it queues the message a message
 * loop pumps. A program that uses either sees the same events.
 *
 * Coordinates are *client pixels* of the guest's window -- the app knows the
 * rect it drew the last frame into and maps a touch through it, so the
 * letterboxing lives in one place. w32_client_size() is that size. */
void w32_input_key(int vk, int down);              /* a virtual-key transition */
/* The same, carrying the character the host's keyboard layout resolved for
 * it. TranslateMessage uses that instead of deriving one, so a non-US layout
 * types what it should -- and a program that never calls TranslateMessage
 * still sees no WM_CHAR, as on Windows. */
void w32_input_key_ch(int vk, int down, uint32_t ch);
void w32_input_char(uint32_t ch);                  /* a character with no key behind it */
void w32_input_mouse_move(int x, int y);           /* absolute, client pixels */
void w32_input_mouse_delta(int dx, int dy);        /* relative: mouselook, a trackpad */
void w32_input_mouse_button(int button, int down); /* 0 left, 1 right, 2 middle */
void w32_input_mouse_wheel(int delta);             /* +/-120 per notch */
void w32_input_reset(void);
void w32_client_size(int *cw, int *ch);
int  w32_has_window(void);                         /* has the guest made one yet? */
/* Whether the guest wants a pointer drawn, and where it thinks it is. A game
 * hides the cursor to say "I am doing mouselook now", which is exactly when a
 * virtual cursor should get out of the way.
 *
 * user32 now draws the pointer itself, into the frame, from the cursor the
 * program set. The host's own pointer overlay is the other half of that and
 * the two must not both be showing: this one is what a program sets and a
 * mouse moves, the host's is the one a finger drags. Ask this before drawing
 * one, and do not draw one at all when a real mouse or trackpad is attached. */
int  w32_cursor_visible(void);
void w32_cursor_pos(int *x, int *y);
/* Is this virtual key held? So xinput.c can let a keyboard stand in for a
 * gamepad without a second copy of the key state. */
int  w32_key_down(int vk);

/* xinput.c: a gamepad, from the host.
 *
 * `present` is 0 when nothing is attached, which is not the same as a pad
 * reading zeros -- a game uses the difference to decide whether to show
 * controller prompts at all. Passing NULL to w32_pad_state goes back to
 * letting the keyboard stand in. */
typedef struct {
    uint16_t buttons;          /* XINPUT_GAMEPAD_* */
    uint8_t  lt, rt;           /* triggers, 0..255 */
    int16_t  lx, ly, rx, ry;   /* sticks, -32768..32767, y up */
    int      present;
} w32_pad;
void w32_pad_state(const w32_pad *p);
void w32_xinput_reset(void);
void w32_dsound_reset(void);

/* d3d9.c: where a presented frame goes. NULL simply drops it, which is what
 * the headless test and CI want; the iOS app sets it to a Metal blit. */
typedef void (*w32_present_fn)(void *ctx, const void *pixels, int width, int height, int pitch);
void w32_set_present(w32_present_fn fn, void *ctx);
w32_present_fn w32_get_present(void **ctx);        /* so a second consumer can chain */
/* The checksum of one presented frame. Shared so that winrun's -frame, the
 * qemu run and the device diagnostics are all comparing the same arithmetic
 * over the same bytes -- see d3d9.c. */
uint32_t w32_frame_crc32(const void *data, size_t n);
/* Ask a guest that is presenting frames to stop: Present and
 * TestCooperativeLevel start returning D3DERR_DEVICELOST, which a game
 * already knows how to exit on. */
void w32_d3d9_device_lost(int on);
int  w32_d3d9_lost(void);          /* d3d11's Present asks the same flag */
void w32_d3d9_reset(void);

/* raster.c: the reference rasterizer d3d9 draws through when no GPU backend
 * has taken over. Deterministic by construction (see the file), so a drawn
 * frame has one checksum everywhere. */
void w32_raster_triangle(void *target, int width, int height, int pitch,
                         const float *xy0, uint32_t c0,
                         const float *xy1, uint32_t c1,
                         const float *xy2, uint32_t c2);

/* stdcall_args.c (generated): how many bytes of arguments a stdcall function
 * pops on x86, so an unimplemented import can return without corrupting the
 * stack. -1 when the name is unknown, which is not the same as zero. */
int w32_stdcall_bytes(const char *name);

/* kernel32.c: where C:\ is on the host. NULL or "" keeps the command-line
 * behaviour (absolute guest paths become relative); the app sets it to its own
 * storage so a program copied in from Files finds its data. */
void w32_set_drive_c(const char *path);
const char *w32_drive_c(void);      /* "" when unset */

/* msvcrt.c: drop every cached guest address, so a second process can start
 * in the same host process (see winrun_main). */
void w32_reset_statics(void);

/* runtime (winrun.c) */
int w32_run(w32 *w);

/* Ask a running guest to stop. Callable from another thread -- the app's Stop
 * button -- and checked between execution slices, so it does not have to
 * interrupt anything. */
void w32_request_stop(void);
/* Something happened -- a frame was presented, input arrived. Resets the idle
 * counter that paces a PeekMessage loop, so a busy game is never slowed and an
 * idle one stops holding a core at 100%. */
void w32_note_activity(void);

/* Everything worth knowing about where a run ended: the reason, the guest's
 * registers, the instructions at RIP, the module map, and what it called that
 * we do not implement. Written whether the run crashed or merely stopped,
 * because "it exited 0 having called nine things that returned nothing" is
 * also a diagnosis. Returns the number of bytes written. */
int w32_crash_report(w32 *w, char *out, size_t out_len);

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
