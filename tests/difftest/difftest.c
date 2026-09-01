/* Differential test: run each snippet on the real CPU and in xcore, compare.
 *
 * The CI runner is x86-64, so the CPU it runs on is the specification. Every
 * instruction the interpreter implements gets a case here; a wrong flag or a
 * missed zero-extension shows up as a diff against silicon, not as a game
 * crashing three layers up.
 *
 * Linux x86-64 only. On other hosts this target is not built.
 */
#define _GNU_SOURCE
#include "xcore/cpu.h"

#include <Zydis/Zydis.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* Does this snippet reference memory at all -- read, write, or via the stack?
 *
 * Detected by decoding rather than by observing writes: a load like
 * `mov rax,[rdi+8]` changes no memory and moves no stack pointer, yet its
 * recorded RDI is a host address that means nothing on another machine. An
 * earlier write-based check missed exactly those and the replay faulted.
 * Hidden operands are included, which is what catches PUSH/POP/CALL/RET. */
static int snippet_touches_memory(const uint8_t *code, size_t len) {
    ZydisDecoder d;
    ZydisDecoderInit(&d, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    size_t off = 0;
    while (off < len) {
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&d, code + off, len - off, &in, ops)))
            return 1;                     /* undecodable: assume the worst */
        for (int i = 0; i < in.operand_count; i++)
            if (ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY) return 1;
        off += in.length;
    }
    return 0;
}

typedef struct {
    uint64_t gpr[16];
    uint64_t rflags;
    uint64_t code;
    uint64_t saved_rsp;
} nstate;

extern void native_run(nstate *s);

enum { STACK_SZ = 64 * 1024, DATA_SZ = 4096, CODE_SZ = 4096 };
static uint8_t *g_code, *g_stack, *g_data;
static const uint64_t SENTINEL = 0x5E17E0000ull;      /* unmapped; emulator stops here */

typedef void (*setup_fn)(uint64_t gpr[16], uint64_t *flags);

typedef struct {
    const char    *name;
    const uint8_t *code;
    size_t         len;
    uint64_t       flag_mask;   /* arithmetic flags that are architecturally defined afterwards */
    setup_fn       setup;
} tcase;

/* deterministic register soup so preserved-flag and upper-bit bugs surface */
static uint64_t xs(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

static void seed_regs(uint64_t gpr[16], uint64_t *flags, uint64_t seed, int needs_mem) {
    uint64_t s = seed | 1;
    for (int i = 0; i < 16; i++) gpr[i] = xs(&s);
    gpr[XC_RSP] = (uint64_t)(g_stack + STACK_SZ - 256);
    /* Only point RDI/RSI at the data buffer when the snippet dereferences
     * them. For everything else they are ordinary values, and using host
     * pointers would bake this machine's ASLR layout into the recording. */
    if (needs_mem) {
        gpr[XC_RDI] = (uint64_t)g_data;
        gpr[XC_RSI] = (uint64_t)g_data + 64;
    }
    *flags = 0x202 | (xs(&s) & XC_ARITH_FLAGS);
}

static FILE *g_golden;          /* non-NULL when emitting golden vectors */

static int run_case(const tcase *t, uint64_t seed) {
    /* code: snippet + ret */
    memset(g_code, 0xCC, CODE_SZ);
    memcpy(g_code, t->code, t->len);
    g_code[t->len] = 0xC3;

    const int needs_mem = snippet_touches_memory(t->code, t->len);
    uint64_t gpr[16], flags;
    seed_regs(gpr, &flags, seed, needs_mem);
    if (t->setup) t->setup(gpr, &flags);

    /* fill data + stack deterministically, snapshot */
    uint64_t s = seed * 7 + 3;
    for (size_t i = 0; i < DATA_SZ; i += 8) { uint64_t v = xs(&s); memcpy(g_data + i, &v, 8); }
    for (size_t i = 0; i < STACK_SZ; i += 8) { uint64_t v = xs(&s); memcpy(g_stack + i, &v, 8); }
    uint8_t *snap_data = malloc(DATA_SZ), *snap_stack = malloc(STACK_SZ);
    memcpy(snap_data, g_data, DATA_SZ); memcpy(snap_stack, g_stack, STACK_SZ);

    /* --- native --- */
    nstate n; memset(&n, 0, sizeof n);
    memcpy(n.gpr, gpr, sizeof gpr); n.rflags = flags; n.code = (uint64_t)g_code;
    native_run(&n);
    uint8_t *nat_data = malloc(DATA_SZ), *nat_stack = malloc(STACK_SZ);
    memcpy(nat_data, g_data, DATA_SZ); memcpy(nat_stack, g_stack, STACK_SZ);

    /* --- emulated --- */
    memcpy(g_data, snap_data, DATA_SZ); memcpy(g_stack, snap_stack, STACK_SZ);
    xc_mem mem; xc_mem_init_identity(&mem);
    xc_cpu c; xc_cpu_init(&c, XC_MODE_64, &mem);
    memcpy(c.gpr, gpr, sizeof gpr);
    c.rflags = flags;
    c.rip = (uint64_t)g_code;
    /* the native call pushed a return address; emulate that slot with a sentinel */
    c.gpr[XC_RSP] -= 8;
    memcpy((void *)c.gpr[XC_RSP], &SENTINEL, 8);
    /* native's return address lives in that slot too; exclude it from the compare */
    size_t ret_slot = (size_t)(c.gpr[XC_RSP] - (uint64_t)g_stack);

    int steps = 0; xc_stop st = XC_STOP_NONE;
    while (c.rip != SENTINEL && steps++ < 100000) {
        st = xc_step(&c);
        if (st != XC_STOP_NONE) break;
    }

    int bad = 0;
    if (c.rip != SENTINEL) {
        printf("  [%s] emulator stopped: %s at rip=%#llx (fault_addr=%#llx)\n",
               t->name, xc_stop_name(st), (unsigned long long)c.rip, (unsigned long long)c.fault_addr);
        bad = 1;
    }
    static const char *rn[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                 "r8","r9","r10","r11","r12","r13","r14","r15"};
    for (int i = 0; i < 16; i++) if (n.gpr[i] != c.gpr[i]) {
        printf("  [%s] %s: native %#018llx  emu %#018llx\n", t->name, rn[i],
               (unsigned long long)n.gpr[i], (unsigned long long)c.gpr[i]);
        bad = 1;
    }
    uint64_t fm = t->flag_mask;
    if ((n.rflags & fm) != (c.rflags & fm)) {
        printf("  [%s] flags: native %#06llx  emu %#06llx  (mask %#06llx)\n", t->name,
               (unsigned long long)(n.rflags & XC_ARITH_FLAGS),
               (unsigned long long)(c.rflags & XC_ARITH_FLAGS), (unsigned long long)fm);
        bad = 1;
    }
    if (memcmp(nat_data, g_data, DATA_SZ)) { printf("  [%s] data buffer differs\n", t->name); bad = 1; }
    memcpy(nat_stack + ret_slot, g_stack + ret_slot, 8);   /* ignore the return-address slot */
    if (memcmp(nat_stack, g_stack, STACK_SZ)) { printf("  [%s] stack differs\n", t->name); bad = 1; }

    /* Emit a golden vector: the native (silicon) post-state, so the same case
     * can be replayed on ARM64 where no native oracle exists.
     *
     * Whether the case touched memory is recorded here, where it is known for
     * certain, rather than inferred at replay time from register values. */
    if (g_golden) {
        const int touched = needs_mem;
        /* Memory cases are skipped on replay, and their registers hold host
         * addresses, so their state is recorded as zero. Non-memory cases
         * record everything except RSP, which the trampoline owns. Both rules
         * exist so the file is byte-identical on any machine -- otherwise CI
         * cannot check it for staleness. */
        fprintf(g_golden, "  { \"%s\", 0x%llxull, { ", t->name, (unsigned long long)seed);
        for (int i = 0; i < 16; i++)
            fprintf(g_golden, "0x%llxull,", (unsigned long long)(touched || i == XC_RSP ? 0 : gpr[i]));
        fprintf(g_golden, " }, 0x%llxull, { ",
                (unsigned long long)(touched ? 0 : (flags & XC_ARITH_FLAGS)));
        for (int i = 0; i < 16; i++)
            fprintf(g_golden, "0x%llxull,", (unsigned long long)(touched || i == XC_RSP ? 0 : n.gpr[i]));
        /* Only the flags we model. The raw RFLAGS carries system bits (IF, AC,
         * ID, IOPL) whose values depend on the machine and the hypervisor, and
         * recording them made the file differ between CI runners. */
        fprintf(g_golden, " }, 0x%llxull, 0x%llxull, %d,\n    (const uint8_t[]){",
                (unsigned long long)(touched ? 0 : (n.rflags & XC_ARITH_FLAGS)),
                (unsigned long long)t->flag_mask, touched);
        for (size_t i = 0; i < t->len; i++) fprintf(g_golden, "0x%02x,", t->code[i]);
        fprintf(g_golden, "}, %zu },\n", t->len);
    }

    free(snap_data); free(snap_stack); free(nat_data); free(nat_stack);
    return bad;
}

/* ------------------------------------------------------------------ cases */

#define ALL  XC_ARITH_FLAGS
#define NO_OF (XC_ARITH_FLAGS & ~XC_OF)
#define CO   (XC_CF | XC_OF)                       /* MUL/IMUL: only CF/OF defined */
#define NONE 0
/* Shifts: the SDM leaves AF undefined, and real CPUs disagree about it --
 * this was caught by two CI runners producing different recordings. OF is
 * defined only for a count of exactly 1. */
#define SH1  (XC_ARITH_FLAGS & ~XC_AF)
#define SHN  (XC_ARITH_FLAGS & ~(XC_AF | XC_OF))

static void s_div(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RDX] = 0; g[XC_RCX] |= 1; g[XC_RAX] &= 0xFFFFFFFF; }
static void s_idiv32(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = 0x1234567 | 1; g[XC_RAX] &= 0x7FFFFFFF; }
static void s_cl0(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] &= ~0xFFull; }
static void s_cl1(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = (g[XC_RCX] & ~0xFFull) | 1; }
static void s_cl7(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = (g[XC_RCX] & ~0xFFull) | 7; }
static void s_rcx4(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = 4; }
static void s_rcx16(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = 16; }
static void s_zf(uint64_t g[16], uint64_t *f) { (void)g; *f |= XC_ZF; }
static void s_nzf(uint64_t g[16], uint64_t *f) { (void)g; *f &= ~(uint64_t)XC_ZF; }
static void s_eq(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RBX] = g[XC_RAX]; }
static void s_idx(uint64_t g[16], uint64_t *f) { (void)f; g[XC_RCX] = 3; }

#define B(...) ((const uint8_t[]){__VA_ARGS__})
#define T(nm, mask, setup, ...) { nm, B(__VA_ARGS__), sizeof(B(__VA_ARGS__)), mask, setup }

static const tcase cases[] = {
    /* moves */
    T("mov rax,rbx",            ALL, 0, 0x48,0x89,0xD8),
    T("mov eax,imm32",          ALL, 0, 0xB8,0x78,0x56,0x34,0x12),
    T("mov rax,imm64",          ALL, 0, 0x48,0xB8,0xEF,0xCD,0xAB,0x89,0x67,0x45,0x23,0x01),
    T("mov ah,imm8",            ALL, 0, 0xB4,0x7F),
    T("mov bh,imm8",            ALL, 0, 0xB7,0x33),
    T("mov r8d,imm32",          ALL, 0, 0x41,0xB8,0x05,0x00,0x00,0x00),
    T("mov [rdi],rax",          ALL, 0, 0x48,0x89,0x07),
    T("mov rax,[rdi+8]",        ALL, 0, 0x48,0x8B,0x47,0x08),
    T("mov [rdi+rcx*4],edx",    ALL, s_idx, 0x89,0x14,0x8F),
    T("movzx eax,byte[rdi]",    ALL, 0, 0x0F,0xB6,0x07),
    T("movsx rax,word[rdi+2]",  ALL, 0, 0x48,0x0F,0xBF,0x47,0x02),
    T("movsxd rax,ecx",         ALL, 0, 0x48,0x63,0xC1),
    T("lea rax,[rdi+rcx*8+16]", ALL, 0, 0x48,0x8D,0x44,0xCF,0x10),
    T("xchg rax,rbx",           ALL, 0, 0x48,0x87,0xD8),

    /* alu */
    T("add rax,rbx",            ALL, 0, 0x48,0x01,0xD8),
    T("add al,5",               ALL, 0, 0x04,0x05),
    T("add ah,bh",              ALL, 0, 0x00,0xFC),
    T("add r8b,cl",             ALL, 0, 0x41,0x00,0xC8),
    T("sub ecx,edx",            ALL, 0, 0x29,0xD1),
    T("sub sil,dil",            ALL, 0, 0x40,0x28,0xFE),
    T("adc rax,rcx",            ALL, 0, 0x48,0x11,0xC8),
    T("sbb r8,r9",              ALL, 0, 0x4D,0x19,0xC8),
    T("and rax,0x0F0F",         ALL, 0, 0x48,0x25,0x0F,0x0F,0x00,0x00),
    T("or edx,0x80000000",      ALL, 0, 0x81,0xCA,0x00,0x00,0x00,0x80),
    T("xor r10,r11",            ALL, 0, 0x4D,0x31,0xDA),
    T("xor eax,eax",            ALL, 0, 0x31,0xC0),
    T("cmp rax,rbx",            ALL, 0, 0x48,0x39,0xD8),
    T("cmp rax,rbx (equal)",    ALL, s_eq, 0x48,0x39,0xD8),
    T("test ecx,edx",           ALL, 0, 0x85,0xD1),
    T("inc rax",                ALL, 0, 0x48,0xFF,0xC0),
    T("dec ecx",                ALL, 0, 0xFF,0xC9),
    T("neg rdx",                ALL, 0, 0x48,0xF7,0xDA),
    T("not r9",                 ALL, 0, 0x49,0xF7,0xD1),
    T("add [rdi],rax",          ALL, 0, 0x48,0x01,0x07),
    T("add dword[rdi+4],7",     ALL, 0, 0x83,0x47,0x04,0x07),

    /* shifts -- OF is defined only for a count of 1 */
    T("shl rax,1",              SH1,   0, 0x48,0xD1,0xE0),
    T("shl rax,3",              SHN,   0, 0x48,0xC1,0xE0,0x03),
    T("shr ecx,5",              SHN,   0, 0xC1,0xE9,0x05),
    T("sar rdx,7",              SHN,   0, 0x48,0xC1,0xFA,0x07),
    T("shl rax,cl (cl=0)",      ALL,   s_cl0, 0x48,0xD3,0xE0),
    T("shl rax,cl (cl=1)",      SH1,   s_cl1, 0x48,0xD3,0xE0),
    T("shr rax,cl (cl=7)",      SHN,   s_cl7, 0x48,0xD3,0xE8),
    T("rol eax,9",              NO_OF, 0, 0xC1,0xC0,0x09),
    T("ror rcx,13",             NO_OF, 0, 0x48,0xC1,0xC9,0x0D),
    T("rol rax,1",              ALL,   0, 0x48,0xD1,0xC0),

    /* multiply / divide */
    T("imul rax,rbx",           CO, 0, 0x48,0x0F,0xAF,0xC3),
    T("imul ecx,edx,100",       CO, 0, 0x6B,0xCA,0x64),
    T("imul rbx (1-op)",        CO, 0, 0x48,0xF7,0xEB),
    T("mul rcx",                CO, 0, 0x48,0xF7,0xE1),
    T("mul ecx",                CO, 0, 0xF7,0xE1),
    T("div rcx",                NONE, s_div, 0x48,0xF7,0xF1),
    T("cdq; idiv ecx",          NONE, s_idiv32, 0x99,0xF7,0xF9),

    /* sign extension */
    T("cbw",  ALL, 0, 0x66,0x98),
    T("cwde", ALL, 0, 0x98),
    T("cdqe", ALL, 0, 0x48,0x98),
    T("cdq",  ALL, 0, 0x99),
    T("cqo",  ALL, 0, 0x48,0x99),

    /* stack */
    T("push rbx; pop rcx",      ALL, 0, 0x53,0x59),
    T("push imm8; pop rax",     ALL, 0, 0x6A,0x12,0x58),
    T("push imm32(neg); pop rdx", ALL, 0, 0x68,0x00,0x00,0x00,0x80,0x5A),
    T("push rbp;mov rbp,rsp;sub rsp,32;leave", ALL, 0, 0x55,0x48,0x89,0xE5,0x48,0x83,0xEC,0x20,0xC9),

    /* conditionals */
    T("cmovz eax,ecx (ZF)",     ALL, s_zf,  0x0F,0x44,0xC1),
    T("cmovz eax,ecx (!ZF)",    ALL, s_nzf, 0x0F,0x44,0xC1),
    T("cmovnz rax,rcx",         ALL, 0,     0x48,0x0F,0x45,0xC1),
    T("setb al",                ALL, 0, 0x0F,0x92,0xC0),
    T("setnle dl",              ALL, 0, 0x0F,0x9F,0xC2),
    T("cmp;jz;mov;jmp;mov",     ALL, 0, 0x48,0x39,0xD8, 0x74,0x07, 0xB9,0x01,0x00,0x00,0x00, 0xEB,0x05, 0xB9,0x02,0x00,0x00,0x00),
    T("cmp;jz (taken)",         ALL, s_eq, 0x48,0x39,0xD8, 0x74,0x07, 0xB9,0x01,0x00,0x00,0x00, 0xEB,0x05, 0xB9,0x02,0x00,0x00,0x00),
    T("call/ret",               ALL, 0, 0xE8,0x06,0x00,0x00,0x00, 0x48,0x83,0xC0,0x01, 0xEB,0x05, 0x48,0x83,0xC1,0x07, 0xC3),

    /* strings */
    T("rep stosq",              ALL, s_rcx4,  0xF3,0x48,0xAB),
    T("rep movsb",              ALL, s_rcx16, 0xF3,0xA4),
    T("stosd",                  ALL, 0,       0xAB),
};

int main(int argc, char **argv) {
    const char *emit = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--emit-golden") && i + 1 < argc) emit = argv[++i];

    g_code  = mmap(0, CODE_SZ, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_stack = mmap(0, STACK_SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_data  = mmap(0, DATA_SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_code == MAP_FAILED || g_stack == MAP_FAILED || g_data == MAP_FAILED) {
        perror("mmap"); return 2;
    }

    if (emit) {
        g_golden = fopen(emit, "w");
        if (!g_golden) { perror("fopen"); return 2; }
        fprintf(g_golden,
            "/* GENERATED by difftest --emit-golden. Do not edit.\n"
            " *\n"
            " * Each entry is a snippet plus the post-state a real x86-64 CPU produced\n"
            " * for it. On ARM64 there is no native oracle, so the on-device self-test\n"
            " * replays these and compares -- which is how we know the interpreter\n"
            " * behaves identically on the target CPU, not just on the CI runner.\n"
            " */\n"
            "#include \"xcore/golden.h\"\n\n"
            "const golden_vec xc_golden[] = {\n");
    }

    int failed = 0, total = 0;
    const int n = (int)(sizeof cases / sizeof cases[0]);
    for (int i = 0; i < n; i++) {
        /* several seeds per case: flags-in and register soup vary */
        for (uint64_t seed = 1; seed <= 6; seed++) {
            total++;
            if (run_case(&cases[i], seed * 0x9E3779B97F4A7C15ull)) { failed++; if (!g_golden) break; }
        }
    }
    if (g_golden) {
        fprintf(g_golden, "};\nconst unsigned xc_golden_count = sizeof xc_golden / sizeof xc_golden[0];\n");
        fclose(g_golden);
        printf("difftest: wrote golden vectors to %s\n", emit);
    }
    printf("difftest: %d cases, %d runs, %d failed\n", n, total, failed);
    return failed ? 1 : 0;
}
