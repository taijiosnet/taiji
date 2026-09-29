#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
ziran=${ZIRAN_BIN:-"$root/../ziranlang/ziran/build/bin/ziran"}
rill=${RILL_DIR:-"$root/sys/src/cmd/rill"}
timeout=${TAIJI_RILL_ZIRAN_TIMEOUT:-240}
log=$root/build/rill-ziran-plan9.log
mkdir -p build usr/glenda/tmp
exec 9>build/rill-ziran-plan9.lock
flock 9
stage=$(mktemp -d "$root/usr/glenda/tmp/rill-ziran-plan9.XXXXXX")
guest_stage=/usr/glenda/tmp/${stage##*/}
cleanup() { rm -rf "$stage"; }
trap cleanup EXIT HUP INT TERM
unset DISPLAY WAYLAND_DISPLAY RILL_CONTAINED_X11

: >"$log"
"$ziran" build --target=plan9-c --define NATIVE_PLAN9 \
    --root "$rill/tests" --module-path "$rill/src" \
    -o "$stage/generated" "$rill/tests/shell_test.zi" >>"$log" 2>&1

guest_cmd="
echo rill-ziran-plan9-start
cd $guest_stage/generated
failed=0
for(source in *.c) {
    if(! 8c -FTVw \$source) {
        echo rill-ziran-plan9-compile-failed
        failed=1
    }
}
if(~ \$failed 0) {
    if(8l -o ../rill-shell-test *.8) {
        echo rill-ziran-plan9-compile-ok
        if(../rill-shell-test)
            echo rill-ziran-plan9-run-ok
        if not
            echo rill-ziran-plan9-run-failed
    }
    if not
        echo rill-ziran-plan9-link-failed
}
fshalt
"

setsid env -u DISPLAY -u WAYLAND_DISPLAY Q9_BOOT_TIMEOUT="$timeout" \
    ./q9 --raw tty-run "$guest_cmd" >>"$log" 2>&1 &
vm_pid=$!
stop_vm() {
    kill -TERM -"$vm_pid" 2>/dev/null || kill -TERM "$vm_pid" 2>/dev/null || true
    wait "$vm_pid" 2>/dev/null || true
}
trap 'stop_vm; cleanup' EXIT HUP INT TERM
start=$(date +%s)
while [ "$(( $(date +%s) - start ))" -lt "$timeout" ]; do
    if rg -q '^rill-ziran-plan9-run-ok' "$log"; then
        stop_vm
        trap cleanup EXIT HUP INT TERM
        echo "rill-ziran-plan9: ok ($(( $(date +%s) - start ))s, native 8c/8l)"
        exit 0
    fi
    if rg -q 'rill-ziran-plan9-(compile|link|run)-failed|rill-shell-test-failed' "$log"; then
        tail -70 "$log" >&2
        exit 1
    fi
    if ! kill -0 "$vm_pid" 2>/dev/null; then
        echo 'rill-ziran-plan9: VM exited before the result' >&2
        tail -70 "$log" >&2
        exit 1
    fi
    sleep 2
done
echo "rill-ziran-plan9: timed out after ${timeout}s" >&2
tail -70 "$log" >&2
exit 1
