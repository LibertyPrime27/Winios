#!/bin/sh
# Run every guest under xrun from inside its directory (so argv[0] is stable)
# and compare with the recorded native output. If a 32-bit compiler is
# available, also build the libc guest as a static i386 glibc binary and
# compare xrun against the native run -- the CI oracle for the arena model.
#   run.sh <xrun> <guest dir>
xrun=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); cd "$2" || exit 2; fail=0
for g in hello libc_hello hello32; do
    exp=$(cat "$g.expected")
    got=$("$xrun" "./$g" 2>/tmp/xrun_err.$$) ; rc=$?
    if [ "$got" != "$exp" ] || [ $rc -ne 0 ]; then
        echo "FAIL $g (rc=$rc)"; echo "--- expected"; echo "$exp"; echo "--- got"; echo "$got"; cat /tmp/xrun_err.$$; fail=1
    else echo "ok   $g"; fi
done
tmp=$(mktemp -d)
if gcc -m32 -static -O2 -o "$tmp/lh32" libc_hello.c 2>/dev/null; then
    exp=$("$tmp/lh32" x y); rc1=$?
    got=$("$xrun" "$tmp/lh32" x y 2>/tmp/xrun_err.$$); rc2=$?
    if [ "$got" != "$exp" ] || [ $rc1 -ne $rc2 ]; then
        echo "FAIL libc_hello (i386 glibc static) rc native=$rc1 xrun=$rc2"; echo "$got"; cat /tmp/xrun_err.$$; fail=1
    else echo "ok   libc_hello (i386 glibc static, built here)"; fi
else echo "skip libc_hello i386: no 32-bit toolchain"; fi
rm -rf "$tmp" /tmp/xrun_err.$$
exit $fail
