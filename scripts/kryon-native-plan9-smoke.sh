#!/bin/sh

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

ziran=${ZIRAN_BIN:-"$root/../ziran/build/bin/ziran"}
kryon=$root/sys/src/kryon
stage=$root/usr/glenda/tmp/kryon-native-plan9
log=$root/build/kryon-native-plan9.log
timeout=${TAIJI_KRYON_NATIVE_TIMEOUT:-240}

mkdir -p build usr/glenda/tmp
rm -rf "$stage"
mkdir -p "$stage"
: >"$log"

cleanup()
{
    rm -rf "$stage"
}
trap cleanup EXIT HUP INT TERM

exec 9>build/kryon-native-plan9.lock
flock 9

test -x "$root/q9"
test -x "$ziran"
test -f "$kryon/tests/libdraw_native_plan9_host.zi"

"$ziran" build --target=plan9-c --root "$kryon/tests" \
    --module-path "$kryon/src/backend" --module-path "$kryon/src/ui" \
    --module-path "$root/../ziran/std" -o "$stage/generated" \
    "$kryon/tests/libdraw_native_plan9_host.zi" >>"$log" 2>&1

guest_cmd='
echo kryon-native-plan9-start
cd /usr/glenda/tmp/kryon-native-plan9/generated
if(8c -FTVw -o c_string.8 c_string.c &&
   8c -FTVw -o drawing_props.8 drawing_props.c &&
   8c -FTVw -o geometry.8 geometry.c &&
   8c -FTVw -o libdraw_native.8 libdraw_native.c &&
   8c -FTVw -o libdraw_native_plan9_host.8 libdraw_native_plan9_host.c &&
   8c -FTVw -o math.8 math.c &&
   8c -FTVw -o semantic.8 semantic.c &&
   8c -FTVw -o session.8 session.c &&
   8c -FTVw -o text_align.8 text_align.c &&
   8c -FTVw -o tree.8 tree.c &&
   8c -FTVw -o tree_input.8 tree_input.c &&
   8c -FTVw -o vec.8 vec.c &&
   8c -FTVw -o widget_kind.8 widget_kind.c &&
   8l -o ../kryon-native-plan9-probe c_string.8 drawing_props.8 geometry.8 libdraw_native.8 libdraw_native_plan9_host.8 math.8 semantic.8 session.8 text_align.8 tree.8 tree_input.8 vec.8 widget_kind.8 -ldraw) {
    echo kryon-native-plan9-compile-ok
    font=/lib/font/bit/pelm/latin1.8.font
    KRYON_OFFSCREEN=1
    rm -f /usr/glenda/tmp/kryon-native-plan9/capture.rgba
    KRYON_CAPTURE_PATH=/usr/glenda/tmp/kryon-native-plan9/capture.rgba ../kryon-native-plan9-probe
    if(test -s /usr/glenda/tmp/kryon-native-plan9/capture.rgba)
        echo kryon-native-plan9-run-ok
    if not
        echo kryon-native-plan9-run-failed
}
if not
    echo kryon-native-plan9-compile-failed
fshalt
'

start=$(date +%s)
setsid env -u DISPLAY -u WAYLAND_DISPLAY Q9_BOOT_TIMEOUT="$timeout" \
    TMPDIR=/tmp ./q9 --raw tty-run "$guest_cmd" >"$log" 2>&1 &
q9_pid=$!

stop_vm()
{
    kill -TERM -"$q9_pid" 2>/dev/null || kill -TERM "$q9_pid" 2>/dev/null || true
    wait "$q9_pid" 2>/dev/null || true
}
trap 'stop_vm; cleanup' EXIT HUP INT TERM

i=0
while [ "$i" -lt "$timeout" ]; do
    if grep -q 'kryon-native-plan9-run-ok' "$log"; then
        stop_vm
        trap cleanup EXIT HUP INT TERM
        capture=$stage/capture.rgba
        size=$(wc -c <"$capture")
        bytes=$(od -An -tx1 -N4 "$capture" | tr -d ' \n')
        if [ "$size" -ne 24576 ] || [ "$bytes" != fe0000ff ]; then
            echo "kryon-native-plan9: invalid capture size=$size bytes=$bytes" >&2
            exit 1
        fi
        elapsed=$(( $(date +%s) - start ))
        echo "kryon-native-plan9: ok (${elapsed}s, ${size} bytes, $bytes)"
        exit 0
    fi
    if grep -q 'kryon-native-plan9-.*-failed' "$log"; then
        echo 'kryon-native-plan9: failed; tail follows' >&2
        tail -80 "$log" >&2
        exit 1
    fi
    if ! kill -0 "$q9_pid" 2>/dev/null; then
        echo 'kryon-native-plan9: q9 exited before success marker; tail follows' >&2
        tail -80 "$log" >&2
        exit 1
    fi
    sleep 5
    i=$(( i + 5 ))
done

echo "kryon-native-plan9: timed out after ${timeout}s; tail follows" >&2
tail -80 "$log" >&2
exit 1
