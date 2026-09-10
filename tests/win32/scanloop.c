/* The loop GameMaker's data.win reader spends its time in, byte for byte.
 *
 * Spamton Subplot's runner (GameMaker 2026, x64) stopped making progress on
 * the iPad inside a linear scan of a pointer vector: unoptimised code that
 * keeps its cursor and end pointer in stack slots and compares them every
 * step -- `mov rax,[rsp+40]; add rax,8; mov [rsp+40],rax; mov rax,[rsp+56];
 * cmp [rsp+40],rax; je done`. The interpreter runs it; the question was
 * whether the dynarec does. So here is the function exactly as the compiler
 * emitted it (spamton_subplot.exe+0x480230), with its two calls redirected
 * to counters, driven the way the chunk reader drives it: once per entry,
 * with a vector whose elements are a mix of hits, misses and nulls.
 *
 * The expected output is arithmetic, not timing: how many lookups hit, how
 * many missed, and whether every hit wrote its third argument into the
 * object it found. A dynarec that loses the cursor never finishes, and the
 * run's time limit turns that into a failure with the report attached. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef struct obj { unsigned char pad[200]; } obj;    /* +96 result, +108 key, +188 kind */
#define F_RESULT(o) (*(void **)((o)->pad + 96))
#define F_KEY(o)    (*(int *)((o)->pad + 108))
#define F_KIND(o)   (*(int *)((o)->pad + 188))

/* the global the function reads its vector from: {begin, end} */
struct { obj **begin, **end; } g_vec;

static int g_hits, g_misses;
void scan_hit(obj *o) { (void)o; g_hits++; }
int scan_miss(const void *what) { (void)what; g_misses++; return 1; }   /* the original returns its logger's result */

/* int scan_find(int kind, int key, void *result): the function at +0x480230 */
int scan_find(int kind, int key, void *result);
__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    ".globl scan_find\n"
    "scan_find:\n"
    "    mov qword ptr [rsp+24], r8\n"
    "    mov dword ptr [rsp+16], edx\n"
    "    mov dword ptr [rsp+8], ecx\n"
    "    sub rsp, 72\n"
    "    lea rax, [rip + g_vec]\n"
    "    mov qword ptr [rsp+48], rax\n"
    "    mov rax, qword ptr [rsp+48]\n"
    "    mov rax, qword ptr [rax]\n"
    "    mov qword ptr [rsp+40], rax\n"
    "    mov rax, qword ptr [rsp+48]\n"
    "    mov rax, qword ptr [rax+8]\n"
    "    mov qword ptr [rsp+56], rax\n"
    "    jmp 2f\n"
    "1:  mov rax, qword ptr [rsp+40]\n"
    "    add rax, 8\n"
    "    mov qword ptr [rsp+40], rax\n"
    "2:  mov rax, qword ptr [rsp+56]\n"
    "    cmp qword ptr [rsp+40], rax\n"
    "    je 5f\n"
    "    mov rax, qword ptr [rsp+40]\n"
    "    mov rax, qword ptr [rax]\n"
    "    mov qword ptr [rsp+32], rax\n"
    "    cmp qword ptr [rsp+32], 0\n"
    "    jne 3f\n"
    "    jmp 1b\n"
    "3:  mov rax, qword ptr [rsp+32]\n"
    "    mov ecx, dword ptr [rsp+80]\n"
    "    cmp dword ptr [rax+188], ecx\n"
    "    jne 4f\n"
    "    mov rax, qword ptr [rsp+32]\n"
    "    mov ecx, dword ptr [rsp+88]\n"
    "    cmp dword ptr [rax+108], ecx\n"
    "    je 6f\n"
    "4:  jmp 1b\n"
    "6:  mov rcx, qword ptr [rsp+32]\n"
    "    call scan_hit\n"
    "    mov rax, qword ptr [rsp+32]\n"
    "    mov rcx, qword ptr [rsp+96]\n"
    "    mov qword ptr [rax+96], rcx\n"
    "    xor eax, eax\n"
    "    jmp 7f\n"
    "    jmp 1b\n"
    "5:  lea rcx, [rip + g_vec]\n"
    "    call scan_miss\n"
    "7:  add rsp, 72\n"
    "    ret\n"
    ".att_syntax prefix\n"
);

#define NOBJ 3000
#define NENTRIES 189
static obj g_objs[NOBJ];
static obj *g_ptrs[NOBJ];
static int g_marker[NENTRIES];

int main(void) {
    /* every third slot null; the rest keyed 0..149 (kind 0) in a scrambled
     * order, plus decoys of kind 1 that share the keys and must be skipped */
    int n = 0, live = 0;
    for (int i = 0; i < NOBJ; i++) {
        if (i % 3 == 1) { g_ptrs[n++] = NULL; continue; }
        obj *o = &g_objs[i];
        memset(o, 0, sizeof *o);
        F_KEY(o) = (live * 7919) % 150;               /* 150 distinct keys per kind */
        F_KIND(o) = (live / 150) % 2 == 0 && live < 300 ? live / 150 : 1;
        live++;
        g_ptrs[n++] = o;
    }
    g_vec.begin = g_ptrs;
    g_vec.end = g_ptrs + n;
    printf("vector: %d slots, %d objects, keys 0..149 of kind 0 present once each\n", n, live);

    int rc_ok = 0;
    for (int k = 0; k < NENTRIES; k++) {
        int rc = scan_find(0, k, &g_marker[k]);
        if ((k < 150 && rc == 0) || (k >= 150 && rc != 0)) rc_ok++;
    }
    printf("lookups: %d, hits %d (want 150), misses %d (want 39), return codes right: %d of %d\n",
           NENTRIES, g_hits, g_misses, rc_ok, NENTRIES);

    int set = 0;
    for (int i = 0; i < NOBJ; i++) {
        obj *o = &g_objs[i];
        if (i % 3 == 1 || F_KIND(o) != 0) continue;
        if (F_RESULT(o) == &g_marker[F_KEY(o)]) set++;
    }
    printf("every hit wrote its result into the right object: %s (%d)\n", set == 150 ? "yes" : "NO", set);
    return g_hits == 150 && g_misses == 39 && set == 150 ? 0 : 1;
}
