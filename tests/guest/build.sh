#!/bin/sh
# Rebuild the guest programs. Needs musl-gcc (Debian/Ubuntu: apt install musl-tools).
# static-pie so the image can load above 4 GB, where iOS has address space.
set -e
cd "$(dirname "$0")"
gcc      -O2 -static-pie -nostdlib -fno-stack-protector -o hello      hello.c
musl-gcc -O2 -static-pie -o libc_hello libc_hello.c
