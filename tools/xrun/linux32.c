/* xrun, 32-bit half: load a static i386 Linux ELF into a 4 GB arena and
 * service its `int 0x80` system calls.
 *
 * This is the memory model every 32-bit game will use on iOS: the guest's
 * whole address space is one host reservation, and a guest address g is host
 * address base+g. Nothing here is passed to the kernel without translation --
 * pointers are rebased, and the i386 struct layouts (stat64, iovec, timespec,
 * rlimit) are converted, because the host is 64-bit. The Win32 loader later
 * replaces the ELF and syscall parts of this file and keeps the memory model.
 *
 * glibc's static i386 startup is the acceptance test: set_thread_area for
 * TLS through %gs, brk for malloc, and the usual dozen of probes.
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
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define PAGE 4096u
#define PAGE_DOWN(x) ((x) & ~(PAGE - 1))
#define PAGE_UP(x)   (((x) + PAGE - 1) & ~(PAGE - 1))
#define ARENA (1ull << 32)

static uint8_t *g_base;               /* host address of guest 0 */
static uint32_t g_entry, g_phdr, g_phnum, g_phent;
static uint32_t g_brk_start, g_brk_cur, g_brk_max;
static uint32_t g_mmap_next = 0x40000000u;     /* bump allocator for anonymous mappings */
static uint32_t g_tid_addr;

/* guest -> host; NULL for a guest NULL so callers can pass it on */
static void *P(uint32_t g) { return g ? g_base + g : 0; }

/* Map guest pages: the arena is one PROT_NONE reservation, so this is a
 * MAP_FIXED over our own range. */
static int guest_map(uint32_t addr, uint32_t len, int prot, int fd, uint64_t off) {
    int flags = MAP_FIXED | MAP_PRIVATE | (fd < 0 ? MAP_ANONYMOUS : 0);
    void *p = mmap(g_base + addr, len, prot, flags, fd, (off_t)off);
    return p == MAP_FAILED ? -errno : 0;
}

/* --------------------------------------------------------------- loader */

int load_elf32(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }
    struct stat st; fstat(fd, &st);
    uint8_t *file = mmap(0, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file == MAP_FAILED) { perror("mmap file"); return -1; }
    Elf32_Ehdr *eh = (Elf32_Ehdr *)file;
    if (eh->e_machine != EM_386 || eh->e_type != ET_EXEC) { fprintf(stderr, "%s: need a static i386 executable\n", path); return -1; }
    Elf32_Phdr *ph = (Elf32_Phdr *)(file + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_INTERP) { fprintf(stderr, "%s: dynamically linked; only static binaries for now\n", path); return -1; }

    g_base = mmap(0, ARENA, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_base == MAP_FAILED) { perror("reserve 4 GB arena"); return -1; }

    uint32_t end_max = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint32_t start = PAGE_DOWN(ph[i].p_vaddr), end = PAGE_UP(ph[i].p_vaddr + ph[i].p_memsz);
        if (guest_map(start, end - start, PROT_READ | PROT_WRITE, -1, 0)) { perror("map segment"); return -1; }
        memcpy(g_base + ph[i].p_vaddr, file + ph[i].p_offset, ph[i].p_filesz);
        int prot = (ph[i].p_flags & PF_W) ? PROT_READ | PROT_WRITE : PROT_READ;
        mprotect(g_base + start, end - start, prot);
        if (end > end_max) end_max = end;
    }
    g_entry = eh->e_entry; g_phnum = eh->e_phnum; g_phent = eh->e_phentsize;
    g_phdr = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_offset <= eh->e_phoff && eh->e_phoff < ph[i].p_offset + ph[i].p_filesz)
            g_phdr = ph[i].p_vaddr + (eh->e_phoff - ph[i].p_offset);
    g_brk_start = g_brk_cur = PAGE_UP(end_max);
    g_brk_max = g_brk_start + (256u << 20);
    munmap(file, (size_t)st.st_size); close(fd);
    return 0;
}

/* Initial stack at the top of the arena, laid out as the i386 kernel does. */
#define STACK_TOP  0xFFFFE000u
#define STACK_SIZE (8u << 20)

uint32_t build_stack32(int argc, char **argv, char **envp) {
    if (guest_map(STACK_TOP - STACK_SIZE, STACK_SIZE, PROT_READ | PROT_WRITE, -1, 0)) { perror("stack"); exit(2); }
    int envc = 0; while (envp[envc]) envc++;
    uint32_t *argp = calloc((size_t)argc + 1, 4), *envpp = calloc((size_t)envc + 1, 4);
    uint32_t sp = STACK_TOP - 16;
    for (int i = argc - 1; i >= 0; i--) { uint32_t n = (uint32_t)strlen(argv[i]) + 1; sp -= n; memcpy(P(sp), argv[i], n); argp[i] = sp; }
    for (int i = envc - 1; i >= 0; i--) { uint32_t n = (uint32_t)strlen(envp[i]) + 1; sp -= n; memcpy(P(sp), envp[i], n); envpp[i] = sp; }
    sp -= 16; uint32_t rnd = sp; for (int i = 0; i < 16; i++) ((uint8_t *)P(rnd))[i] = (uint8_t)(0x5A + i * 37);
    sp -= 8; memcpy(P(sp), "i686", 5); uint32_t plat = sp;

    struct { uint32_t k, v; } aux[] = {
        { AT_PHDR, g_phdr }, { AT_PHENT, g_phent }, { AT_PHNUM, g_phnum }, { AT_PAGESZ, PAGE },
        { AT_BASE, 0 }, { AT_FLAGS, 0 }, { AT_ENTRY, g_entry }, { AT_UID, 1000 }, { AT_EUID, 1000 },
        { AT_GID, 1000 }, { AT_EGID, 1000 }, { AT_SECURE, 0 }, { AT_RANDOM, rnd }, { AT_HWCAP, 0x178bfbff },
        { AT_CLKTCK, 100 }, { AT_PLATFORM, plat }, { AT_EXECFN, argp[0] }, { AT_NULL, 0 },
        /* no AT_SYSINFO: the guest uses int 0x80, which is what we service */
    };
    uint32_t naux = sizeof aux / sizeof aux[0];
    uint32_t words = 1 + (uint32_t)argc + 1 + (uint32_t)envc + 1 + naux * 2;
    uint32_t base = (sp - words * 4) & ~15u;
    uint32_t *w = (uint32_t *)P(base);
    *w++ = (uint32_t)argc;
    for (int i = 0; i < argc; i++) *w++ = argp[i];
    *w++ = 0;
    for (int i = 0; i < envc; i++) *w++ = envpp[i];
    *w++ = 0;
    for (uint32_t i = 0; i < naux; i++) { *w++ = aux[i].k; *w++ = aux[i].v; }
    free(argp); free(envpp);
    return base;
}

void *arena_base32(void) { return g_base; }
uint32_t entry32(void) { return g_entry; }

/* ------------------------------------------------------------ syscalls */

static long host(long nr, long a, long b, long c_, long d, long e, long f) {
    long r = syscall(nr, a, b, c_, d, e, f);
    return r < 0 ? -errno : r;
}

/* i386 struct stat64 (96 bytes, packed layout) from the host's struct stat */
static void put_stat64(uint32_t g, const struct stat *s) {
    uint8_t *p = P(g); memset(p, 0, 96);
    uint64_t dev = s->st_dev, rdev = s->st_rdev, ino = s->st_ino, blocks = (uint64_t)s->st_blocks;
    int64_t size = s->st_size;
    uint32_t v;
    memcpy(p + 0, &dev, 8);
    v = (uint32_t)ino;            memcpy(p + 12, &v, 4);
    v = s->st_mode;               memcpy(p + 16, &v, 4);
    v = (uint32_t)s->st_nlink;    memcpy(p + 20, &v, 4);
    v = s->st_uid;                memcpy(p + 24, &v, 4);
    v = s->st_gid;                memcpy(p + 28, &v, 4);
    memcpy(p + 32, &rdev, 8);
    memcpy(p + 44, &size, 8);
    v = (uint32_t)s->st_blksize;  memcpy(p + 52, &v, 4);
    memcpy(p + 56, &blocks, 8);
    v = (uint32_t)s->st_atim.tv_sec;  memcpy(p + 64, &v, 4); v = (uint32_t)s->st_atim.tv_nsec; memcpy(p + 68, &v, 4);
    v = (uint32_t)s->st_mtim.tv_sec;  memcpy(p + 72, &v, 4); v = (uint32_t)s->st_mtim.tv_nsec; memcpy(p + 76, &v, 4);
    v = (uint32_t)s->st_ctim.tv_sec;  memcpy(p + 80, &v, 4); v = (uint32_t)s->st_ctim.tv_nsec; memcpy(p + 84, &v, 4);
    memcpy(p + 88, &ino, 8);
}

static long do_rw_vec(int write, int fd, uint32_t giov, int cnt) {
    if (cnt < 0 || cnt > 1024) return -EINVAL;
    struct iovec *v = calloc((size_t)cnt, sizeof *v);
    const uint32_t *g = P(giov);
    for (int i = 0; i < cnt; i++) { v[i].iov_base = P(g[2 * i]); v[i].iov_len = g[2 * i + 1]; }
    long r = write ? writev(fd, v, cnt) : readv(fd, v, cnt);
    r = r < 0 ? -errno : r;
    free(v);
    return r;
}

/* Anonymous or file mapping into the arena. The arena is one reservation, so
 * free space is ours to track: a bump allocator from 1 GB upwards, honouring
 * a hint that lies above the watermark. Nothing is ever handed back to the
 * host -- see guest_unmap. */
static long do_mmap32(uint32_t addr, uint32_t len, int prot, int flags, int fd, uint64_t off) {
    if (len == 0) return -EINVAL;
    len = PAGE_UP(len);
    uint32_t a;
    if (flags & MAP_FIXED) {
        a = PAGE_DOWN(addr);
        if ((uint64_t)a + len > ARENA || a < 0x1000) return -EINVAL;
    } else {
        a = (addr >= g_mmap_next && (uint64_t)PAGE_DOWN(addr) + len <= 0xF0000000u) ? PAGE_DOWN(addr) : g_mmap_next;
        if ((uint64_t)a + len > 0xF0000000u) return -ENOMEM;
        g_mmap_next = a + len;
    }
    int r = guest_map(a, len, prot, fd, off);
    return r ? r : (long)a;
}

/* munmap: turn the pages back into reservation, so the host never places
 * anything of its own inside the guest's 4 GB. */
static long guest_unmap(uint32_t addr, uint32_t len) {
    if (addr & 0xFFF) return -EINVAL;
    len = PAGE_UP(len);
    if ((uint64_t)addr + len > ARENA) return -EINVAL;
    void *p = mmap(g_base + addr, len, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? -errno : 0;
}

/* Returns 1 to keep running, 0 on exit (status in *code). i386 ABI: number
 * in eax, arguments ebx ecx edx esi edi ebp, result in eax. */
int do_syscall32(xc_cpu *c, int *code, int verbose) {
    uint32_t nr = (uint32_t)c->gpr[XC_RAX];
    uint32_t a0 = (uint32_t)c->gpr[XC_RBX], a1 = (uint32_t)c->gpr[XC_RCX], a2 = (uint32_t)c->gpr[XC_RDX],
             a3 = (uint32_t)c->gpr[XC_RSI], a4 = (uint32_t)c->gpr[XC_RDI], a5 = (uint32_t)c->gpr[XC_RBP];
    long r;
    struct stat st;
    switch (nr) {
    case 1: case 252: *code = (int)a0 & 0xff; return 0;                       /* exit, exit_group */
    case 3:   r = host(SYS_read, a0, (long)P(a1), a2, 0, 0, 0); break;
    case 4:   r = host(SYS_write, a0, (long)P(a1), a2, 0, 0, 0); break;
    case 5:   r = host(SYS_openat, AT_FDCWD, (long)P(a0), (long)(a1 & ~0100000u), a2, 0, 0); break;   /* O_LARGEFILE is implicit here */
    case 295: r = host(SYS_openat, (int32_t)a0, (long)P(a1), (long)(a2 & ~0100000u), a3, 0, 0); break;
    case 6:   r = host(SYS_close, a0, 0, 0, 0, 0, 0); break;
    case 11:  r = -ENOSYS; break;                                            /* execve */
    case 12:  r = host(SYS_chdir, (long)P(a0), 0, 0, 0, 0, 0); break;
    case 13:  { time_t t = time(0); if (a0) *(uint32_t *)P(a0) = (uint32_t)t; r = (long)(uint32_t)t; break; }
    case 20:  r = getpid(); break;
    case 24: case 199: r = getuid(); break;
    case 47: case 200: r = getgid(); break;
    case 49: case 201: r = geteuid(); break;
    case 50: case 202: r = getegid(); break;
    case 64:  r = getppid(); break;
    case 224: r = getpid(); break;                                           /* gettid: one thread */
    case 33:  r = host(SYS_faccessat, AT_FDCWD, (long)P(a0), a1, 0, 0, 0); break;
    case 45: {                                                                /* brk */
        if (a0 == 0) { r = g_brk_cur; break; }
        if (a0 < g_brk_start || a0 > g_brk_max) { r = g_brk_cur; break; }
        uint32_t cur = PAGE_UP(g_brk_cur), want = PAGE_UP(a0);
        if (want > cur && guest_map(cur, want - cur, PROT_READ | PROT_WRITE, -1, 0)) { r = g_brk_cur; break; }
        g_brk_cur = a0; r = a0; break;
    }
    case 54:  r = host(SYS_ioctl, a0, a1, (long)P(a2), 0, 0, 0); break;      /* TCGETS: same layout on both ABIs */
    case 85:  r = host(SYS_readlinkat, AT_FDCWD, (long)P(a0), (long)P(a1), a2, 0, 0); break;
    case 305: r = host(SYS_readlinkat, (int32_t)a0, (long)P(a1), (long)P(a2), a3, 0, 0); break;
    case 10:  r = host(SYS_unlinkat, AT_FDCWD, (long)P(a0), 0, 0, 0, 0); break;
    case 301: r = host(SYS_unlinkat, (int32_t)a0, (long)P(a1), a2, 0, 0, 0); break;
    case 39:  r = host(SYS_mkdirat, AT_FDCWD, (long)P(a0), a1, 0, 0, 0); break;
    case 40:  r = host(SYS_unlinkat, AT_FDCWD, (long)P(a0), AT_REMOVEDIR, 0, 0, 0); break;
    case 38:  r = host(SYS_renameat, AT_FDCWD, (long)P(a0), AT_FDCWD, (long)P(a1), 0, 0); break;
    case 41:  r = host(SYS_dup, a0, 0, 0, 0, 0, 0); break;
    case 63:  r = host(SYS_dup2, a0, a1, 0, 0, 0, 0); break;
    case 330: r = host(SYS_dup3, a0, a1, a2, 0, 0, 0); break;
    case 42:  r = host(SYS_pipe2, (long)P(a0), 0, 0, 0, 0, 0); break;
    case 331: r = host(SYS_pipe2, (long)P(a0), a1, 0, 0, 0, 0); break;
    case 55: case 221: r = host(SYS_fcntl, a0, a1, a2, 0, 0, 0); break;      /* F_GETFL/F_SETFL/F_DUPFD... (no struct flock here) */
    case 60:  r = host(SYS_umask, a0, 0, 0, 0, 0, 0); break;
    case 65:  r = getpgrp(); break;
    case 94:  r = host(SYS_fchmod, a0, a1, 0, 0, 0, 0); break;
    case 118: r = host(SYS_fsync, a0, 0, 0, 0, 0, 0); break;
    case 133: r = host(SYS_fchdir, a0, 0, 0, 0, 0, 0); break;
    case 168: r = host(SYS_poll, (long)P(a0), a1, a2, 0, 0, 0); break;      /* struct pollfd is the same */
    case 194: r = host(SYS_ftruncate, a0, (long)(((uint64_t)a2 << 32) | a1), 0, 0, 0, 0); break;
    case 91:  r = guest_unmap(a0, a1); break;
    case 122: r = host(SYS_uname, (long)P(a0), 0, 0, 0, 0, 0); break;        /* same layout */
    case 125: r = host(SYS_mprotect, (long)(g_base + a0), a1, a2, 0, 0, 0); break;
    case 140: {                                                               /* _llseek */
        off_t res = lseek((int)a0, (off_t)(((uint64_t)a1 << 32) | a2), (int)a4);
        if (res < 0) { r = -errno; break; }
        int64_t v = res; memcpy(P(a3), &v, 8); r = 0; break;
    }
    case 145: r = do_rw_vec(0, (int)a0, a1, (int)a2); break;
    case 146: r = do_rw_vec(1, (int)a0, a1, (int)a2); break;
    case 174: case 175: case 186: case 311: r = 0; break;                    /* signals, robust list: accepted, ignored */
    case 192: r = do_mmap32(a0, a1, (int)a2, (int)a3, (int)a4, (uint64_t)a5 * PAGE); break;
    case 90: {                                                                /* old mmap: args in a struct */
        const uint32_t *m = P(a0);
        r = do_mmap32(m[0], m[1], (int)m[2], (int)m[3], (int)m[4], m[5]); break;
    }
    case 195: r = stat((const char *)P(a0), &st) < 0 ? -errno : 0; if (!r) put_stat64(a1, &st); break;
    case 196: r = lstat((const char *)P(a0), &st) < 0 ? -errno : 0; if (!r) put_stat64(a1, &st); break;
    case 197: r = fstat((int)a0, &st) < 0 ? -errno : 0; if (!r) put_stat64(a1, &st); break;
    case 300: r = fstatat((int32_t)a0, (const char *)P(a1), &st, (int)a3) < 0 ? -errno : 0; if (!r) put_stat64(a2, &st); break;
    case 383: r = host(SYS_statx, (int32_t)a0, (long)P(a1), a2, a3, (long)P(a4), 0); break;   /* struct statx is arch-independent */
    case 219: r = host(SYS_madvise, (long)(g_base + a0), a1, a2, 0, 0, 0); break;
    case 220: r = host(SYS_getdents64, a0, (long)P(a1), a2, 0, 0, 0); break;
    case 240: r = ((a1 & 0x7f) == 1) ? 0 : -EAGAIN; break;                   /* futex on one thread */
    case 243: {                                                               /* set_thread_area */
        uint32_t *ud = P(a0);                                                 /* entry_number, base_addr, limit, flags */
        c->gs_base = ud[1];
        if ((int32_t)ud[0] == -1) ud[0] = 12;
        r = 0; break;
    }
    case 258: g_tid_addr = a0; r = getpid(); break;
    case 265: case 403: {                                                     /* clock_gettime, clock_gettime64 */
        struct timespec ts; r = clock_gettime((clockid_t)a0, &ts) < 0 ? -errno : 0;
        if (!r && a1) {
            if (nr == 403) { int64_t v[2] = { ts.tv_sec, ts.tv_nsec }; memcpy(P(a1), v, 16); }
            else { uint32_t v[2] = { (uint32_t)ts.tv_sec, (uint32_t)ts.tv_nsec }; memcpy(P(a1), v, 8); }
        }
        break;
    }
    case 78: { struct timeval tv; r = gettimeofday(&tv, 0) < 0 ? -errno : 0;
               if (!r && a0) { uint32_t v[2] = { (uint32_t)tv.tv_sec, (uint32_t)tv.tv_usec }; memcpy(P(a0), v, 8); } break; }
    case 162: { const uint32_t *t = P(a0); struct timespec ts = { t[0], t[1] }; r = nanosleep(&ts, 0) < 0 ? -errno : 0; break; }
    case 158: r = 0; break;                                                   /* sched_yield */
    case 183: r = host(SYS_getcwd, (long)P(a0), a1, 0, 0, 0, 0); break;
    case 191: { struct rlimit rl; r = getrlimit((int)a0, &rl) < 0 ? -errno : 0;   /* ugetrlimit: 32-bit fields */
                if (!r) { uint32_t v[2] = { rl.rlim_cur > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)rl.rlim_cur,
                                            rl.rlim_max > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)rl.rlim_max }; memcpy(P(a1), v, 8); } break; }
    case 340: r = host(SYS_prlimit64, a0, a1, (long)P(a2), (long)P(a3), 0, 0); break;   /* rlimit64: same layout */
    case 355: r = host(SYS_getrandom, (long)P(a0), a1, a2, 0, 0, 0); break;
    case 386: r = -ENOSYS; break;                                             /* rseq */
    case 270: case 37: fprintf(stderr, "xrun: guest raised signal %u\n", nr == 270 ? a2 : a1); *code = 128 + (int)(nr == 270 ? a2 : a1); return 0;
    default:
        fprintf(stderr, "xrun: unimplemented i386 syscall %u at eip=%#llx\n", nr, (unsigned long long)c->rip);
        r = -ENOSYS;
    }
    if (verbose) fprintf(stderr, "  int80 %u(%#x, %#x, %#x) = %ld\n", nr, a0, a1, a2, r);
    c->gpr[XC_RAX] = (uint32_t)r;
    return 1;
}
