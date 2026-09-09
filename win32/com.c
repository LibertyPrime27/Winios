/* COM objects the guest can call.
 *
 * Direct3D, like most of the Windows API surface a game touches, is not a set
 * of exported functions -- it is a pointer to a pointer to an array of
 * function pointers. `dev->lpVtbl->Clear(dev, ...)` compiles to an indirect
 * call through a slot the guest computed itself, so an interface is only
 * usable if the vtable lives in guest memory and every slot in it is
 * something the guest can call.
 *
 * That is exactly what the import stubs already are. A class here is a table
 * of methods in vtable order; building its vtable makes one `int3` stub per
 * slot and writes their addresses into a guest array. Calling a method traps
 * on the stub and lands in the same dispatcher an imported function does,
 * with `this` as argument 0 -- which is where the calling convention puts it
 * in both bitnesses (pushed first for x86 stdcall, rcx on x64). Slots we have
 * not implemented get a stub that names the interface and the slot number, so
 * a game reaching into an unimplemented corner produces one line rather than
 * a jump to zero.
 *
 * An object's state lives in guest memory, right after the vtable pointer and
 * the reference count. That keeps the host side stateless: nothing to free,
 * nothing to reset, and an object is exactly as valid as the guest memory it
 * sits in.
 */
#include "w32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every class whose vtable has been built. The addresses in them belong to a
 * particular process's guest memory, so winrun_reset has to forget them. */
enum { MAX_CLASSES = 32 };
static w32_com_class *g_classes[MAX_CLASSES];
static int g_nclasses;

void w32_com_reset(void) {
    for (int i = 0; i < g_nclasses; i++) g_classes[i]->vtable = 0;
    g_nclasses = 0;
}

uint64_t w32_com_vtable(w32 *w, w32_com_class *cls) {
    if (cls->vtable) return cls->vtable;
    int psz = (int)w32_ptrsize(w);
    uint64_t v = w32_alloc(w, (uint64_t)psz * (uint32_t)cls->nmethods + 16, 0);
    if (!v) { fprintf(stderr, "winrun: no memory for the %s vtable\n", cls->name); return 0; }
    cls->dll.name = cls->name;
    cls->dll.apis[0] = cls->methods;
    for (int i = 0; i < cls->nmethods; i++) {
        const w32_api *m = &cls->methods[i];
        uint64_t s;
        if (m->name) s = w32_stub_alloc(w, &cls->dll, m, 0);
        else {
            char *label = malloc(strlen(cls->name) + 24);
            sprintf(label, "%s::slot %d", cls->name, i);
            s = w32_stub_alloc(w, &cls->dll, 0, label);
        }
        w32_write(w, v + (uint64_t)psz * i, psz, s);
    }
    cls->vtable = v;
    if (g_nclasses < MAX_CLASSES) g_classes[g_nclasses++] = cls;
    if (w->verbose) fprintf(stderr, "winrun: %s vtable at %#llx, %d slots\n", cls->name, (unsigned long long)v, cls->nmethods);
    return v;
}

/* [vtable][refs][class tag][fields...]. The tag is only for QueryInterface
 * and for saying something useful when a pointer turns out to be the wrong
 * kind of object. */
uint64_t w32_com_new(w32 *w, w32_com_class *cls, uint32_t nfields) {
    int psz = (int)w32_ptrsize(w);
    uint64_t obj = w32_heap_alloc(w, (uint64_t)psz + 8 + 8ull * nfields);
    if (!obj) return 0;
    memset(W32P(w, obj), 0, (size_t)psz + 8 + 8u * nfields);
    w32_write(w, obj, psz, w32_com_vtable(w, cls));
    w32_write(w, obj + psz, 4, 1);
    w32_write(w, obj + psz + 4, 4, cls->tag);
    return obj;
}

static uint64_t field_at(w32 *w, uint64_t obj, int n) { return obj + w32_ptrsize(w) + 8 + 8ull * (uint32_t)n; }
uint64_t w32_com_get(w32 *w, uint64_t obj, int n) { return w32_read(w, field_at(w, obj, n), 8); }
void     w32_com_set(w32 *w, uint64_t obj, int n, uint64_t v) { w32_write(w, field_at(w, obj, n), 8, v); }
int      w32_com_tag(w32 *w, uint64_t obj) { return obj ? (int)w32_read(w, obj + w32_ptrsize(w) + 4, 4) : 0; }

/* IUnknown, shared by every interface. QueryInterface says yes to anything:
 * we have one object per interface and no aggregation, and a guest that asks
 * for an interface it then calls will fail on the method, not the cast --
 * which is the more useful place to fail. */
void w32_com_QueryInterface(w32 *w) {
    uint64_t self = ARG(0), out = ARG(2);
    if (out) w32_write(w, out, w32_ptrsize(w), self);
    w32_com_AddRef(w);
    RET(0);                                        /* S_OK */
}
void w32_com_AddRef(w32 *w) {
    uint64_t self = ARG(0), at = self + w32_ptrsize(w);
    uint32_t r = (uint32_t)w32_read(w, at, 4) + 1;
    w32_write(w, at, 4, r);
    RET(r);
}
void w32_com_Release(w32 *w) {
    uint64_t self = ARG(0), at = self + w32_ptrsize(w);
    uint32_t r = (uint32_t)w32_read(w, at, 4);
    if (r) r--;
    w32_write(w, at, 4, r);
    /* the guest memory stays mapped: an object's methods may still be on the
     * stack above us, and keeping a dead object readable is cheaper than the
     * class of bug that follows from unmapping one */
    RET(r);
}

/* CoCreateInstance for the classes that exist here. Each DLL that has one
 * answers for its own CLSIDs and says whether it made the object; the first
 * to say yes wins. There is deliberately no registry to keep in step with
 * the code -- a class is created by the same file that implements it. */
int w32_com_create(w32 *w, const uint8_t clsid[16], const uint8_t iid[16], uint64_t out) {
    if (w32_dsound_create_class(w, clsid, iid, out)) return 1;
    if (w32_dinput_create_class(w, clsid, iid, out)) return 1;
    if (w32_xaudio2_create_class(w, clsid, iid, out)) return 1;
    if (w32_mmdevapi_create_class(w, clsid, iid, out)) return 1;
    return 0;
}
