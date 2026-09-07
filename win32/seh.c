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

void w32_seh_reset(void) {
    g_filter = 0; g_nveh = 0; g_depth = 0; g_last_code = 0; g_last_addr = 0;
}

/* --- CONTEXT ------------------------------------------------------------- */

/* The x87 area a handler may look at. The register file is stored the way
 * FSAVE stores it -- eight ten-byte entries from ST(0) -- so a guest that
 * reads it sees its own stack in its own order. */
static void save_x87(w32 *w, uint64_t fsa) {
    xc_cpu *c = w->c;
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
    xc_cpu *c = w->c;
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
    xc_cpu *c = w->c;
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
    if (p < w->stack_limit || p + 8 > w->stack_base) return 0;
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
int w32_raise(w32 *w, uint32_t code, uint32_t flags, uint64_t exc_addr,
              int nparams, const uint64_t *params) {
    xc_cpu *c = w->c;
    g_last_code = code; g_last_addr = exc_addr;
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
    if (scratch < w->stack_limit) {
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

    /* The frame list. 64-bit Windows does not have one -- see the note at the
     * end of this file -- so this is the 32-bit path only. */
    if (!handled && w->is32) {
        uint64_t frame = w32_read(w, w->teb + 0, 4), prev = 0;
        while (frame && frame != 0xFFFFFFFFu) {
            if (!frame_ok(w, frame, prev)) {
                fprintf(stderr, "winrun: exception registration list is corrupt at %#llx "
                                "(stack is %#llx..%#llx)\n", (unsigned long long)frame,
                        (unsigned long long)w->stack_limit, (unsigned long long)w->stack_base);
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
    if (handled) { w32_context_load(w, ctx); return 1; }
    c->gpr[XC_RSP] = saved_rsp; c->rip = saved_rip;
    return 0;
}

/* What ended the run, for the crash report. */
uint32_t w32_last_exception(uint64_t *addr) {
    if (addr) *addr = g_last_addr;
    return g_last_code;
}

/* A CPU fault becomes the exception Windows would have raised for it. */
int w32_fault_to_exception(w32 *w) {
    xc_cpu *c = w->c;
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
    if (w->stack_limit && c->fault_addr + 0x10000 >= w->stack_limit && c->fault_addr < w->stack_limit)
        code = EXC_STACK_OVERFLOW;
    return w32_raise(w, code, 0, c->rip, code == EXC_STACK_OVERFLOW ? 0 : 2, params);
}

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

    xc_cpu *c = w->c;
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
    if (!w->is32) { RET(0); return; }
    uint64_t target = ARG(0);
    uint64_t urec = ARG(2);
    uint64_t frame = w32_read(w, w->teb + 0, 4), prev = 0;
    xc_cpu *c = w->c;
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
        w32_write(w, w->teb + 0, 4, next);
        prev = frame; frame = next;
    }
    if (target && target != 0xFFFFFFFFu) w32_write(w, w->teb + 0, 4, target);
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
    xc_cpu *c = w->c;
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
    RET(p && W32P(w, p) && W32P(w, p + n - 1) ? 0 : 1);
}
static void k_IsBadWritePtr(w32 *w) { k_IsBadReadPtr(w); }
static void k_IsBadCodePtr(w32 *w) {
    uint64_t p = ARG(0);
    RET(p && W32P(w, p) ? 0 : 1);
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
    { 0, 0, 0, 0, 0 },
};
const w32_api w32_seh_ntdll[] = {
    F(RtlUnwind, 4),
    F(RtlCaptureContext, 1),
    F(RtlRestoreContext, 2),
    F(NtContinue, 2),
    F(RtlAddVectoredExceptionHandler, 2),
    F(RtlRemoveVectoredExceptionHandler, 1),
    { 0, 0, 0, 0, 0 },
};

/* --- what is not here ----------------------------------------------------
 *
 * 64-bit SEH. x86-64 Windows does not keep a list on the stack; the compiler
 * emits an unwind table (.pdata/.xdata) describing every function's prologue,
 * and the dispatcher looks the faulting RIP up in it, walks frames using that
 * description, and calls the language handler each entry names. It needs the
 * image's exception directory parsed and a frame walker, neither of which the
 * 32-bit path shares.
 *
 * Vectored handlers and the unhandled filter work in both bitnesses because
 * they do not depend on the frame mechanism; a 64-bit guest's __except does.
 *
 * Faults from dynarec-compiled code do work: each guest memory access carries
 * a recovery stub that writes the guest registers back, so the CONTEXT a
 * handler is handed is the state at the faulting instruction whether the
 * interpreter or a compiled block was running. See the fault-recovery section
 * of core/src/jit/jit.c, and tests/test_faultdiff.c, which holds the state it
 * produces against what x86 says it should be.
 */
