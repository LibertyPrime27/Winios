/* shared between the 64-bit (xrun.c) and 32-bit (linux32.c) halves */
#ifndef XRUN_H
#define XRUN_H
#include "xcore/cpu.h"
int      load_elf32(const char *path);
uint32_t build_stack32(int argc, char **argv, char **envp);
void    *arena_base32(void);
uint32_t entry32(void);
int      do_syscall32(xc_cpu *c, int *code, int verbose);
#endif
