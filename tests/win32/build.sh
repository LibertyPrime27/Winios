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
    # A dialog with a real resource. windres compiles the .rc into an
    # RT_DIALOG the loader has to walk -- writing the template by hand in C
    # would test the parser against our own idea of the format rather than
    # against what a resource compiler emits.
    [ $a = 32 ] && RC=i686-w64-mingw32-windres || RC=x86_64-w64-mingw32-windres
    $RC dlgtest.rc dlgres$a.o
    $CC -O2 -s -o dlgtest$a.exe dlgtest.c dlgres$a.o -lgdi32 -lcomctl32 -luser32
    rm -f dlgres$a.o
    # Direct3D 9 through d3d9.dll: COM vtables in guest memory
    $CC -O2 -s -o d3dtest$a.exe  d3dtest.c  -ld3d9
    $CC -O2 -s -o d3dframe$a.exe d3dframe.c -ld3d9
    $CC -O2 -s -o d3dloop$a.exe  d3dloop.c  -ld3d9
    $CC -O2 -s -o d3ddraw$a.exe  d3ddraw.c  -ld3d9
    $CC -O2 -s -o pathtest$a.exe pathtest.c
    # finding files, mapping them, and the registry -- how a game finds its
    # archives and how it finds out where it was installed
    $CC -O2 -s -o filetest$a.exe filetest.c
    $CC -O2 -s -o regtest$a.exe  regtest.c -ladvapi32
    # structured exception handling: the dispatch, and faults
    $CC -O2 -s -o sehtest$a.exe   sehtest.c
    $CC -O2 -s -o faulttest$a.exe faulttest.c
    # a window, a message pump, a keyboard and a mouse
    $CC -O2 -s -o inputtest$a.exe inputtest.c -ld3d9 -luser32
    # audio that initialises, and a gamepad the keyboard can stand in for
    $CC -O2 -s -o audiotest$a.exe audiotest.c -ldsound -lxinput -lole32
    # threads: CreateThread and the CRT's own, critical sections, events,
    # mutexes, interlocked, TLS. Written so every check is true under every
    # interleaving, so a pass means locking works rather than that the
    # scheduler happened to be kind (see the file).
    $CC -O2 -s -o threadtest$a.exe threadtest.c
    # Not in the suite: the same increment 25000 times with no forced
    # handover. It is how the lost update was found; it is not a test,
    # because a passing run proves nothing.
    $CC -O2 -s -o threadstress$a.exe threadstress.c
    # A program that behaves like a silent install, for testing the importer
    # against: it checks free space, resolves the shell folders, makes
    # directories, copies files, keeps settings in an .ini and writes an
    # uninstall key -- and refuses to do any of it unless it was handed the
    # flags the Inno Setup family takes. Not in the guest suite: it is driven
    # by tests/import/install.sh through wimport, which is what exercises the
    # detection and the flag table as well as the run.
    $RC fakesetup.rc fakesetupres$a.o
    $CC -O2 -s -o fakesetup$a.exe fakesetup.c fakesetupres$a.o -ladvapi32 -lshell32 -lole32 -lcomctl32 -luser32
    rm -f fakesetupres$a.o
done

# Not run by the suite: it calls things we do not implement, on purpose.
# `winrun -imports tests/win32/gamelike32.exe` names them, and that list is
# the work between here and a real game.
i686-w64-mingw32-gcc -O2 -s -o gamelike32.exe gamelike.c -luser32 -lgdi32 -ladvapi32 -lwinmm
