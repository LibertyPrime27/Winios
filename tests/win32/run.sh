#!/bin/sh
# Run every Windows guest under winrun and compare stdout and the exit code
# with the recorded expectations (recorded from a run whose values were
# checked by hand / against the Linux build of the same source).
#   run.sh <winrun> <dir>
winrun=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); cd "$2" || exit 2; fail=0
check() {   # name expected_rc args...
    name=$1; erc=$2; shift 2
    got=$("$winrun" "./$name" "$@" 2>/tmp/winrun_err.$$); rc=$?
    exp=$(cat "${name%.exe}.expected")
    if [ "$got" != "$exp" ] || [ "$rc" -ne "$erc" ]; then
        echo "FAIL $name (rc=$rc, want $erc)"; echo "--- expected"; echo "$exp"; echo "--- got"; echo "$got"; cat /tmp/winrun_err.$$; fail=1
    else echo "ok   $name"; fi
}
check hello64.exe 7 a b
check hello32.exe 7 a b
check crt64.exe 3
check crt32.exe 3
check nbody64.exe 0
check nbody32.exe 0
rm -f /tmp/winrun_err.$$
exit $fail
