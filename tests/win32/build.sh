#!/bin/sh
# Rebuild the Win32 guest programs with mingw-w64 (Debian/Ubuntu: apt install
# gcc-mingw-w64-x86-64 gcc-mingw-w64-i686). The .exe and .dll files are
# committed so CI and the device builds do not need the cross compiler.
set -e
cd "$(dirname "$0")"
x86_64-w64-mingw32-gcc -O2 -nostdlib -nostartfiles -s -e entry -o hello64.exe hello.c -lkernel32
i686-w64-mingw32-gcc   -O2 -nostdlib -nostartfiles -s -e _entry@0 -o hello32.exe hello.c -lkernel32
x86_64-w64-mingw32-gcc -O2 -s -o crt64.exe crt.c
i686-w64-mingw32-gcc   -O2 -s -o crt32.exe crt.c
x86_64-w64-mingw32-gcc -O2 -s -o nbody64.exe nbody.c
i686-w64-mingw32-gcc   -O2 -s -o nbody32.exe nbody.c

# The DLL chain: dlltest.exe -> mid.dll -> sub.dll, plus late.dll which is
# only ever reached through LoadLibrary. 32- and 64-bit images live in the
# same directory, so the names carry the bitness.
for a in 32 64; do
    [ $a = 32 ] && CC=i686-w64-mingw32-gcc || CC=x86_64-w64-mingw32-gcc
    sed "s/sub32\./sub$a./" dlllate.def > dlllate$a.def
    $CC -O2 -shared -s -o sub$a.dll  dllsub.c  dllsub.def       -Wl,--out-implib,libsub$a.a
    $CC -O2 -shared -s -o mid$a.dll  dllmid.c  -L. -l:libsub$a.a -Wl,--out-implib,libmid$a.a
    $CC -O2 -shared -s -o late$a.dll dlllate.c dlllate$a.def -L. -l:libsub$a.a
    $CC -O2 -s -DSUBDLL=\"sub$a.dll\" -DLATEDLL=\"late$a.dll\" \
        -o dlltest$a.exe dlltest.c -L. -l:libmid$a.a -l:libsub$a.a
    rm -f dlllate$a.def libsub$a.a libmid$a.a
done
