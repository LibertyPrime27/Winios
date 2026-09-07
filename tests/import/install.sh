#!/bin/sh
# Installer mode, end to end.
#   install.sh <wimport> <guestdir> [scratch]
#
# `fakesetup` is a program that does what a silent install does, in the order
# one does it (see tests/win32/fakesetup.c). What is being checked here is the
# whole chain rather than any one link: identify the family from the bytes,
# choose that family's silent flags, get them to the guest intact, run it, tell
# what it wrote from what was already there, and pick the installed program out
# of the result.
#
# It is deliberately strict about the flags. fakesetup *fails* unless it was
# given the ones Inno Setup takes, so a broken flag table cannot pass by
# installing anyway.
set -e
wimport=$1; guests=$2
scratch=${3:-${TMPDIR:-/tmp}/wimport-test.$$}
fail=0

for bits in 32 64; do
    setup="$guests/fakesetup$bits.exe"
    [ -f "$setup" ] || { echo "FAIL fakesetup$bits.exe is not built"; fail=1; continue; }
    drive="$scratch/c$bits"
    rm -rf "$drive"; mkdir -p "$drive"

    out=$("$wimport" install "$setup" "$drive" 2>&1) || true

    # The chain, one claim at a time. Grepping for each rather than diffing the
    # whole thing: the report contains file sizes and a path, and recording
    # those would be recording this machine.
    # printf, not echo: sh's echo interprets backslash escapes, and every
    # pattern here is a Windows path -- so a failure message would report
    # "C:akesetup32" and send you looking for the wrong bug.
    check() {  # description pattern
        if printf '%s\n' "$out" | grep -qF "$2"; then printf 'ok   %s-bit: %s\n' "$bits" "$1"
        else printf 'FAIL %s-bit: %s (looking for "%s")\n' "$bits" "$1" "$2"; fail=1; fi
    }
    check "identified as Inno Setup"        "Inno Setup"
    # The two that a real NSIS installer stopped on, on a real device: the
    # DLL-search narrowing it does first, and the older shell-folder pair it
    # resolves a Start Menu path through.
    check "narrowed the DLL search path"    "dllsearch: narrowed"
    check "resolved the Start Menu"         "menu:  C:\\Users\\Winios\\Start Menu\\Programs"
    check "given the silent flags"          "/SILENT /SP- /NORESTART /DIR=C:\\fakesetup$bits"
    check "the guest saw them"              "silent=1 sp-=1 norestart=1"
    check "free space was reported"         "free:  enough"
    check "Program Files was resolved"      "progs: C:\\Program Files"
    check "the .ini read back"              "Display/Width = 1920"
    check "it finished"                     "The installer exited 0"
    check "the install was found"            "fakesetup$bits/FakeGame.exe"
    check "and its data file"                "fakesetup$bits/data/assets.dat"
    check "the temp file was not counted"   "temporary files and the registry ignored"
    check "an executable was chosen"        "Will run: FakeGame.exe"

    # And the point of all of it: the installed program is one we can run.
    if [ -f "$drive/fakesetup$bits/FakeGame.exe" ]; then
        echo "ok   $bits-bit: the installed executable is on the drive"
    else
        echo "FAIL $bits-bit: nothing was installed"; fail=1
        printf '%s\n' "$out" | tail -20 | sed 's/^/  | /'
    fi
    # The registry key an uninstaller would look for.
    if grep -q "Uninstall" "$drive/registry.txt" 2>/dev/null; then
        echo "ok   $bits-bit: the uninstall key was written"
    else
        echo "FAIL $bits-bit: no uninstall key in the registry"; fail=1
    fi
    rm -rf "$drive"
done

# --- and the same installer, run the way a person sees it ---------------------
#
# No silent flags: fakesetup puts up its own dialog out of its own resources,
# and installs where the dialog says rather than where we asked. Nobody clicks
# it here -- it clicks its own Install button -- so what this checks is that
# the template was found and walked, the controls were made, the dialog was
# drawn, and a click came back as a WM_COMMAND. A finger on the glass is the
# one part a test cannot supply.
#
# The destination is deliberately awkward: Program Files\Fake Game NN\bin. An
# importer that named the library entry after the folder holding the .exe would
# call it "bin", and one that assumed the folder sits at the top of the drive
# would point at nothing. Both are the failure a person would report as "it
# installed and then vanished".
for bits in 32 64; do
    setup="$guests/fakesetup$bits.exe"
    [ -f "$setup" ] || continue
    drive="$scratch/v$bits"
    rm -rf "$drive"; mkdir -p "$drive"

    out=$("$wimport" install "$setup" "$drive" -visible -t 120 2>&1) || true
    checkv() {
        if printf '%s\n' "$out" | grep -qF "$2"; then echo "ok   $bits-bit visible: $1"
        else echo "FAIL $bits-bit visible: $1"; fail=1
             printf '%s\n' "$out" | tail -25 | sed 's/^/  | /'; fi
    }
    checkv "it was run with no arguments"    "Running: fakesetup$bits.exe"
    checkv "the dialog chose the destination" "visible: installing to C:\\Program Files\\Fake Game $bits\\bin"
    checkv "the install was found"           "Program Files/Fake Game $bits/bin/FakeGame.exe"
    checkv "the entry is named after the game, not the bin folder" \
                                             'added to your library as "Fake Game '"$bits"'"'
    checkv "and it runs the executable below it" "Will run: bin/FakeGame.exe"

    if [ -f "$drive/Program Files/Fake Game $bits/bin/FakeGame.exe" ]; then
        echo "ok   $bits-bit visible: the installed executable is on the drive"
    else
        echo "FAIL $bits-bit visible: nothing was installed"; fail=1
    fi
    rm -rf "$drive"
done

rmdir "$scratch" 2>/dev/null || true
exit $fail
