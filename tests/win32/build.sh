#!/bin/sh
# Rebuild the Win32 guest programs with mingw-w64 (Debian/Ubuntu: apt install
# gcc-mingw-w64-x86-64 gcc-mingw-w64-i686). The .exe files are committed so
# CI and the device builds do not need the cross compiler.
set -e
cd "$(dirname "$0")"
x86_64-w64-mingw32-gcc -O2 -nostdlib -nostartfiles -s -e entry -o hello64.exe hello.c -lkernel32
i686-w64-mingw32-gcc   -O2 -nostdlib -nostartfiles -s -e _entry@0 -o hello32.exe hello.c -lkernel32
x86_64-w64-mingw32-gcc -O2 -s -o crt64.exe crt.c
i686-w64-mingw32-gcc   -O2 -s -o crt32.exe crt.c
x86_64-w64-mingw32-gcc -O2 -s -o nbody64.exe nbody.c
i686-w64-mingw32-gcc   -O2 -s -o nbody32.exe nbody.c
