#!/bin/sh
# Rebuild the guest programs. Needs musl-gcc (Debian/Ubuntu: apt install
# musl-tools) and, for the 32-bit ones, gcc-multilib.
# 64-bit guests are static-pie so the image can load above 4 GB, where iOS
# has address space; 32-bit guests live in the arena and link normally.
set -e
cd "$(dirname "$0")"
gcc      -O2 -static-pie -nostdlib -fno-stack-protector -o hello      hello.c
musl-gcc -O2 -static-pie -o libc_hello libc_hello.c
gcc -m32 -O2 -static -nostdlib -fno-stack-protector -o hello32 hello32.c
