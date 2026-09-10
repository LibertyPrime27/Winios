/* Structured exception handling: the OS half of __try/__except.
 *
 * On 32-bit Windows this is a linked list on the stack. `__try` pushes an
 * eight-byte record -- next pointer, handler address -- and stores its address
 * at fs:[0]; `__except` pops it. When something faults, the kernel walks that
 * list from fs:[0] outward, calling each handler with a description of what
 * happened and a copy of the register state, and the first one that says "I
 * will deal with this" gets control. That is the whole protocol, and all of it
 * lives in memory the guest owns, so implementing it is a matter of walking the
 * guest's own list and calling the guest's own functions.
 *
 * Why it matters more than it looks: 32-bit MSVC compiles C++ `throw` into
 * RaiseException(0xE06D7363) and catches it through this same chain. So a game
 * built with MSVC does not need to contain a single __try for this to be on
 * its critical path -- any C++ exception, anywhere, is an SEH dispatch. It is
 * also how a program survives its own bad pointer, which is why code that
 * works on Windows dies here without it.
 *
 * What a handler is handed has to be laid out exactly right or the guest reads
 * the wrong fields: EXCEPTION_RECORD (80 bytes on x86, 152 on x64) and CONTEXT
 * (716 / 1232). Those offsets are not invented here -- they were read out of
 * mingw-w64's headers with offsetof, by a program compiled for Windows and run
 * on this emulator, which is a pleasant way to get them from the source of
 * truth rather than from memory.
 *
 * 64-bit SEH is a different mechanism -- unwind tables in the image rather
 * than a list on the stack -- and is not implemented; see the end of this file.
 */
#include "w32.h"

#include <stdio.h>
#include <string.h>

/* Exception codes a guest actually tests for. */
enum {
    EXC_ACCESS_VIOLATION   = 0xC0000005u,
    EXC_DATATYPE_MISALIGN  = 0x80000002u,
    EXC_BREAKPOINT         = 0x80000003u,
    EXC_ILLEGAL_INSTRUCTION= 0xC000001Du,
    EXC_PRIV_INSTRUCTION   = 0xC0000096u,
    EXC_INT_DIVIDE_BY_ZERO = 0xC0000094u,
    EXC_STACK_OVERFLOW     = 0xC00000FDu,
    EXC_CXX_EXCEPTION      = 0xE06D7363u,      /* MSVC C++ throw */
};
/* EXCEPTION_RECORD.ExceptionFlags */
enum { EF_NONCONTINUABLE = 1, EF_UNWINDING = 2, EF_EXIT_UNWIND = 4,
       EF_STACK_INVALID = 8, EF_NESTED_CALL = 0x10, EF_TARGET_UNWIND = 0x20,
       EF_COLLIDED_UNWIND = 0x40 };
/* What a frame handler returns. */
enum { ContinueExecution = 0, ContinueSearch = 1, NestedException = 2, CollidedUnwind = 3 };
/* What a filter returns. */
enum { FILTER_EXECUTE_HANDLER = 1, FILTER_CONTINUE_SEARCH = 0, FILTER_CONTINUE_EXECUTION = -1 };

/* CONTEXT, from mingw-w64's headers. */
enum {
    CTX32_SIZE = 716, CTX32_FLAGS = 0x00, CTX32_FLOATSAVE = 0x1C,
    CTX32_GS = 140, CTX32_FS = 144, CTX32_ES = 148, CTX32_DS = 152,
    CTX32_EDI = 156, CTX32_ESI = 160, CTX32_EBX = 164, CTX32_EDX = 168,
    CTX32_ECX = 172, CTX32_EAX = 176, CTX32_EBP = 180, CTX32_EIP = 184,
    CTX32_CS = 188, CTX32_EFLAGS = 192, CTX32_ESP = 196, CTX32_SS = 200,
    CTX32_EXTENDED = 204,
    CTX32_FULL = 0x00010007,                  /* CONTEXT_i386 | CONTROL | INTEGER | SEGMENTS */

    CTX64_SIZE = 1232, CTX64_FLAGS = 0x30, CTX64_MXCSR = 0x34,
    CTX64_CS = 0x38, CTX64_SS = 0x42, CTX64_EFLAGS = 0x44,
    CTX64_RAX = 0x78,                          /* then rcx rdx rbx rsp rbp rsi rdi r8..r15, 8 apart */
    CTX64_RIP = 0xF8, CTX64_FLTSAVE = 0x100, CTX64_XMM0 = 0x1A0,
    CTX64_FULL = 0x0010000B,                  /* CONTEXT_AMD64 | CONTROL | INTEGER | FLOATING_POINT */
};
/* EXCEPTION_RECORD */
enum {
    ER32_SIZE = 80,  ER32_CODE = 0, ER32_FLAGS = 4, ER32_REC = 8,  ER32_ADDR = 12, ER32_NPARAM = 16, ER32_INFO = 20,
    ER64_SIZE = 152, ER64_CODE = 0, ER64_FLAGS = 4, ER64_REC = 8,  ER64_ADDR = 16, ER64_NPARAM = 24, ER64_INFO = 32,
};
enum { EXC_MAXIMUM_PARAMETERS = 15 };

/* CONTEXT.Rax and the fourteen that follow it are in x86-64 register-number
 * order except that rsp and rbp are swapped relative to xc's XC_R* -- so the
 * mapping is written out rather than computed. */
static const int CTX64_ORDER[16] = {
    XC_RAX, XC_RCX, XC_RDX, XC_RBX, XC_RSP, XC_RBP, XC_RSI, XC_RDI,
    XC_R8,  XC_R9,  XC_R10, XC_R11, XC_R12, XC_R13, XC_R14, XC_R15,
};

/* --- state --------------------------------------------------------------- */

enum { MAX_VEH = 16 };
static uint64_t g_filter;                      /* SetUnhandledExceptionFilter */
static uint64_t g_veh[MAX_VEH];                /* vectored handlers, first to last */
static int g_nveh;
static int g_depth;                            /* dispatches in flight, to stop runaway recursion */
static uint32_t g_last_code;                   /* what ended the run, for the report */
static uint64_t g_last_addr;

/* 64-bit state; see the table-driven section below. */
enum { MAX_DYNTAB = 32 };
static struct { uint64_t table, base; uint32_t count; } g_dyntab[MAX_DYNTAB];   /* RtlAddFunctionTable: a JIT's frames */
static int g_ndyntab;
static uint64_t g_seh_target;             /* the unwind a handler asked for, waiting for the host frames to drop */
static uint64_t g_resume_ctx[8];          /* the exception contexts the dispatches in progress work with */
static int g_nresume;
static int dispatch64(w32 *w, uint64_t rec, uint64_t ctx);

void w32_seh_reset(void) {
    g_filter = 0; g_nveh = 0; g_depth = 0; g_last_code = 0; g_last_addr = 0;
    g_ndyntab = 0; g_seh_target = 0; g_nresume = 0;
}

/* --- CONTEXT ------------------------------------------------------------- */

/* The x87 area a handler may look at. The register file is stored the way
 * FSAVE stores it -- eight ten-byte entries from ST(0) -- so a guest that
 * reads it sees its own stack in its own order. */
static void save_x87(w32 *w, uint64_t fsa) {
    xc_cpu *c = w32_cpu(w);
    w32_write(w, fsa + 0, 4, c->fcw);
    w32_write(w, fsa + 4, 4, c->fsw);
    uint32_t tag = 0;
    for (int i = 0; i < 8; i++) tag |= (uint32_t)((c->ftag_empty >> i & 1) ? 3u : 0u) << (2 * i);
    w32_write(w, fsa + 8, 4, tag);
    int top = (c->fsw >> 11) & 7;
    for (int i = 0; i < 8; i++) {
        const xc_f80 *r = &c->fpr[(top + i) & 7];
        uint64_t at = fsa + 28 + 10u * (unsigned)i;
        w32_write(w, at + 0, 4, (uint32_t)r->mant);
        w32_write(w, at + 4, 4, (uint32_t)(r->mant >> 32));
        w32_write(w, at + 8, 2, r->se);
    }
}

/* Capture the CPU into a guest CONTEXT. */
void w32_context_save(w32 *w, uint64_t ctx) {
    xc_cpu *c = w32_cpu(w);
    xc_flags_sync(c);
    if (w->is32) {
        for (uint64_t o = 0; o < CTX32_SIZE; o += 4) w32_write(w, ctx + o, 4, 0);
        w32_write(w, ctx + CTX32_FLAGS, 4, CTX32_FULL);
        w32_write(w, ctx + CTX32_GS, 4, c->sreg[5]); w32_write(w, ctx + CTX32_FS, 4, c->sreg[4]);
        w32_write(w, ctx + CTX32_ES, 4, c->sreg[0]); w32_write(w, ctx + CTX32_DS, 4, c->sreg[3]);
        w32_write(w, ctx + CTX32_EDI, 4, c->gpr[XC_RDI]); w32_write(w, ctx + CTX32_ESI, 4, c->gpr[XC_RSI]);
        w32_write(w, ctx + CTX32_EBX, 4, c->gpr[XC_RBX]); w32_write(w, ctx + CTX32_EDX, 4, c->gpr[XC_RDX]);
        w32_write(w, ctx + CTX32_ECX, 4, c->gpr[XC_RCX]); w32_write(w, ctx + CTX32_EAX, 4, c->gpr[XC_RAX]);
        w32_write(w, ctx + CTX32_EBP, 4, c->gpr[XC_RBP]); w32_write(w, ctx + CTX32_EIP, 4, c->rip);
        w32_write(w, ctx + CTX32_CS, 4, c->sreg[1]); w32_write(w, ctx + CTX32_EFLAGS, 4, c->rflags);
        w32_write(w, ctx + CTX32_ESP, 4, c->gpr[XC_RSP]); w32_write(w, ctx + CTX32_SS, 4, c->sreg[2]);
        save_x87(w, ctx + CTX32_FLOATSAVE);
    } else {
        for (uint64_t o = 0; o < CTX64_SIZE; o += 8) w32_write(w, ctx + o, 8, 0);
        w32_write(w, ctx + CTX64_FLAGS, 4, CTX64_FULL);
        w32_write(w, ctx + CTX64_MXCSR, 4, c->mxcsr);
        w32_write(w, ctx + CTX64_CS, 2, c->sreg[1]); w32_write(w, ctx + CTX64_SS, 2, c->sreg[2]);
        w32_write(w, ctx + CTX64_EFLAGS, 4, c->rflags);
        for (int i = 0; i < 16; i++) w32_write(w, ctx + CTX64_RAX + 8u * (unsigned)i, 8, c->gpr[CTX64_ORDER[i]]);
        w32_write(w, ctx + CTX64_RIP, 8, c->rip);
        /* FltSave doubles as the XMM_SAVE_AREA32 a handler may read */
        w32_write(w, ctx + CTX64_FLTSAVE + 0, 2, c->fcw);
        w32_write(w, ctx + CTX64_FLTSAVE + 2, 2, c->fsw);
        w32_write(w, ctx + CTX64_FLTSAVE + 24, 4, c->mxcsr);
        for (int i = 0; i < 16; i++) {
            w32_write(w, ctx + CTX64_XMM0 + 16u * (unsigned)i + 0, 8, c->xmm[i].lo);
            w32_write(w, ctx + CTX64_XMM0 + 16u * (unsigned)i + 8, 8, c->xmm[i].hi);
        }
    }
}

/* Put a guest CONTEXT back on the CPU. A handler that returns
 * ExceptionContinueExecution has usually edited exactly one field -- Eip past
 * the instruction that faulted, or a register it has now made valid -- so
 * every field it could have edited has to be read back, not just the ones we
 * expect it to touch. */
void w32_context_load(w32 *w, uint64_t ctx) {
    xc_cpu *c = w32_cpu(w);
    if (w->is32) {
        c->gpr[XC_RDI] = (uint32_t)w32_read(w, ctx + CTX32_EDI, 4);
        c->gpr[XC_RSI] = (uint32_t)w32_read(w, ctx + CTX32_ESI, 4);
        c->gpr[XC_RBX] = (uint32_t)w32_read(w, ctx + CTX32_EBX, 4);
        c->gpr[XC_RDX] = (uint32_t)w32_read(w, ctx + CTX32_EDX, 4);
        c->gpr[XC_RCX] = (uint32_t)w32_read(w, ctx + CTX32_ECX, 4);
        c->gpr[XC_RAX] = (uint32_t)w32_read(w, ctx + CTX32_EAX, 4);
        c->gpr[XC_RBP] = (uint32_t)w32_read(w, ctx + CTX32_EBP, 4);
        c->gpr[XC_RSP] = (uint32_t)w32_read(w, ctx + CTX32_ESP, 4);
        c->rip         = (uint32_t)w32_read(w, ctx + CTX32_EIP, 4);
        c->rflags      = (uint32_t)w32_read(w, ctx + CTX32_EFLAGS, 4);
    } else {
        for (int i = 0; i < 16; i++) c->gpr[CTX64_ORDER[i]] = w32_read(w, ctx + CTX64_RAX + 8u * (unsigned)i, 8);
        c->rip    = w32_read(w, ctx + CTX64_RIP, 8);
        c->rflags = (uint32_t)w32_read(w, ctx + CTX64_EFLAGS, 4);
        c->mxcsr  = (uint32_t)w32_read(w, ctx + CTX64_MXCSR, 4);
    }
    c->lz_op = XC_LZ_NONE;                      /* rflags is now exact */
}

/* --- the dispatcher ------------------------------------------------------ */

static void rec_write(w32 *w, uint64_t rec, uint32_t code, uint32_t flags,
                      uint64_t addr, int nparams, const uint64_t *params) {
    int p = w->is32 ? 4 : 8;
    uint64_t o_code = w->is32 ? ER32_CODE : ER64_CODE, o_flags = w->is32 ? ER32_FLAGS : ER64_FLAGS;
    uint64_t o_rec = w->is32 ? ER32_REC : ER64_REC, o_addr = w->is32 ? ER32_ADDR : ER64_ADDR;
    uint64_t o_np = w->is32 ? ER32_NPARAM : ER64_NPARAM, o_info = w->is32 ? ER32_INFO : ER64_INFO;
    for (uint64_t o = 0; o < (uint64_t)(w->is32 ? ER32_SIZE : ER64_SIZE); o += 4) w32_write(w, rec + o, 4, 0);
    w32_write(w, rec + o_code, 4, code);
    w32_write(w, rec + o_flags, 4, flags);
    w32_write(w, rec + o_rec, p, 0);
    w32_write(w, rec + o_addr, p, addr);
    if (nparams > EXC_MAXIMUM_PARAMETERS) nparams = EXC_MAXIMUM_PARAMETERS;
    w32_write(w, rec + o_np, 4, (uint32_t)nparams);
    for (int i = 0; i < nparams; i++) w32_write(w, rec + o_info + (uint64_t)p * (unsigned)i, p, params[i]);
}

/* Is `p` a plausible registration record on this thread's stack? Windows
 * checks the same thing, because the list is in memory the faulting code may
 * have just corrupted -- and a corrupt list must end the dispatch, not send
 * control to an arbitrary address. */
static int frame_ok(w32 *w, uint64_t p, uint64_t prev) {
    if (p <= prev) return 0;                                   /* must move up the stack */
    if (p < w32_self()->stack_limit || p + 8 > w32_self()->stack_base) return 0;
    if (p & (w->is32 ? 3u : 7u)) return 0;
    return 1;
}

const char *w32_exception_name(uint32_t code) {
    switch (code) {
    case EXC_ACCESS_VIOLATION:    return "access violation";
    case EXC_DATATYPE_MISALIGN:   return "misaligned data";
    case EXC_BREAKPOINT:          return "breakpoint";
    case EXC_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXC_PRIV_INSTRUCTION:    return "privileged instruction";
    case EXC_INT_DIVIDE_BY_ZERO:  return "integer divide by zero";
    case EXC_STACK_OVERFLOW:      return "stack overflow";
    case EXC_CXX_EXCEPTION:       return "C++ exception";
    default:                      return "exception";
    }
}

/* Raise an exception in the guest and let the guest deal with it.
 *
 * Returns 1 if a handler took it and the CPU is set up to carry on, 0 if
 * nothing did -- in which case the caller ends the run, because that is what
 * Windows does too.
 *
 * Scratch lives on the guest stack below the current SP, where Windows also
 * puts it: a handler is allowed to hold on to the EXCEPTION_RECORD for the
 * duration of the dispatch, and a per-dispatch copy is what makes nesting
 * work.
 */
static uint64_t g_raised;                  /* every exception handed to the guest, for the report */
int w32_raise(w32 *w, uint32_t code, uint32_t flags, uint64_t exc_addr,
              int nparams, const uint64_t *params) {
    xc_cpu *c = w32_cpu(w);
    g_last_code = code; g_last_addr = exc_addr; g_raised++;
    if (g_depth >= 8) {
        fprintf(stderr, "winrun: exception %#x while dispatching seven others; giving up\n", code);
        return 0;
    }

    int p = w->is32 ? 4 : 8;
    uint64_t recsz = w->is32 ? ER32_SIZE : ER64_SIZE, ctxsz = w->is32 ? CTX32_SIZE : CTX64_SIZE;
    uint64_t sp = c->gpr[XC_RSP];
    /* Below the guest's SP, 16-aligned, with a gap so a handler that walks
     * back up the stack does not immediately land in our records. */
    uint64_t scratch = (sp - 256 - recsz - ctxsz - 2u * (uint64_t)p) & ~15ull;
    if (w->is32) scratch = (uint32_t)scratch;
    if (scratch < w32_self()->stack_limit) {
        fprintf(stderr, "winrun: no room on the guest stack to dispatch exception %#x\n", code);
        return 0;
    }
    uint64_t rec = scratch, ctx = scratch + recsz, ptrs = ctx + ctxsz;
    rec_write(w, rec, code, flags, exc_addr, nparams, params);
    w32_context_save(w, ctx);
    w32_write(w, ptrs + 0, p, rec);                            /* EXCEPTION_POINTERS */
    w32_write(w, ptrs + (uint64_t)p, p, ctx);

    uint64_t saved_rsp = c->gpr[XC_RSP], saved_rip = c->rip;
    c->gpr[XC_RSP] = scratch;                                  /* handlers run below our records */
    g_depth++;

    int handled = 0;

    /* Vectored handlers run before the frame list, and before any of the
     * guest's own __try blocks -- that is the point of them. */
    for (int i = 0; i < g_nveh && !handled; i++) {
        uint64_t args[1] = { ptrs };
        int64_t r = (int32_t)w32_call_guest(w, g_veh[i], 1, args);
        if (w->exited) { g_depth--; return 1; }
        if (r == FILTER_CONTINUE_EXECUTION) handled = 1;
    }

    /* 64-bit: the unwind tables. */
    if (!handled && !w->is32) handled = dispatch64(w, rec, ctx);

    /* The frame list: 32-bit only; 64-bit Windows does not have one. */
    if (!handled && w->is32) {
        uint64_t frame = w32_read(w, w32_self()->teb + 0, 4), prev = 0;
        while (frame && frame != 0xFFFFFFFFu) {
            if (!frame_ok(w, frame, prev)) {
                fprintf(stderr, "winrun: exception registration list is corrupt at %#llx "
                                "(stack is %#llx..%#llx)\n", (unsigned long long)frame,
                        (unsigned long long)w32_self()->stack_limit, (unsigned long long)w32_self()->stack_base);
                break;
            }
            uint64_t next = w32_read(w, frame + 0, 4);
            uint64_t handler = w32_read(w, frame + 4, 4);
            if (w->verbose > 1) fprintf(stderr, "winrun: seh: frame %#llx handler %#llx\n",
                                        (unsigned long long)frame, (unsigned long long)handler);
            /* handler(ExceptionRecord, EstablisherFrame, ContextRecord, DispatcherContext) */
            uint64_t args[4] = { rec, frame, ctx, ptrs };
            uint64_t r = (uint32_t)w32_call_guest(w, handler, 4, args);
            if (w->exited) { g_depth--; return 1; }
            if (r == ContinueExecution) { handled = 1; break; }
            if (r == NestedException || r == CollidedUnwind) {
                /* the handler faulted while handling; Windows would build a
                 * nested record. Nothing here can do better than say so. */
                fprintf(stderr, "winrun: seh: handler at %#llx reported a nested exception\n",
                        (unsigned long long)handler);
                break;
            }
            prev = frame; frame = next;                        /* ContinueSearch */
        }
    }

    /* Nobody wanted it. The unhandled filter is a program's last chance to
     * write a crash log of its own, and games do exactly that. */
    if (!handled && g_filter) {
        uint64_t args[1] = { ptrs };
        int64_t r = (int32_t)w32_call_guest(w, g_filter, 1, args);
        if (w->exited) { g_depth--; return 1; }
        if (r == FILTER_CONTINUE_EXECUTION) handled = 1;
    }

    g_depth--;
    if (handled) {
        /* a handler that unwound left the landing pad's context for us */
        if (g_seh_target) { uint64_t t = g_seh_target; g_seh_target = 0; w32_context_load(w, t); }
        else w32_context_load(w, ctx);
        return 1;
    }
    c->gpr[XC_RSP] = saved_rsp; c->rip = saved_rip;
    return 0;
}

/* What ended the run, for the crash report. */
uint32_t w32_last_exception(uint64_t *addr) {
    if (addr) *addr = g_last_addr;
    return g_last_code;
}
uint64_t w32_exceptions_raised(void) { return g_raised; }

/* A CPU fault becomes the exception Windows would have raised for it. */
int w32_fault_to_exception(w32 *w) {
    xc_cpu *c = w32_cpu(w);
    if (c->fault_kind == XC_FAULT_DIVIDE) return w32_raise(w, EXC_INT_DIVIDE_BY_ZERO, 0, c->rip, 0, 0);
    /* ExceptionInformation for an access violation: [0] is 0 for a read, 1
     * for a write, 8 for an execute; [1] is the address. We do not yet know
     * which of the three it was, and reporting a read we are not sure about
     * would be worse than reporting the address alone -- so the flag says
     * read, which is what a guest logging the address will print correctly. */
    uint64_t params[2] = { 0, c->fault_addr };
    uint32_t code = EXC_ACCESS_VIOLATION;
    /* An access just past the far end of the guest stack is a stack overflow,
     * and a guest that has a handler for one wants to be told that rather
     * than "access violation" -- the recovery is different. */
    if (w32_self()->stack_limit && c->fault_addr + 0x10000 >= w32_self()->stack_limit && c->fault_addr < w32_self()->stack_limit)
        code = EXC_STACK_OVERFLOW;
    return w32_raise(w, code, 0, c->rip, code == EXC_STACK_OVERFLOW ? 0 : 2, params);
}


/* --- 64-bit: table-driven unwinding ---------------------------------------
 *
 * x86-64 Windows keeps no list on the stack. The compiler emits, for every
 * function, a RUNTIME_FUNCTION in .pdata (begin, end, unwind info) and an
 * UNWIND_INFO in .xdata describing its prologue as a list of codes; the
 * dispatcher looks the faulting RIP up, plays the codes backwards to recover
 * the caller's registers, and calls the language handler the entry names.
 * That is RtlLookupFunctionEntry and RtlVirtualUnwind, and the two loops
 * built on them: dispatch (search for a handler that wants it) and
 * RtlUnwindEx (run the cleanups and land on the target).
 *
 * Two things about running this in an emulator. A handler is called through
 * w32_call_guest, whose return address is a host stub that is in no module;
 * when a walk started inside a handler reaches it, it continues from the
 * exception context the dispatch was working with -- which is what Windows'
 * own dispatcher frame does, with a machine frame. And a handler that unwinds
 * to a landing pad does not longjmp over the host: RtlUnwindEx loads the
 * target context, notes it, and returns to the host through the same stub, so
 * the dispatcher's host frames unwind too. Each caught exception costs no
 * host stack afterwards.
 */
enum { UNW_FLAG_EHANDLER = 1, UNW_FLAG_UHANDLER = 2, UNW_FLAG_CHAININFO = 4 };
enum { UWOP_PUSH_NONVOL = 0, UWOP_ALLOC_LARGE, UWOP_ALLOC_SMALL, UWOP_SET_FPREG, UWOP_SAVE_NONVOL, UWOP_SAVE_NONVOL_FAR,
       UWOP_EPILOG, UWOP_SPARE_CODE, UWOP_SAVE_XMM128, UWOP_SAVE_XMM128_FAR, UWOP_PUSH_MACHFRAME };
enum { DC_ControlPc = 0, DC_ImageBase = 8, DC_FunctionEntry = 16, DC_EstablisherFrame = 24, DC_TargetIp = 32,
       DC_ContextRecord = 40, DC_LanguageHandler = 48, DC_HandlerData = 56, DC_HistoryTable = 64, DC_ScopeIndex = 72, DC_SIZE = 80 };
enum { STATUS_UNWIND_ = 0xC0000027 };

/* Function tables a JIT registers at run time (RtlAddFunctionTable): Mono,
 * .NET and V8 all do, so their frames unwind like compiled ones. */

static uint64_t rd64(w32 *w, uint64_t a) { return w32_read(w, a, 8); }
static uint32_t rd32(w32 *w, uint64_t a) { return (uint32_t)w32_read(w, a, 4); }

/* RtlLookupFunctionEntry: the RUNTIME_FUNCTION covering `pc`, or 0. Modules
 * first (binary search: .pdata is sorted), then the dynamic tables. */
static uint64_t lookup_entry(w32 *w, uint64_t pc, uint64_t *base) {
    for (int i = 0; i < w->nmods; i++) {
        w32_module *m = &w->mods[i];
        if (pc < m->base || pc >= m->base + m->size || !m->pdata_size) continue;
        uint64_t tab = m->base + m->pdata_rva; uint32_t n = m->pdata_size / 12, lo = 0, hi = n;
        uint32_t rva = (uint32_t)(pc - m->base);
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2, b = rd32(w, tab + 12ull * mid), e = rd32(w, tab + 12ull * mid + 4);
            if (rva < b) hi = mid; else if (rva >= e) lo = mid + 1;
            else { *base = m->base; return tab + 12ull * mid; }
        }
        return 0;                                    /* in the module, but a leaf */
    }
    for (int i = 0; i < g_ndyntab; i++) {
        uint64_t b = g_dyntab[i].base;
        for (uint32_t k = 0; k < g_dyntab[i].count; k++) {
            uint64_t e = g_dyntab[i].table + 12ull * k;
            if (pc >= b + rd32(w, e) && pc < b + rd32(w, e + 4)) { *base = b; return e; }
        }
    }
    return 0;
}
static uint64_t ctx_reg(w32 *w, uint64_t ctx, int r) { return rd64(w, ctx + CTX64_RAX + 8u * (unsigned)r); }
static void ctx_set(w32 *w, uint64_t ctx, int r, uint64_t v) { w32_write(w, ctx + CTX64_RAX + 8u * (unsigned)r, 8, v); }

/* RtlVirtualUnwind: play one function's prologue backwards over `ctx`, so it
 * becomes the caller's context. Returns the handler if the entry has one of
 * the `want` kind, and says where the establisher frame was. */
static uint64_t virtual_unwind(w32 *w, int want, uint64_t base, uint64_t pc, uint64_t entry, uint64_t ctx,
                               uint64_t *handler_data, uint64_t *establisher) {
    uint64_t handler = 0; int chained = 0;
    *handler_data = 0; *establisher = ctx_reg(w, ctx, 4);
    for (int hop = 0; hop < 8; hop++) {
        uint64_t begin = base + rd32(w, entry), info = base + rd32(w, entry + 8);
        if (!w32_mem_ok(w, info, 4)) return 0;
        uint32_t b0 = rd32(w, info) & 0xFF, flags = b0 >> 3;
        uint32_t ncodes = (rd32(w, info) >> 16) & 0xFF, frame_reg = (rd32(w, info) >> 24) & 0xF, frame_off = (rd32(w, info) >> 28) & 0xF;
        uint64_t codes = info + 4;
        uint64_t rsp = ctx_reg(w, ctx, 4);
        if (!chained && frame_reg) *establisher = ctx_reg(w, ctx, (int)frame_reg) - 16ull * frame_off;
        uint32_t offset_in = pc >= begin ? (uint32_t)(pc - begin) : 0xFFFFFFFFu;
        int machframe = 0;
        for (uint32_t i = 0; i < ncodes; ) {
            uint32_t code = rd32(w, codes + 2ull * i) & 0xFFFF, coff = code & 0xFF, op = (code >> 8) & 0xF, opinfo = code >> 12;
            uint32_t slots = 1;
            switch (op) {
            case UWOP_ALLOC_LARGE: slots = opinfo ? 3 : 2; break;
            case UWOP_SAVE_NONVOL: case UWOP_SAVE_XMM128: case UWOP_EPILOG: slots = 2; break;
            case UWOP_SAVE_NONVOL_FAR: case UWOP_SAVE_XMM128_FAR: case UWOP_SPARE_CODE: slots = 3; break;
            default: break;
            }
            if (op == UWOP_EPILOG) slots = 1;
            /* a code the prologue has not executed yet (fault inside the prologue) does not apply */
            if (!chained && coff > offset_in) { i += slots; continue; }
            switch (op) {
            case UWOP_PUSH_NONVOL: ctx_set(w, ctx, (int)opinfo, rd64(w, rsp)); rsp += 8; break;
            case UWOP_ALLOC_LARGE:
                if (opinfo) rsp += rd32(w, codes + 2ull * (i + 1));
                else rsp += 8ull * (rd32(w, codes + 2ull * (i + 1)) & 0xFFFF);
                break;
            case UWOP_ALLOC_SMALL: rsp += 8ull * opinfo + 8; break;
            case UWOP_SET_FPREG: rsp = ctx_reg(w, ctx, (int)frame_reg) - 16ull * frame_off; break;
            case UWOP_SAVE_NONVOL: ctx_set(w, ctx, (int)opinfo, rd64(w, rsp + 8ull * (rd32(w, codes + 2ull * (i + 1)) & 0xFFFF))); break;
            case UWOP_SAVE_NONVOL_FAR: ctx_set(w, ctx, (int)opinfo, rd64(w, rsp + rd32(w, codes + 2ull * (i + 1)))); break;
            case UWOP_SAVE_XMM128: { uint64_t at = rsp + 16ull * (rd32(w, codes + 2ull * (i + 1)) & 0xFFFF);
                w32_write(w, ctx + CTX64_XMM0 + 16u * opinfo, 8, rd64(w, at)); w32_write(w, ctx + CTX64_XMM0 + 16u * opinfo + 8, 8, rd64(w, at + 8)); break; }
            case UWOP_SAVE_XMM128_FAR: { uint64_t at = rsp + rd32(w, codes + 2ull * (i + 1));
                w32_write(w, ctx + CTX64_XMM0 + 16u * opinfo, 8, rd64(w, at)); w32_write(w, ctx + CTX64_XMM0 + 16u * opinfo + 8, 8, rd64(w, at + 8)); break; }
            case UWOP_PUSH_MACHFRAME:
                w32_write(w, ctx + CTX64_RIP, 8, rd64(w, rsp + (opinfo ? 8 : 0)));
                rsp = rd64(w, rsp + (opinfo ? 32 : 24)); machframe = 1; break;
            default: break;
            }
            i += slots;
        }
        ctx_set(w, ctx, 4, rsp);
        if ((flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) && !handler) {
            uint64_t after = codes + 2ull * ((ncodes + 1) & ~1u);
            if (flags & want) { handler = base + rd32(w, after); *handler_data = after + 4; }
        }
        if (flags & UNW_FLAG_CHAININFO) {
            entry = codes + 2ull * ((ncodes + 1) & ~1u);      /* a RUNTIME_FUNCTION follows the codes */
            chained = 1; continue;
        }
        if (!machframe) { w32_write(w, ctx + CTX64_RIP, 8, rd64(w, rsp)); ctx_set(w, ctx, 4, rsp + 8); }   /* the return address */
        break;
    }
    return handler;
}
static void ctx_copy(w32 *w, uint64_t dst, uint64_t src) {
    void *d = W32PN(w, dst, CTX64_SIZE); const void *s = W32PN(w, src, CTX64_SIZE);
    if (d && s) memcpy(d, s, CTX64_SIZE);
}
static int frame_in_stack(w32 *w, uint64_t f) {
    return f >= w32_self()->stack_limit && f < w32_self()->stack_base && !(f & 7);
}
/* A walk that meets one of our return stubs: the frame above it is host code,
 * and above that the exception the dispatch was working with. */
static int cross_host_frame(w32 *w, uint64_t uctx) {
    uint64_t rip = rd64(w, uctx + CTX64_RIP);
    if (rip != w32_stub_return_addr(w) || !g_nresume) return 0;
    ctx_copy(w, uctx, g_resume_ctx[g_nresume - 1]);
    return rd64(w, uctx + CTX64_RIP) != rip;                     /* a resume point that is itself the stub would loop */
}
static void dc_write(w32 *w, uint64_t dc, uint64_t pc, uint64_t base, uint64_t entry, uint64_t frame, uint64_t target_ip,
                     uint64_t ctx, uint64_t handler, uint64_t hdata) {
    w32_write(w, dc + DC_ControlPc, 8, pc); w32_write(w, dc + DC_ImageBase, 8, base); w32_write(w, dc + DC_FunctionEntry, 8, entry);
    w32_write(w, dc + DC_EstablisherFrame, 8, frame); w32_write(w, dc + DC_TargetIp, 8, target_ip); w32_write(w, dc + DC_ContextRecord, 8, ctx);
    w32_write(w, dc + DC_LanguageHandler, 8, handler); w32_write(w, dc + DC_HandlerData, 8, hdata); w32_write(w, dc + DC_HistoryTable, 8, 0);
    w32_write(w, dc + DC_ScopeIndex, 4, 0); w32_write(w, dc + DC_ScopeIndex + 4, 4, 0);
}

/* The search: walk from the exception context, calling each frame's exception
 * handler until one continues execution, or unwinds (g_seh_target), or the
 * stack runs out. `ctx` is the record the handlers see; the walk uses a copy. */
static int dispatch64(w32 *w, uint64_t rec, uint64_t ctx) {
    xc_cpu *c = w32_cpu(w);
    uint64_t sp = c->gpr[XC_RSP];
    uint64_t uctx = (sp - 2 * CTX64_SIZE - DC_SIZE - 64) & ~15ull, dc = uctx + CTX64_SIZE, resume = dc + DC_SIZE;
    if (uctx < w32_self()->stack_limit) return 0;
    ctx_copy(w, uctx, ctx);
    /* A private copy is what a walk resumes from past our host frame. The
     * record the handlers see (`ctx`) is theirs to scribble on -- libgcc
     * passes it to RtlUnwindEx as scratch -- so it cannot also be the anchor. */
    ctx_copy(w, resume, ctx);
    c->gpr[XC_RSP] = uctx - 64;                                  /* handlers run below all of it */
    if (g_nresume < 8) g_resume_ctx[g_nresume++] = resume;
    int handled = 0;
    for (int steps = 0; steps < 4096 && !handled; steps++) {
        uint64_t pc = rd64(w, uctx + CTX64_RIP), base = 0;
        uint64_t entry = lookup_entry(w, pc, &base);
        if (!entry) {
            if (cross_host_frame(w, uctx)) continue;
            uint64_t rsp = ctx_reg(w, uctx, 4);                   /* a leaf: the return address is at RSP */
            if (!frame_in_stack(w, rsp)) break;
            w32_write(w, uctx + CTX64_RIP, 8, rd64(w, rsp)); ctx_set(w, uctx, 4, rsp + 8);
            continue;
        }
        uint64_t hdata = 0, frame = 0;
        uint64_t handler = virtual_unwind(w, UNW_FLAG_EHANDLER, base, pc, entry, uctx, &hdata, &frame);
        if (!frame_in_stack(w, frame)) break;
        if (handler) {
            dc_write(w, dc, pc, base, entry, frame, 0, ctx, handler, hdata);
            if (w->verbose > 1) fprintf(stderr, "winrun: seh64: frame %#llx handler %#llx for pc %#llx\n", (unsigned long long)frame, (unsigned long long)handler, (unsigned long long)pc);
            uint64_t args[4] = { rec, frame, ctx, dc };
            uint64_t r = (uint32_t)w32_call_guest(w, handler, 4, args);
            if (w->exited) break;
            if (g_seh_target) { handled = 1; break; }             /* it unwound to a landing pad */
            if (r == ContinueExecution) { handled = 1; break; }
            if (r != ContinueSearch) {
                fprintf(stderr, "winrun: seh64: handler at %#llx returned %llu\n", (unsigned long long)handler, (unsigned long long)r);
                break;
            }
        }
    }
    g_nresume--;
    return handled;
}

/* RtlUnwindEx(TargetFrame, TargetIp, ExceptionRecord, ReturnValue, ContextRecord, HistoryTable).
 * Walks from the caller, giving every frame's unwind handler its say, until
 * the establisher frame is TargetFrame; lands there with RIP = TargetIp and
 * RAX = ReturnValue. Never returns to its caller. */
static void unwind64(w32 *w, uint64_t target_frame, uint64_t target_ip, uint64_t urec, uint64_t retval, uint64_t ctx_arg) {
    xc_cpu *c = w32_cpu(w);
    uint64_t saved_rsp = c->gpr[XC_RSP];
    uint64_t sp = c->gpr[XC_RSP];
    uint64_t scratch = (sp - 3 * CTX64_SIZE - DC_SIZE - ER64_SIZE - 128) & ~15ull;
    if (scratch < w32_self()->stack_limit) { fprintf(stderr, "winrun: RtlUnwindEx: no stack for the unwind\n"); w32_exit(w, 129); return; }
    uint64_t ctx = ctx_arg && w32_mem_ok(w, ctx_arg, CTX64_SIZE) ? ctx_arg : scratch;
    uint64_t next = scratch + CTX64_SIZE, dc = next + CTX64_SIZE, rec = urec ? urec : dc + DC_SIZE, resume = dc + DC_SIZE + ER64_SIZE;
    /* the caller's context: RtlUnwindEx returned to it, as far as the walk is concerned */
    { uint64_t ret = rd64(w, c->gpr[XC_RSP]); uint64_t rip0 = c->rip, rsp0 = c->gpr[XC_RSP];
      c->rip = ret; c->gpr[XC_RSP] = rsp0 + 8; w32_context_save(w, ctx); c->rip = rip0; c->gpr[XC_RSP] = rsp0; }
    if (!urec) rec_write(w, rec, STATUS_UNWIND_, EF_UNWINDING | (target_frame ? 0 : EF_EXIT_UNWIND), rd64(w, ctx + CTX64_RIP), 0, 0);
    else w32_write(w, rec + ER64_FLAGS, 4, rd32(w, rec + ER64_FLAGS) | (uint32_t)EF_UNWINDING | (target_frame ? 0 : (uint32_t)EF_EXIT_UNWIND));
    c->gpr[XC_RSP] = scratch - 64;
    int found = 0;
    for (int steps = 0; steps < 4096; steps++) {
        uint64_t pc = rd64(w, ctx + CTX64_RIP), base = 0;
        uint64_t entry = lookup_entry(w, pc, &base);
        if (w->verbose > 1) fprintf(stderr, "winrun: unwind64: pc %#llx rsp %#llx entry %#llx%s\n", (unsigned long long)pc, (unsigned long long)ctx_reg(w, ctx, 4), (unsigned long long)entry, pc == w32_stub_return_addr(w) ? "  (host stub)" : "");
        if (!entry) {
            if (cross_host_frame(w, ctx)) continue;
            uint64_t rsp = ctx_reg(w, ctx, 4);
            if (!frame_in_stack(w, rsp)) { if (w->verbose) fprintf(stderr, "winrun: unwind64: leaf with rsp %#llx outside the stack; stopping\n", (unsigned long long)rsp); break; }
            w32_write(w, ctx + CTX64_RIP, 8, rd64(w, rsp)); ctx_set(w, ctx, 4, rsp + 8);
            continue;
        }
        ctx_copy(w, next, ctx);
        uint64_t hdata = 0, frame = 0;
        uint64_t handler = virtual_unwind(w, UNW_FLAG_UHANDLER, base, pc, entry, next, &hdata, &frame);
        if (w->verbose > 1) fprintf(stderr, "winrun: unwind64:   frame %#llx handler %#llx -> pc %#llx rsp %#llx\n", (unsigned long long)frame, (unsigned long long)handler, (unsigned long long)rd64(w, next + CTX64_RIP), (unsigned long long)ctx_reg(w, next, 4));
        if (!frame_in_stack(w, frame)) { if (w->verbose) fprintf(stderr, "winrun: unwind64: frame %#llx outside the stack; stopping\n", (unsigned long long)frame); break; }
        if (target_frame && frame > target_frame) { fprintf(stderr, "winrun: RtlUnwindEx: passed the target frame %#llx at %#llx\n", (unsigned long long)target_frame, (unsigned long long)frame); break; }
        if (handler) {
            uint32_t fl = rd32(w, rec + ER64_FLAGS);
            if (frame == target_frame) w32_write(w, rec + ER64_FLAGS, 4, fl | EF_TARGET_UNWIND);
            dc_write(w, dc, pc, base, entry, frame, target_ip, ctx, handler, hdata);
            ctx_copy(w, resume, ctx);
            if (g_nresume < 8) g_resume_ctx[g_nresume++] = resume;
            uint64_t args[4] = { rec, frame, ctx, dc };
            uint64_t r = (uint32_t)w32_call_guest(w, handler, 4, args);
            g_nresume--;
            w32_write(w, rec + ER64_FLAGS, 4, fl);
            if (w->exited) return;
            if (g_seh_target) return;                            /* a nested unwind took over */
            if (r != ContinueSearch) fprintf(stderr, "winrun: RtlUnwindEx: handler at %#llx returned %llu\n", (unsigned long long)handler, (unsigned long long)r);
        }
        if (frame == target_frame) { found = 1; break; }
        ctx_copy(w, ctx, next);
    }
    if (!found && target_frame) {
        fprintf(stderr, "winrun: RtlUnwindEx: target frame %#llx not found on the stack\n", (unsigned long long)target_frame);
        c->gpr[XC_RSP] = saved_rsp;
        w->stop_reason = "an unhandled exception"; w32_exit(w, 129); return;
    }
    ctx_set(w, ctx, 0, retval);
    if (target_ip) w32_write(w, ctx + CTX64_RIP, 8, target_ip);
    if (g_depth > 0) {
        /* inside a dispatch: hand the target to it and let the host frames unwind */
        g_seh_target = ctx;
        w32_return_to_host(w);
    } else {
        w32_context_load(w, ctx);
        w->redirected = 1;
    }
}

/* __C_specific_handler(rec, frame, ctx, dc): MSVC's __try. The scope table in
 * the handler data lists, per __try, its range, its filter (or 1 for "always")
 * and its __except body -- or, with no body, a __finally. */
void w32_C_specific_handler(w32 *w) {
    uint64_t rec = ARG(0), frame = ARG(1), ctx = ARG(2), dc = ARG(3);
    uint64_t base = rd64(w, dc + DC_ImageBase), table = rd64(w, dc + DC_HandlerData);
    uint64_t pc = rd64(w, dc + DC_ControlPc) - base;
    uint32_t flags = rd32(w, rec + ER64_FLAGS), count = rd32(w, table), scope0 = rd32(w, dc + DC_ScopeIndex);
    if (count > 4096 || !w32_mem_ok(w, table, 4 + 16ull * count)) { RET(ContinueSearch); return; }
    if (flags & (EF_UNWINDING | EF_EXIT_UNWIND)) {
        uint64_t target = rd64(w, dc + DC_TargetIp) - base;
        for (uint32_t i = scope0; i < count; i++) {
            uint64_t e = table + 4 + 16ull * i;
            uint32_t b = rd32(w, e), en = rd32(w, e + 4), h = rd32(w, e + 8), jt = rd32(w, e + 12);
            if (pc < b || pc >= en || jt) continue;               /* not this scope, or a __try/__except */
            if ((flags & EF_TARGET_UNWIND) && target >= b && target < en) break;   /* landing inside it: leave it be */
            w32_write(w, dc + DC_ScopeIndex, 4, i + 1);
            uint64_t args[2] = { 1, frame };                       /* __finally(abnormal = TRUE, frame) */
            w32_call_guest(w, base + h, 2, args);
            if (w->exited || g_seh_target) { RET(ContinueSearch); return; }
        }
        RET(ContinueSearch); return;
    }
    for (uint32_t i = scope0; i < count; i++) {
        uint64_t e = table + 4 + 16ull * i;
        uint32_t b = rd32(w, e), en = rd32(w, e + 4), h = rd32(w, e + 8), jt = rd32(w, e + 12);
        if (pc < b || pc >= en || !jt) continue;
        int64_t r = 1;
        if (h != 1) {                                             /* a filter expression; 1 means EXCEPTION_EXECUTE_HANDLER outright */
            xc_cpu *c = w32_cpu(w);
            uint64_t ptrs = (c->gpr[XC_RSP] - 32) & ~15ull;
            w32_write(w, ptrs, 8, rec); w32_write(w, ptrs + 8, 8, ctx);
            uint64_t saved = c->gpr[XC_RSP]; c->gpr[XC_RSP] = ptrs - 16;
            uint64_t args[2] = { ptrs, frame };
            r = (int32_t)w32_call_guest(w, base + h, 2, args);
            c->gpr[XC_RSP] = saved;
            if (w->exited || g_seh_target) { RET(ContinueSearch); return; }
        }
        if (r < 0) { RET(ContinueExecution); return; }
        if (r > 0) {
            unwind64(w, frame, base + jt, rec, rd32(w, rec + ER64_CODE), ctx);
            RET(ContinueSearch); return;                          /* not reached as a return the guest sees */
        }
    }
    RET(ContinueSearch);
}

/* --- the 64-bit exports ------------------------------------------------------ */
static void k_RtlLookupFunctionEntry(w32 *w) {
    uint64_t base = 0, e = w->is32 ? 0 : lookup_entry(w, ARG(0), &base);
    if (ARG(1)) w32_write(w, ARG(1), 8, base);
    RET(e);
}
/* RtlVirtualUnwind(HandlerType, ImageBase, ControlPc, FunctionEntry, Context, *HandlerData, *EstablisherFrame, ContextPointers) */
static void k_RtlVirtualUnwind(w32 *w) {
    if (w->is32 || !ARG(3) || !ARG(4) || !w32_mem_ok(w, ARG(4), CTX64_SIZE)) { RET(0); return; }
    uint64_t hdata = 0, frame = 0;
    uint64_t h = virtual_unwind(w, (int)ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), &hdata, &frame);
    if (ARG(5)) w32_write(w, ARG(5), 8, hdata);
    if (ARG(6)) w32_write(w, ARG(6), 8, frame);
    RET(h);
}
static void k_RtlUnwindEx(w32 *w) {
    if (w->is32) { RET(0); return; }
    unwind64(w, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4));
}
static void k_RtlPcToFileHeader(w32 *w) {
    uint64_t pc = ARG(0), base = 0;
    for (int i = 0; i < w->nmods; i++) if (pc >= w->mods[i].base && pc < w->mods[i].base + w->mods[i].size) base = w->mods[i].base;
    if (!base) base = w32_module_handle(w, "kernel32.dll") == pc ? pc : 0;
    if (ARG(1)) w32_write(w, ARG(1), (int)w32_ptrsize(w), base);
    RET(base);
}
/* RtlAddFunctionTable(FunctionTable, EntryCount, BaseAddress) -- a JIT's frames. */
static void k_RtlAddFunctionTable(w32 *w) {
    if (w->is32 || g_ndyntab >= MAX_DYNTAB || !ARG(0)) { RET(0); return; }
    g_dyntab[g_ndyntab].table = ARG(0); g_dyntab[g_ndyntab].count = (uint32_t)ARG(1); g_dyntab[g_ndyntab].base = ARG(2);
    g_ndyntab++;
    if (w->verbose) fprintf(stderr, "winrun: RtlAddFunctionTable: %u entries at %#llx, base %#llx\n", (unsigned)ARG(1), (unsigned long long)ARG(0), (unsigned long long)ARG(2));
    RET(1);
}
static void k_RtlDeleteFunctionTable(w32 *w) {
    for (int i = 0; i < g_ndyntab; i++) if (g_dyntab[i].table == ARG(0)) { g_dyntab[i] = g_dyntab[--g_ndyntab]; RET(1); return; }
    RET(0);
}
static void k_RtlInstallFunctionTableCallback(w32 *w) {
    w32_note_refused(w, "ntdll!RtlInstallFunctionTableCallback (a callback-supplied unwind table; tables handed over whole do work)");
    RET(0);
}
static void k_RtlGrowFunctionTable(w32 *w) { (void)w; RET(0); }
static void k___C_specific_handler(w32 *w) { w32_C_specific_handler(w); }

/* --- the API ------------------------------------------------------------- */

/* RaiseException(code, flags, nparams, params).
 *
 * A software exception has no faulting instruction, so the address reported --
 * and the Eip a handler that continues execution would return to -- is the
 * instruction after the call. Windows reports an address inside
 * kernel32!RaiseException instead, which is of no use to anyone; the call site
 * is what a crash log wants. */
static void k_RaiseException(w32 *w) {
    uint32_t code = (uint32_t)ARG(0), flags = (uint32_t)ARG(1);
    uint32_t n = (uint32_t)ARG(2);
    uint64_t pp = ARG(3);
    int p = w->is32 ? 4 : 8;
    uint64_t params[EXC_MAXIMUM_PARAMETERS];
    if (n > EXC_MAXIMUM_PARAMETERS) n = EXC_MAXIMUM_PARAMETERS;
    for (uint32_t i = 0; i < n; i++) params[i] = pp ? w32_read(w, pp + (uint64_t)p * i, p) : 0;

    xc_cpu *c = w32_cpu(w);
    uint64_t ret_addr = w32_read(w, c->gpr[XC_RSP], p);
    /* Return to the caller *first*, so the state a handler sees is the state
     * after RaiseException returned -- which is what continuing execution has
     * to mean, and what a stack walk in the handler expects to see. */
    uint64_t argbytes = w->is32 ? 4u + 4u * 4u : 8u;
    c->rip = ret_addr;
    c->gpr[XC_RSP] = w->is32 ? (uint32_t)(c->gpr[XC_RSP] + argbytes) : c->gpr[XC_RSP] + argbytes;

    if (!w32_raise(w, code, flags, ret_addr, (int)n, params)) {
        fprintf(stderr, "winrun: unhandled %s (%#x) raised at %#llx\n",
                w32_exception_name(code), code, (unsigned long long)ret_addr);
        w->stop_reason = "an unhandled exception";
        w32_exit(w, 129);
        return;
    }
    /* Set only now: a handler almost certainly called something of its own,
     * and each of those calls came through the stub dispatcher, which would
     * have consumed this flag and then returned for us anyway. */
    w->redirected = 1;
}

/* RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue).
 *
 * Called from inside a handler that has decided to run its __except block: it
 * pops the registration list down to TargetFrame, giving every handler it
 * passes a chance to run its __finally blocks (that is what EXCEPTION_UNWINDING
 * means). Control then returns to the caller, which jumps to its own handler
 * body -- MSVC's __global_unwind2 puts the label it wants immediately after
 * the call, so returning normally lands where TargetIp points. */
static void k_RtlUnwind(w32 *w) {
    if (!w->is32) { unwind64(w, ARG(0), ARG(1), ARG(2), ARG(3), 0); return; }
    uint64_t target = ARG(0);
    uint64_t urec = ARG(2);
    uint64_t frame = w32_read(w, w32_self()->teb + 0, 4), prev = 0;
    xc_cpu *c = w32_cpu(w);
    uint64_t saved_rsp = c->gpr[XC_RSP], saved_rip = c->rip;

    /* An EXCEPTION_RECORD for the unwind, if the caller did not supply one */
    uint64_t rec = urec;
    uint64_t scratch = 0;
    if (!rec) {
        scratch = ((c->gpr[XC_RSP] - 128 - ER32_SIZE) & ~15ull);
        rec = (uint32_t)scratch;
        rec_write(w, rec, 0xC0000027u /* STATUS_UNWIND */, EF_UNWINDING, c->rip, 0, 0);
    } else {
        w32_write(w, rec + ER32_FLAGS, 4, w32_read(w, rec + ER32_FLAGS, 4) | (uint32_t)EF_UNWINDING);
    }

    while (frame && frame != 0xFFFFFFFFu && frame != target) {
        if (!frame_ok(w, frame, prev)) break;
        uint64_t next = w32_read(w, frame + 0, 4);
        uint64_t handler = w32_read(w, frame + 4, 4);
        uint64_t args[4] = { rec, frame, 0, 0 };
        w32_call_guest(w, handler, 4, args);
        if (w->exited) return;
        /* pop as we go, so a handler that faults does not see a frame that
         * has already been unwound */
        w32_write(w, w32_self()->teb + 0, 4, next);
        prev = frame; frame = next;
    }
    if (target && target != 0xFFFFFFFFu) w32_write(w, w32_self()->teb + 0, 4, target);
    c->gpr[XC_RSP] = saved_rsp; c->rip = saved_rip;
    RET(0);
}

static void k_SetUnhandledExceptionFilter(w32 *w) {
    uint64_t prev = g_filter;
    g_filter = ARG(0);
    RET(prev);
}
/* A program calls this from inside its own filter to get the default
 * behaviour. There is no debugger to offer and no dialog to show, so the
 * answer is "run your handler". */
static void k_UnhandledExceptionFilter(w32 *w) { (void)w; RET(FILTER_EXECUTE_HANDLER); }

static void k_AddVectoredExceptionHandler(w32 *w) {
    uint32_t first = (uint32_t)ARG(0);
    uint64_t h = ARG(1);
    if (!h || g_nveh >= MAX_VEH) { RET(0); return; }
    if (first) {
        for (int i = g_nveh; i > 0; i--) g_veh[i] = g_veh[i - 1];
        g_veh[0] = h;
    } else g_veh[g_nveh] = h;
    g_nveh++;
    RET(h);                                     /* the handle is the handler itself */
}
static void k_RemoveVectoredExceptionHandler(w32 *w) {
    uint64_t h = ARG(0);
    for (int i = 0; i < g_nveh; i++) if (g_veh[i] == h) {
        for (int k = i; k + 1 < g_nveh; k++) g_veh[k] = g_veh[k + 1];
        g_nveh--; RET(1); return;
    }
    RET(0);
}
static void k_AddVectoredContinueHandler(w32 *w) { k_AddVectoredExceptionHandler(w); }
static void k_RemoveVectoredContinueHandler(w32 *w) { k_RemoveVectoredExceptionHandler(w); }
static void k_RtlAddVectoredExceptionHandler(w32 *w) { k_AddVectoredExceptionHandler(w); }
static void k_RtlRemoveVectoredExceptionHandler(w32 *w) { k_RemoveVectoredExceptionHandler(w); }

/* RtlCaptureContext(ContextRecord) -- the state of the caller. The Eip and
 * Esp reported are the caller's, not ours, which is the whole reason a
 * program calls it. */
static void k_RtlCaptureContext(w32 *w) {
    uint64_t ctx = ARG(0);
    if (!ctx) { RET(0); return; }
    xc_cpu *c = w32_cpu(w);
    int p = w->is32 ? 4 : 8;
    uint64_t ret_addr = w32_read(w, c->gpr[XC_RSP], p);
    uint64_t saved_rip = c->rip, saved_rsp = c->gpr[XC_RSP];
    c->rip = ret_addr;
    c->gpr[XC_RSP] = w->is32 ? (uint32_t)(saved_rsp + 4 + 4) : saved_rsp + 8;
    w32_context_save(w, ctx);
    c->rip = saved_rip; c->gpr[XC_RSP] = saved_rsp;
    RET(0);
}

/* RtlRestoreContext / NtContinue: put a saved context back and go. */
static void k_NtContinue(w32 *w) {
    uint64_t ctx = ARG(0);
    if (!ctx) { RET(0xC000000Du); return; }
    w32_context_load(w, ctx);
    w->redirected = 1;
}
static void k_RtlRestoreContext(w32 *w) { k_NtContinue(w); }

/* IsBadReadPtr/IsBadWritePtr: a program uses these to avoid faulting, so
 * answering them honestly is what stops it from faulting here. */
static void k_IsBadReadPtr(w32 *w) {
    uint64_t p = ARG(0), n = ARG(1);
    if (!n) { RET(0); return; }
    /* These used to answer from W32P, which for a 64-bit guest says yes to
     * every non-zero number -- so a program using them to avoid a fault was
     * told to go ahead and fault. The mapping table gives the real answer. */
    RET(w32_mem_ok(w, p, n) ? 0 : 1);
}
static void k_IsBadWritePtr(w32 *w) { k_IsBadReadPtr(w); }
static void k_IsBadCodePtr(w32 *w) {
    RET(w32_mem_ok(w, ARG(0), 1) ? 0 : 1);
}

/* These belong to two DLLs on real Windows and are declared as two tables for
 * that reason, not for tidiness: a guest imports RaiseException from
 * kernel32.dll and NtContinue from ntdll.dll, and resolving either from the
 * wrong one would be a lie the import table could catch us in. */
#define F(n, a)  { #n, a, 0, k_##n, 0 }
const w32_api w32_seh_kernel32[] = {
    F(RaiseException, 4),
    F(RtlUnwind, 4),
    F(RtlCaptureContext, 1),
    F(SetUnhandledExceptionFilter, 1),
    F(UnhandledExceptionFilter, 1),
    F(AddVectoredExceptionHandler, 2),
    F(RemoveVectoredExceptionHandler, 1),
    F(AddVectoredContinueHandler, 2),
    F(RemoveVectoredContinueHandler, 1),
    F(IsBadReadPtr, 2),
    F(IsBadWritePtr, 2),
    F(IsBadCodePtr, 1),
    F(RtlLookupFunctionEntry, 3),
    F(RtlVirtualUnwind, 8),
    F(RtlUnwindEx, 6),
    F(RtlPcToFileHeader, 2),
    F(RtlAddFunctionTable, 3),
    F(RtlDeleteFunctionTable, 1),
    F(RtlInstallFunctionTableCallback, 6),
    F(RtlGrowFunctionTable, 2),
    { 0, 0, 0, 0, 0 },
};
const w32_api w32_seh_ntdll[] = {
    F(RtlUnwind, 4),
    F(RtlCaptureContext, 1),
    F(RtlRestoreContext, 2),
    F(NtContinue, 2),
    F(RtlAddVectoredExceptionHandler, 2),
    F(RtlRemoveVectoredExceptionHandler, 1),
    F(RtlLookupFunctionEntry, 3),
    F(RtlVirtualUnwind, 8),
    F(RtlUnwindEx, 6),
    F(RtlPcToFileHeader, 2),
    F(RtlAddFunctionTable, 3),
    F(RtlDeleteFunctionTable, 1),
    F(RtlInstallFunctionTableCallback, 6),
    F(__C_specific_handler, 4),
    { 0, 0, 0, 0, 0 },
};

/* --- notes ----------------------------------------------------------------
 *
 * Faults from dynarec-compiled code do work: each guest memory access carries
 * a recovery stub that writes the guest registers back, so the CONTEXT a
 * handler is handed is the state at the faulting instruction whether the
 * interpreter or a compiled block was running. See the fault-recovery section
 * of core/src/jit/jit.c, and tests/test_faultdiff.c, which holds the state it
 * produces against what x86 says it should be.
 */
