/* The first guest: no libc. Two syscalls and a string. If xcore cannot run
 * this, nothing else matters. */
static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
void _start(void) {
    static const char msg[] = "hello from xcore\n";
    sys3(1, 1, (long)msg, sizeof msg - 1);
    sys3(60, 0, 0, 0);
    for (;;) {}
}
