#!/bin/sh
# Run every Windows guest under winrun and compare stdout and the exit code
# with the recorded expectations (recorded from a run whose values were
# checked by hand / against the Linux build of the same source).
#   run.sh <winrun> <dir>
winrun=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); cd "$2" || exit 2; fail=0
# C:\ for the guests that use absolute paths. Everything else ignores it.
WINRUN_DRIVE_C=$PWD/cdrive; export WINRUN_DRIVE_C
# The registry persists on purpose, so start every suite run from nothing --
# otherwise the first run records the expectations and the second contradicts them.
rm -f cdrive/registry.txt
check() {   # name expected_rc args...
    name=$1; erc=$2; shift 2
    checkx "$name" "${name%.exe}.expected" "$erc" "$@"
}
# Same, but the expectation file is named separately: a guest run twice with
# different arguments has two outputs and only one name.
# Arguments after a bare `--` are winrun's own flags rather than the guest's,
# so a case can ask for an input script without needing another helper.
checkx() {  # name expected_file expected_rc [guest args...] [-- winrun flags...]
    name=$1; expf=$2; erc=$3; shift 3
    gargs=""; wargs=""; seen=0
    for t in "$@"; do
        if [ "$t" = "--" ]; then seen=1; continue; fi
        if [ $seen -eq 1 ]; then wargs="$wargs $t"; else gargs="$gargs $t"; fi
    done
    got=$("$winrun" $wargs "./$name" $gargs 2>/tmp/winrun_err.$$); rc=$?
    exp=$(cat "$expf")
    if [ "$got" != "$exp" ] || [ "$rc" -ne "$erc" ]; then
        echo "FAIL $name (rc=$rc, want $erc)"; echo "--- expected"; echo "$exp"; echo "--- got"; echo "$got"; cat /tmp/winrun_err.$$; fail=1
        # diagnostics: is it the JIT? what does -v say?
        got2=$(XCORE_JIT=0 "$winrun" $wargs "./$name" $gargs 2>/dev/null); rc2=$?
        [ "$got2" = "$exp" ] && [ "$rc2" -eq "$erc" ] && echo "  (passes with XCORE_JIT=0: JIT-specific)" || echo "  (also fails with XCORE_JIT=0, rc=$rc2)"
        "$winrun" $wargs -v "./$name" $gargs 2>&1 >/dev/null | tail -24 | sed 's/^/  | /'
    else echo "ok   $name"; fi
}
# Only the exit code and one line of output. These runs end in the crash
# report, which prints registers and addresses that differ between the
# interpreter and the dynarec by design -- recording it would be recording
# the machine, not the behaviour.
checkrc() {  # name expected_rc must_contain args...
    name=$1; erc=$2; want=$3; shift 3
    got=$("$winrun" "./$name" "$@" 2>/tmp/winrun_err.$$); rc=$?
    if [ "$rc" -ne "$erc" ] || ! printf '%s\n' "$got" | grep -qF "$want"; then
        echo "FAIL $name $* (rc=$rc, want $erc; looking for \"$want\")"
        printf '%s\n' "$got" | head -8; cat /tmp/winrun_err.$$; fail=1
    else echo "ok   $name $* (ended with $erc)"; fi
}
check hello64.exe 7 a b
check hello32.exe 7 a b
check crt64.exe 3
check crt32.exe 3
check nbody64.exe 0
check nbody32.exe 0
check dlltest64.exe 0
check dlltest32.exe 0
check d3dtest64.exe 0
check d3dtest32.exe 0
check d3dframe64.exe 0
check d3dframe32.exe 0
check d3dloop64.exe 0 12
check d3dloop32.exe 0 12
check pathtest64.exe 0
check pathtest32.exe 0
check d3ddraw64.exe 0
check d3ddraw32.exe 0
check filetest64.exe 0
check filetest32.exe 0
# The registry has to survive the process that wrote it, so write in one run
# and read in the next. The read half deletes what it found, so the pair is
# repeatable and the 64-bit pair below starts from the same empty store.
checkx regtest32.exe regtest_write32.expected 0 write
checkx regtest32.exe regtest_read32.expected  0 read
checkx regtest64.exe regtest_write64.expected 0 write
checkx regtest64.exe regtest_read64.expected  0 read
# Structured exception handling. Software exceptions -- RaiseException, and so
# every 32-bit MSVC C++ throw -- work the same through the interpreter and the
# dynarec, because they arrive through a stub with the registers spilled.
check sehtest64.exe 0
check sehtest32.exe 0
checkrc sehtest64.exe 129 "an unhandled exception" die
checkrc sehtest32.exe 129 "an unhandled exception" die
# Faults are the other half. A fault inside a compiled block is recovered
# through the recovery stub that block carries (core/src/jit/jit.c), so these
# run the same way as everything else -- and are then run again with the JIT
# off, because "the same either way" is the property that matters and it is
# only proved by checking both.
check faulttest64.exe 0
check faulttest32.exe 0
checkrc faulttest64.exe 129 "an unhandled exception" die
checkrc faulttest32.exe 129 "an unhandled exception" die
XCORE_JIT=0; export XCORE_JIT
check faulttest64.exe 0
check faulttest32.exe 0
checkrc faulttest64.exe 129 "an unhandled exception" die
checkrc faulttest32.exe 129 "an unhandled exception" die
unset XCORE_JIT
# A window, a message pump and input. The events come from a script aimed at
# particular frames (`winrun -input`), because input needs a driver and a
# recording of the tester's reflexes is not a test.
check inputtest64.exe 0 6 -- -input inputtest.script
check inputtest32.exe 0 6 -- -input inputtest.script
# What -k returns from a function we do not have.
#
# Its own block rather than one of the helpers above, because the two runs
# want opposite outcomes from the same guest: without -k it must stop at the
# first missing function (127), and with -k it must get past three of them,
# use the result of one as a pointer, and exit 0.
#
# The report is not compared, because it contains registers -- what is
# checked is the exit code and that all three names reached the list. A run
# that returned zero from a pointer-returning function faulted on the first
# one, which is what this exists to stop happening again.
for b in 64 32; do
    rcs=$("$winrun" "./keepgoing$b.exe" >/dev/null 2>&1; echo $?)
    if [ "$rcs" = "127" ]; then echo "ok   keepgoing$b.exe stops at the first missing function"
    else echo "FAIL keepgoing$b.exe without -k exited $rcs, want 127"; fail=1; fi

    outk=$("$winrun" -k "./keepgoing$b.exe" 2>&1); rck=$?
    missing=$(printf '%s\n' "$outk" | grep -c 'user32.dll!\(SwitchDesktop\|LockWorkStation\|CreateDesktopW\)')
    if [ "$rck" = "0" ] && [ "$missing" = "3" ] && printf '%s\n' "$outk" | grep -q '0 failures'; then
        echo "ok   keepgoing$b.exe -k names all three and the guest survives the answers"
    else
        echo "FAIL keepgoing$b.exe -k exited $rck, listed $missing of 3"; fail=1
        printf '%s\n' "$outk" | head -12 | sed 's/^/  | /'
    fi
done
# Walking and casing strings. Small functions, and one of them missing was
# the whole distance between "nothing was installed" and an install: a
# Unicode installer walks every path through CharNextW. The surrogate cases
# are the ones worth having a test for -- a wrong step there corrupts a path
# quietly instead of failing.
check chartest64.exe 0
check chartest32.exe 0
# A dialog, from a real RT_DIALOG resource in the guest's own image, drawn.
# The guest checks what a guest can check -- controls found by id, text set
# and read back, a click coming back as WM_COMMAND -- and `-frame` checks
# what it cannot: the pixels. The checksum is over the frame the app would
# have been handed, so it covers the loader walking the resource directory,
# the dialog-unit conversion, every control's own drawing, and the fact that
# the finished frame was actually presented rather than merely composed.
#
# The arithmetic behind those pixels is all integer, deliberately, so the
# checksum is the same on an x86 runner, under qemu on aarch64, and on a
# phone. A difference is a bug, not a rounding.
check dlgtest64.exe 0 -- -frame -screen 640x400
check dlgtest32.exe 0 -- -frame -screen 640x400
# Audio and a gamepad. Nothing makes a sound and no controller is attached --
# what is checked is that a game's audio init succeeds (many abort when it
# fails), that a sound buffer is memory the guest can write and read back,
# that the play cursor advances at the rate the format implies, and that the
# keyboard reaches XInput.
check audiotest64.exe 0 -- -input audiotest.script
check audiotest32.exe 0 -- -input audiotest.script
# Threads. Every line of the expected output is true under every interleaving
# -- "four threads each added 400, so the total is 1600" -- so a pass means
# the locking held, not that the scheduler happened to be kind. Run twice for
# the same reason faulttest is: the dynarec hands the guest lock over at a
# block boundary and the interpreter at an instruction, so they interleave
# differently and only checking both proves the answer does not depend on it.
check threadtest64.exe 0
check threadtest32.exe 0
XCORE_JIT=0; export XCORE_JIT
check threadtest64.exe 0
check threadtest32.exe 0
unset XCORE_JIT
rm -f /tmp/winrun_err.$$
exit $fail
