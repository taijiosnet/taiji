#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
ziran=${ZIRAN_BIN:-"$root/../ziranlang/ziran/build/bin/ziran"}
std=${ZIRAN_STD:-"$root/../ziranlang/ziran/std"}
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
for suite in shell persistence file_plan9; do
    source_root=$rill/tests
    input=$source_root/${suite}_test.zi
    if test "$suite" = file_plan9; then
        source_root=$root/../ziranlang/ziran/tests/spec
        input=$source_root/file_plan9_test.zi
    fi
    "$ziran" ir --define NATIVE_PLAN9 --root "$source_root" \
        --module-path "$rill/src" --module-path "$std" \
        -o "$stage/$suite-ir" "$input" >>"$log" 2>&1
    "$ziran" build --target=plan9-c --define NATIVE_PLAN9 \
        --root "$source_root" --module-path "$rill/src" --module-path "$std" \
        -o "$stage/$suite-source" "$input" >>"$log" 2>&1
    "$ziran" build --target=plan9-c --define NATIVE_PLAN9 \
        --root "$stage/$suite-ir" -o "$stage/$suite-saved" \
        "$stage/$suite-ir/${suite}_test.zir" >>"$log" 2>&1
    mkdir "$stage/$suite-source/data" "$stage/$suite-saved/data"
done
# Keep the filesystem primitive checks first so failures can be distinguished
# from application parsing and interrupted-save behavior.

guest_cmd="
echo rill-ziran-plan9-start
cd $guest_stage
failed=0
for(suite in file_plan9-source file_plan9-saved shell-source shell-saved persistence-source persistence-saved) {
    cd $guest_stage/\$suite
    echo rill-ziran-plan9-suite \$suite
    for(source in *.c) {
        if(! 8c -FTVw \$source) {
            echo rill-ziran-plan9-compile-failed
            failed=1
        }
    }
    if(~ \$failed 0) {
        if(8l -o run *.8) {
            echo rill-ziran-plan9-compile-ok
            if(RILL_TEST_ROOT=$guest_stage/\$suite/data ZIRAN_TEST_ROOT=$guest_stage/\$suite/data ./run)
                echo rill-ziran-plan9-suite-ok \$suite
            if not {
                echo rill-ziran-plan9-run-failed
                failed=1
            }
        }
        if not {
            echo rill-ziran-plan9-link-failed
            failed=1
        }
    }
    if(~ \$failed 1) break
}
if(~ \$failed 0)
    echo rill-ziran-plan9-run-ok
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
    if rg -q 'rill-ziran-plan9-(compile|link|run)-failed|rill-(shell|persistence)-test-failed|file-plan9-test-failed' "$log"; then
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
