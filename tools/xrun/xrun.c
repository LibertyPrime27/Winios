/* xrun -- load a static x86-64 Linux ELF and run it on xcore.
 *
 * This is the smallest thing that turns the interpreter from "passes vectors"
 * into "runs programs": an ELF loader, an initial stack the way the kernel
 * builds it, and a Linux system-call layer. Guest addresses are host
 * addresses (xcore's identity mapping), so a pointer the guest passes to
 * write() can be handed straight to the host.
 *
 * The system-call layer is deliberately explicit rather than a blind
 * pass-through: every call the guest makes is one we chose to allow, with
 * the ones that would let guest state leak into this process (signal
 * handlers, brk, thread creation) handled here instead. The list is what
 * musl and glibc static binaries need to start, print, allocate and exit.
 * Linux x86-64 host only for now; the same layer will sit on top of a Mach
 * process later, which is why the guest is never allowed to touch the host
 * kernel directly.
 *
 *   xrun [-v] [-s max_steps] program [args...]
 */
#define _GNU_SOURCE
#include "xcore/cpu.h"
#include "xrun.h"

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static int verbose;

#define PAGE 4096ull
#define PAGE_DOWN(x) ((x) & ~(PAGE - 1))
#define PAGE_UP(x)   (((x) + PAGE - 1) & ~(PAGE - 1))

/* -------------------------------------------------------------- loader */

typedef struct {
    uint64_t entry;
    uint64_t base;          /* load bias (0 for ET_EXEC) */
    uint64_t phdr;          /* guest address of the program headers */
    uint16_t phnum, phent;
    uint64_t brk_start, brk_cur, brk_max;
} image;

static int load_elf(const char *path, image *im) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }
    struct stat st; fstat(fd, &st);
    uint8_t *file = mmap(0, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file == MAP_FAILED) { perror("mmap file"); return -1; }

    Elf64_Ehdr *eh = (Elf64_Ehdr *)file;
    if (memcmp(eh->e_ident, ELFMAG, 4) || eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_machine != EM_X86_64) {
        fprintf(stderr, "%s: not an x86-64 ELF64\n", path); return -1;
    }
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) { fprintf(stderr, "%s: not an executable\n", path); return -1; }
    Elf64_Phdr *ph = (Elf64_Phdr *)(file + eh->e_phoff);

    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_INTERP) { fprintf(stderr, "%s: dynamically linked; only static binaries for now\n", path); return -1; }

    /* Extent of all PT_LOADs, so a PIE gets one contiguous reservation. */
    uint64_t lo = ~0ull, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD) {
        if (PAGE_DOWN(ph[i].p_vaddr) < lo) lo = PAGE_DOWN(ph[i].p_vaddr);
        if (PAGE_UP(ph[i].p_vaddr + ph[i].p_memsz) > hi) hi = PAGE_UP(ph[i].p_vaddr + ph[i].p_memsz);
    }
    /* Reserve the whole image first: for ET_EXEC exactly where it asks (and
     * fail loudly if this process already lives there), for a PIE wherever
     * the host has room -- above 4 GB by preference so the same binary also
     * loads where Darwin reserves the low 4 GB. */
    uint64_t bias = 0;
    void *res;
    if (eh->e_type == ET_DYN) {
        res = mmap((void *)0x200000000ull, hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (res != MAP_FAILED) bias = (uint64_t)res - lo;
    } else {
        res = mmap((void *)lo, hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        if (res != MAP_FAILED && (uint64_t)res != lo) { munmap(res, hi - lo); res = MAP_FAILED; }
    }
    if (res == MAP_FAILED) { fprintf(stderr, "%s: cannot reserve %#llx..%#llx\n", path, (unsigned long long)lo, (unsigned long long)hi); return -1; }

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t va = ph[i].p_vaddr + bias;
        uint64_t start = PAGE_DOWN(va), end = PAGE_UP(va + ph[i].p_memsz);
        int prot = (ph[i].p_flags & PF_R ? PROT_READ : 0) | (ph[i].p_flags & PF_W ? PROT_WRITE : 0) | (ph[i].p_flags & PF_X ? PROT_EXEC : 0);
        void *p = mmap((void *)start, end - start, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (p == MAP_FAILED) { perror("mmap segment"); return -1; }
        memcpy((void *)va, file + ph[i].p_offset, ph[i].p_filesz);
        /* The interpreter reads guest code through xc_mem_ptr, so a segment
         * needs no host PROT_EXEC; keep it writable-or-not as the ELF says. */
        int hp = prot & (PROT_READ | PROT_WRITE);
        mprotect((void *)start, end - start, hp ? hp : PROT_READ);
        if (end > im->brk_start) im->brk_start = end;
    }
    im->entry = eh->e_entry + bias;
    im->base = bias;
    im->phnum = eh->e_phnum; im->phent = eh->e_phentsize;
    /* Program headers as the guest sees them: inside the first PT_LOAD that
     * covers e_phoff, else copy them somewhere readable. */
    im->phdr = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_offset <= eh->e_phoff && eh->e_phoff < ph[i].p_offset + ph[i].p_filesz)
            im->phdr = ph[i].p_vaddr + bias + (eh->e_phoff - ph[i].p_offset);
    if (!im->phdr) {
        void *p = mmap(0, PAGE_UP((uint64_t)eh->e_phnum * eh->e_phentsize), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        memcpy(p, ph, (size_t)eh->e_phnum * eh->e_phentsize);
        im->phdr = (uint64_t)p;
    }

    /* brk heap: reserve right after the image, grow on demand. */
    im->brk_start = PAGE_UP(im->brk_start);
    im->brk_cur = im->brk_start;
    im->brk_max = im->brk_start + (256ull << 20);
    if (mmap((void *)im->brk_start, im->brk_max - im->brk_start, PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0) == MAP_FAILED) {
        perror("reserve brk"); return -1;
    }
    munmap(file, (size_t)st.st_size);
    close(fd);
    return 0;
}

/* ------------------------------------------------------- initial stack */

#define STACK_SIZE (8ull << 20)

static uint64_t build_stack(const image *im, int argc, char **argv, char **envp) {
    uint8_t *stk = mmap((void *)0x7f0000000000ull, STACK_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (stk == MAP_FAILED) { perror("stack"); exit(2); }
    uint8_t *top = stk + STACK_SIZE;

    /* strings first, from the top down */
    int envc = 0; while (envp[envc]) envc++;
    uint64_t *argp = calloc((size_t)argc + 1, 8), *envpp = calloc((size_t)envc + 1, 8);
    uint8_t *sp = top - 16;
    for (int i = argc - 1; i >= 0; i--) { size_t n = strlen(argv[i]) + 1; sp -= n; memcpy(sp, argv[i], n); argp[i] = (uint64_t)sp; }
    for (int i = envc - 1; i >= 0; i--) { size_t n = strlen(envp[i]) + 1; sp -= n; memcpy(sp, envp[i], n); envpp[i] = (uint64_t)sp; }
    sp -= 16; uint8_t *rnd = sp;                      /* AT_RANDOM: 16 bytes */
    for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)(0x5A + i * 37);
    sp -= 8; memcpy(sp, "x86_64", 7); uint8_t *plat = sp;

    /* auxv, envp, argv, argc -- with rsp 16-byte aligned at argc */
    struct { uint64_t k, v; } aux[] = {
        { AT_PHDR, im->phdr }, { AT_PHENT, im->phent }, { AT_PHNUM, im->phnum },
        { AT_PAGESZ, PAGE }, { AT_BASE, 0 }, { AT_FLAGS, 0 }, { AT_ENTRY, im->entry },
        { AT_UID, 1000 }, { AT_EUID, 1000 }, { AT_GID, 1000 }, { AT_EGID, 1000 },
        { AT_SECURE, 0 }, { AT_RANDOM, (uint64_t)rnd }, { AT_HWCAP, 0x078bfbff },
        { AT_CLKTCK, 100 }, { AT_PLATFORM, (uint64_t)plat }, { AT_EXECFN, argp[0] },
        { AT_NULL, 0 },
    };
    size_t naux = sizeof aux / sizeof aux[0];
    size_t words = 1 + (size_t)argc + 1 + (size_t)envc + 1 + naux * 2;
    uint64_t *base = (uint64_t *)(((uint64_t)sp - words * 8) & ~15ull);
    uint64_t *w = base;
    *w++ = (uint64_t)argc;
    for (int i = 0; i < argc; i++) *w++ = argp[i];
    *w++ = 0;
    for (int i = 0; i < envc; i++) *w++ = envpp[i];
    *w++ = 0;
    for (size_t i = 0; i < naux; i++) { *w++ = aux[i].k; *w++ = aux[i].v; }
    free(argp); free(envpp);
    return (uint64_t)base;
}

/* ------------------------------------------------------------ syscalls */

static image g_im;
static uint64_t g_tid_addr;

#define ARG(n) (c->gpr[(int[]){XC_RDI, XC_RSI, XC_RDX, XC_R10, XC_R8, XC_R9}[n]])

#if defined(__x86_64__)
static long host(long nr, long a, long b, long c_, long d, long e, long f) {
    long r = syscall(nr, a, b, c_, d, e, f);
    return r < 0 ? -errno : r;
}
#endif

/* Returns 1 to keep running, 0 on exit (status in *code). */
#if !defined(__x86_64__)
/* On a non-x86 host the guest's syscall numbers mean nothing to the kernel,
 * so this is a small translated table -- the calls a musl static binary
 * needs to start, print and exit -- through libc wrappers. Enough to run
 * the guest tests on an ARM64 host; the 32-bit path (linux32.c) is complete. */
#include <sys/uio.h>
#include <sys/ioctl.h>
static int do_syscall(xc_cpu *c, int *code) {
    uint64_t nr = c->gpr[XC_RAX];
    long a0 = (long)ARG(0), a1 = (long)ARG(1), a2 = (long)ARG(2);
    long r;
    switch (nr) {
    case 0:   r = read((int)a0, (void *)a1, (size_t)a2); if (r < 0) r = -errno; break;
    case 1:   r = write((int)a0, (const void *)a1, (size_t)a2); if (r < 0) r = -errno; break;
    case 3:   r = close((int)a0) < 0 ? -errno : 0; break;
    case 16:  r = ioctl((int)a0, (unsigned long)a1, (void *)a2) < 0 ? -errno : 0; break;
    case 20:  r = writev((int)a0, (const struct iovec *)a1, (int)a2); if (r < 0) r = -errno; break;
    case 39:  r = getpid(); break;
    case 60: case 231: *code = (int)a0 & 0xff; return 0;
    case 9: {                                            /* mmap: the identity mapping makes this a host mmap */
        void *p = mmap((void *)a0, (size_t)a1, (int)a2, (int)ARG(3), (int)ARG(4), (off_t)ARG(5));
        r = p == MAP_FAILED ? -errno : (long)p; break;
    }
    case 10:  r = mprotect((void *)a0, (size_t)a1, (int)a2) < 0 ? -errno : 0; break;
    case 11:  r = munmap((void *)a0, (size_t)a1) < 0 ? -errno : 0; break;
    case 12: {
        uint64_t want = (uint64_t)a0;
        if (want == 0 || want < g_im.brk_start || want > g_im.brk_max) { r = (long)g_im.brk_cur; break; }
        uint64_t cur_pg = PAGE_UP(g_im.brk_cur), want_pg = PAGE_UP(want);
        if (want_pg > cur_pg) mprotect((void *)cur_pg, want_pg - cur_pg, PROT_READ | PROT_WRITE);
        g_im.brk_cur = want; r = (long)want; break;
    }
    case 13: case 14: case 131: case 273: r = 0; break;  /* rt_sigaction, rt_sigprocmask, sigaltstack, set_robust_list */
    case 158:
        if (a0 == 0x1002) { c->fs_base = (uint64_t)a1; r = 0; } else if (a0 == 0x1001) { c->gs_base = (uint64_t)a1; r = 0; } else r = -EINVAL;
        break;
    case 218: g_tid_addr = (uint64_t)a0; r = getpid(); break;
    case 228: { struct timespec ts; r = clock_gettime((clockid_t)a0, &ts) < 0 ? -errno : 0; if (!r) memcpy((void *)a1, &ts, sizeof ts); break; }
    case 334: r = -ENOSYS; break;                        /* rseq */
    default:
        fprintf(stderr, "xrun: unimplemented (translated) syscall %llu at rip=%#llx\n", (unsigned long long)nr, (unsigned long long)c->rip);
        r = -ENOSYS;
    }
    if (verbose) fprintf(stderr, "  syscall %llu(%#lx, %#lx, %#lx) = %ld\n", (unsigned long long)nr, a0, a1, a2, r);
    c->gpr[XC_RAX] = (uint64_t)r;
    c->gpr[XC_RCX] = c->rip;
    c->gpr[XC_R11] = c->rflags;
    return 1;
}
#else
static int do_syscall(xc_cpu *c, int *code) {
    uint64_t nr = c->gpr[XC_RAX];
    long r;
    long a0 = (long)ARG(0), a1 = (long)ARG(1), a2 = (long)ARG(2), a3 = (long)ARG(3), a4 = (long)ARG(4), a5 = (long)ARG(5);
    switch (nr) {
    /* plain pass-through: pointers are host pointers, layouts are the host's */
    case SYS_read: case SYS_write: case SYS_close: case SYS_fstat: case SYS_lseek:
    case SYS_pread64: case SYS_pwrite64: case SYS_readv: case SYS_writev: case SYS_access:
    case SYS_dup: case SYS_dup2: case SYS_getpid: case SYS_uname: case SYS_fcntl:
    case SYS_getcwd: case SYS_readlink: case SYS_getuid: case SYS_getgid: case SYS_geteuid:
    case SYS_getegid: case SYS_gettid: case SYS_clock_gettime: case SYS_clock_getres:
    case SYS_nanosleep: case SYS_clock_nanosleep: case SYS_openat: case SYS_newfstatat:
    case SYS_getrandom: case SYS_prlimit64: case SYS_getdents64: case SYS_ioctl:
    case SYS_pipe2: case SYS_poll: case SYS_stat: case SYS_lstat: case SYS_unlink:
    case SYS_mkdir: case SYS_rename: case SYS_ftruncate: case SYS_fsync: case SYS_umask:
    case SYS_gettimeofday: case SYS_sched_yield: case SYS_getppid: case SYS_getpgrp:
    case SYS_socket: case SYS_connect: case SYS_sendto: case SYS_recvfrom: case SYS_shutdown:
    case SYS_readlinkat: case SYS_faccessat: case SYS_faccessat2: case SYS_unlinkat: case SYS_mkdirat:
    case SYS_renameat: case SYS_renameat2: case SYS_fchmod: case SYS_fchmodat: case SYS_fchown:
    case SYS_utimensat: case SYS_statx: case SYS_getdents: case SYS_chdir: case SYS_fchdir:
    case SYS_sysinfo: case SYS_getrlimit: case SYS_setrlimit: case SYS_times: case SYS_getpgid:
    case SYS_getsid: case SYS_setpgid: case SYS_getgroups: case SYS_pipe: case SYS_select:
    case SYS_pselect6: case SYS_ppoll: case SYS_epoll_create1: case SYS_epoll_ctl: case SYS_epoll_wait:
    case SYS_eventfd2: case SYS_timerfd_create: case SYS_timerfd_settime: case SYS_symlinkat:
    case SYS_linkat: case SYS_truncate: case SYS_fdatasync: case SYS_flock: case SYS_fallocate:
    case SYS_copy_file_range: case SYS_sendfile: case SYS_splice: case SYS_getpriority:
    case SYS_setpriority: case SYS_sched_getaffinity: case SYS_getcpu:
        r = host((long)nr, a0, a1, a2, a3, a4, a5); break;
    case SYS_open:
        r = host(SYS_openat, AT_FDCWD, a0, a1, a2, 0, 0); break;

    /* memory: identity mapping means the host call is exactly right, but
     * never let the guest unmap or remap our own pages */
    case SYS_mmap: {
        int flags = (int)a3;
        if ((flags & MAP_FIXED) && a0 < 0x10000) { r = -EINVAL; break; }
        r = host(SYS_mmap, a0, a1, a2, a3, a4, a5);
        break;
    }
    case SYS_munmap: case SYS_mprotect: case SYS_madvise: case SYS_mremap:
        r = host((long)nr, a0, a1, a2, a3, a4, a5); break;
    case SYS_brk: {
        uint64_t want = (uint64_t)a0;
        if (want == 0) { r = (long)g_im.brk_cur; break; }
        if (want < g_im.brk_start || want > g_im.brk_max) { r = (long)g_im.brk_cur; break; }
        uint64_t cur_pg = PAGE_UP(g_im.brk_cur), want_pg = PAGE_UP(want);
        if (want_pg > cur_pg) mprotect((void *)cur_pg, want_pg - cur_pg, PROT_READ | PROT_WRITE);
        g_im.brk_cur = want;
        r = (long)want;
        break;
    }

    /* threading and signals: single core, no signals delivered -- accept */
    case SYS_arch_prctl:
        if (a0 == 0x1002) { c->fs_base = (uint64_t)a1; r = 0; }             /* ARCH_SET_FS */
        else if (a0 == 0x1003) { *(uint64_t *)a1 = c->fs_base; r = 0; }    /* ARCH_GET_FS */
        else if (a0 == 0x1001) { c->gs_base = (uint64_t)a1; r = 0; }
        else if (a0 == 0x1004) { *(uint64_t *)a1 = c->gs_base; r = 0; }
        else r = -EINVAL;
        break;
    case SYS_set_tid_address: g_tid_addr = (uint64_t)a0; r = getpid(); break;
    case SYS_set_robust_list: case SYS_rt_sigaction: case SYS_rt_sigprocmask:
    case SYS_sigaltstack: case SYS_prctl: case SYS_membarrier:
        r = 0; break;
    case SYS_rseq: r = -ENOSYS; break;
    case SYS_futex:
        /* FUTEX_WAKE / FUTEX_WAIT on a single thread: nobody to wake, and a
         * wait would block forever -- report "no waiters" / EAGAIN. */
        r = ((a1 & 0x7f) == 1) ? 0 : -EAGAIN; break;
    case SYS_tgkill: case SYS_kill:
        fprintf(stderr, "xrun: guest raised signal %ld\n", a2 ? a2 : a1);
        *code = 128 + (int)(a2 ? a2 : a1); return 0;

    case SYS_exit: case SYS_exit_group:
        *code = (int)a0 & 0xff; return 0;

    default:
        fprintf(stderr, "xrun: unimplemented syscall %llu at rip=%#llx\n",
                (unsigned long long)nr, (unsigned long long)c->rip);
        r = -ENOSYS;
    }
    if (verbose) fprintf(stderr, "  syscall %llu(%#lx, %#lx, %#lx) = %ld\n", (unsigned long long)nr, a0, a1, a2, r);
    c->gpr[XC_RAX] = (uint64_t)r;
    c->gpr[XC_RCX] = c->rip;                 /* SYSCALL clobbers rcx and r11 */
    c->gpr[XC_R11] = c->rflags;
    return 1;
}
#endif

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv, char **envp) {
    uint64_t max_steps = ~0ull;
    int ai = 1;
    while (ai < argc && argv[ai][0] == '-') {
        if (!strcmp(argv[ai], "-v")) verbose = 1;
        else if (!strcmp(argv[ai], "-s") && ai + 1 < argc) max_steps = strtoull(argv[++ai], 0, 0);
        else { fprintf(stderr, "usage: xrun [-v] [-s max_steps] program [args...]\n"); return 2; }
        ai++;
    }
    if (ai >= argc) { fprintf(stderr, "usage: xrun [-v] [-s max_steps] program [args...]\n"); return 2; }

    /* Which ELF class? 32-bit guests get the arena, 64-bit ones identity. */
    int is32 = 0;
    { FILE *f = fopen(argv[ai], "rb"); unsigned char id[5] = {0}; if (f) { if (fread(id, 1, 5, f) != 5) id[0] = 0; fclose(f); }
      if (memcmp(id, ELFMAG, 4)) { fprintf(stderr, "%s: not an ELF file\n", argv[ai]); return 2; }
      is32 = id[EI_CLASS] == ELFCLASS32; }

    xc_mem mem; xc_cpu c;
    if (is32) {
        if (load_elf32(argv[ai])) return 2;
        uint32_t esp = build_stack32(argc - ai, argv + ai, envp);
        xc_mem_init_arena(&mem, arena_base32(), 1ull << 32);
        xc_cpu_init(&c, XC_MODE_32, &mem);
        c.rip = entry32();
        c.gpr[XC_RSP] = esp;
        c.sreg[1] = 0x23; c.sreg[0] = c.sreg[2] = c.sreg[3] = 0x2b;     /* what a 32-bit Linux process sees */
    } else {
        if (load_elf(argv[ai], &g_im)) return 2;
        uint64_t rsp = build_stack(&g_im, argc - ai, argv + ai, envp);
        xc_mem_init_identity(&mem);
        xc_cpu_init(&c, XC_MODE_64, &mem);
        c.rip = g_im.entry;
        c.gpr[XC_RSP] = rsp;
    }
    if (verbose) fprintf(stderr, "xrun: %d-bit guest, entry %#llx rsp %#llx\n", is32 ? 32 : 64,
                         (unsigned long long)c.rip, (unsigned long long)c.gpr[XC_RSP]);

    uint64_t steps = 0;
    int code = 0;
    for (;;) {
        uint64_t chunk = max_steps - steps < 100000 ? max_steps - steps : 100000;
        if (chunk == 0) { fprintf(stderr, "xrun: step limit reached at rip=%#llx\n", (unsigned long long)c.rip); code = 124; break; }
        xc_stop st = xc_run(&c, chunk);
        steps += chunk;                      /* approximate: the last chunk may be short */
        if (st == XC_STOP_STEPS) continue;
        if (st == XC_STOP_SYSCALL) {
            int keep = c.syscall_vector == 0x80 ? do_syscall32(&c, &code, verbose)
                     : c.syscall_vector == -1 && !is32 ? do_syscall(&c, &code)
                     : (fprintf(stderr, "xrun: unexpected syscall vector %d at rip=%#llx\n", c.syscall_vector, (unsigned long long)c.rip), code = 125, 0);
            if (!keep) break;
            continue;
        }
        char dis[128]; xc_disasm(&c, c.rip, dis, sizeof dis);
        fprintf(stderr, "xrun: stopped: %s at rip=%#llx  [%s]", xc_stop_name(st), (unsigned long long)c.rip, dis);
        if (st == XC_STOP_FAULT) fprintf(stderr, "  fault_addr=%#llx", (unsigned long long)c.fault_addr);
        fprintf(stderr, "\n");
        code = 125; break;
    }
    if (verbose) {
        uint64_t hits, builds, flushes, smc, jb, jco, jbytes;
        xc_cache_stats(&hits, &builds, &flushes, &smc);
        xc_jit_stats(&jb, &jco, &jbytes);
        fprintf(stderr, "xrun: exit %d after ~%llu steps; blocks built %llu, hits %llu, smc %llu; jit: %s, %llu blocks (%llu KB), %llu callouts\n",
                code, (unsigned long long)steps, (unsigned long long)builds, (unsigned long long)hits, (unsigned long long)smc,
                xc_jit_enabled() ? "on" : "off", (unsigned long long)jb, (unsigned long long)(jbytes >> 10), (unsigned long long)jco);
    }
    return code;
}
