#!/bin/sh
# Run every guest under xrun from inside its directory (so argv[0] is stable)
# and compare with the recorded native output.
#   run.sh <xrun> <guest dir>
xrun=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); cd "$2" || exit 2; fail=0
for g in hello libc_hello; do
    exp=$(cat "$g.expected")
    got=$("$xrun" "./$g" 2>/tmp/xrun_err.$$) ; rc=$?
    if [ "$got" != "$exp" ] || [ $rc -ne 0 ]; then
        echo "FAIL $g (rc=$rc)"; echo "--- expected"; echo "$exp"; echo "--- got"; echo "$got"; cat /tmp/xrun_err.$$; fail=1
    else echo "ok   $g"; fi
done
rm -f /tmp/xrun_err.$$
exit $fail
