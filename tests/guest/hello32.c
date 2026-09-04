/* The 32-bit first guest: int 0x80, no libc. Runs in the 4 GB arena. */
static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
void _start(void) {
    static const char msg[] = "hello from 32-bit xcore\n";
    sys3(4, 1, (long)msg, sizeof msg - 1);
    sys3(1, 0, 0, 0);
    for (;;) {}
}
