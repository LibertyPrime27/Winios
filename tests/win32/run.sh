#!/bin/sh
# Run every Windows guest under winrun and compare stdout and the exit code
# with the recorded expectations (recorded from a run whose values were
# checked by hand / against the Linux build of the same source).
#   run.sh <winrun> <dir>
winrun=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); cd "$2" || exit 2; fail=0
# C:\ for the guests that use absolute paths. Everything else ignores it.
WINRUN_DRIVE_C=$PWD/cdrive; export WINRUN_DRIVE_C
check() {   # name expected_rc args...
    name=$1; erc=$2; shift 2
    got=$("$winrun" "./$name" "$@" 2>/tmp/winrun_err.$$); rc=$?
    exp=$(cat "${name%.exe}.expected")
    if [ "$got" != "$exp" ] || [ "$rc" -ne "$erc" ]; then
        echo "FAIL $name (rc=$rc, want $erc)"; echo "--- expected"; echo "$exp"; echo "--- got"; echo "$got"; cat /tmp/winrun_err.$$; fail=1
        # diagnostics: is it the JIT? what does -v say?
        got2=$(XCORE_JIT=0 "$winrun" "./$name" "$@" 2>/dev/null); rc2=$?
        [ "$got2" = "$exp" ] && [ "$rc2" -eq "$erc" ] && echo "  (passes with XCORE_JIT=0: JIT-specific)" || echo "  (also fails with XCORE_JIT=0, rc=$rc2)"
        "$winrun" -v "./$name" "$@" 2>&1 >/dev/null | tail -24 | sed 's/^/  | /'
    else echo "ok   $name"; fi
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
rm -f /tmp/winrun_err.$$
exit $fail
