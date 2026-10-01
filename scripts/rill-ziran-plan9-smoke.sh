#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
ziran=${ZIRAN_BIN:-"$root/../../ziranlang/ziran/build/bin/ziran"}
std=${ZIRAN_STD:-"$root/../../ziranlang/ziran/std"}
rill=${RILL_DIR:-"$root/sys/src/cmd/rill"}
kryon=${KRYON_DIR:-"$root/sys/src/kryon"}
timeout=${TAIJI_RILL_ZIRAN_TIMEOUT:-1800}
suites_only=${TAIJI_RILL_ZIRAN_SUITES_ONLY:-0}
case "$suites_only" in 0|1) ;; *) echo "SUITES_ONLY must be 0 or 1" >&2; exit 1 ;; esac
applications=${TAIJI_RILL_ZIRAN_APPLICATIONS:-"run applications calendar desktop settings about"}
if test "$suites_only" = 1; then applications=; fi
for application in $applications; do
    case "$application" in
        run|applications|calendar|desktop|settings|about) ;;
        *) echo "unknown native application: $application" >&2; exit 1 ;;
    esac
done
log=$root/build/rill-ziran-plan9.log
mkdir -p build usr/glenda/tmp
exec 9>build/rill-ziran-plan9.lock
flock 9
stage=$(mktemp -d "$root/usr/glenda/tmp/rill-ziran-plan9.XXXXXX")
guest_stage=/usr/glenda/tmp/${stage##*/}
cleanup() {
    python3 - "$stage" <<'PY'
import os
import shutil
import stat
import sys

stage = sys.argv[1]
if os.path.isdir(stage) and not os.path.islink(stage):
    # This unique directory contains only this gate's disposable outputs.
    # Restore directory traversal/deletion rights without following links.
    for current, directories, _ in os.walk(stage, topdown=True, followlinks=False):
        os.chmod(current, stat.S_IMODE(os.stat(current).st_mode) | 0o700)
        for name in directories:
            path = os.path.join(current, name)
            if not os.path.islink(path):
                os.chmod(path, stat.S_IMODE(os.stat(path).st_mode) | 0o700)
    shutil.rmtree(stage)
PY
}
trap cleanup EXIT HUP INT TERM
unset DISPLAY WAYLAND_DISPLAY RILL_CONTAINED_X11
open_output=$rill/build/ziran/open-plan9
if test "$suites_only" = 1; then open_output=$stage/open-plan9; fi

: >"$log"
"$ziran" build --target=plan9-c --root "$root/sys/src/cmd/rio9" \
    --module-path "$std" -o "$root/sys/src/cmd/rio9/build/ziran/plan9" \
    "$root/sys/src/cmd/rio9/window_snapshot.zi" >>"$log" 2>&1
"$ziran" build --target=plan9-c --define NATIVE_PLAN9 --root "$rill/src" \
    --module-path "$std" -o "$rill/build/ziran/plan9" \
    "$rill/src/shell.zi" "$rill/src/panel.zi" "$rill/src/settings.zi" \
    "$rill/src/platform_plan9.zi" "$rill/src/run_dialog.zi" "$rill/src/applications.zi" "$rill/src/clock.zi" >>"$log" 2>&1
"$ziran" build --target=plan9-c --define NATIVE_PLAN9 --root "$rill/tests" \
    --module-path "$rill/src" --module-path "$std" \
    -o "$rill/build/ziran/plan9-test" "$rill/tests/persistence_test.zi" >>"$log" 2>&1
"$ziran" build --target=plan9-c --define NATIVE_PLAN9 --root "$rill/app" \
    --module-path "$rill/src" --module-path "$std" \
    -o "$open_output" "$rill/app/open_main.zi" >>"$log" 2>&1
for application in $applications; do
    "$ziran" build --target=plan9-c --define NATIVE_PLAN9 --define PLAN9_BUILD \
        --root "$rill/app" --module-path "$rill/src" \
        --module-path "$kryon/src/ui" --module-path "$kryon/src/backend" --module-path "$std" \
        -o "$rill/build/ziran/$application-plan9" "$rill/app/${application}_main.zi" >>"$log" 2>&1
    mkdir "$stage/$application-app-home"
done
mkdir "$stage/app-bin"
rill_objects=
for source in "$rill"/build/ziran/plan9/*.c; do
    module=${source##*/}
    rill_objects="$rill_objects build/ziran/plan9/${module%.c}.8"
done
suites=${TAIJI_RILL_ZIRAN_SUITES:-"file_plan9 process_plan9 plan9_switch plan9_wide_compare inflate_plan9 date_time_plan9 calendar png_native png_raster window_snapshot shell persistence platform_plan9 document_open file_transfer folder_transfer trash run run_ui applications applications_ui clock calendar_ui desktop_ui desktop_files_ui preferences settings_ui"}
guest_suites=
for suite in $suites; do
    case "$suite" in
        file_plan9|process_plan9|plan9_switch|plan9_wide_compare|inflate_plan9|date_time_plan9|calendar|png_native|png_raster|window_snapshot|shell|persistence|platform_plan9|document_open|file_transfer|folder_transfer|trash|run|run_ui|applications|applications_ui|clock|calendar_ui|desktop_ui|desktop_files_ui|preferences|settings_ui) ;;
        *) echo "unknown native suite: $suite" >&2; exit 1 ;;
    esac
    guest_suites="$guest_suites $suite-source $suite-saved"
    source_root=$rill/tests
    input=$source_root/${suite}_test.zi
    case "$suite" in
        file_plan9|process_plan9|plan9_switch|plan9_wide_compare|inflate_plan9|date_time_plan9|calendar)
            source_root=$root/../../ziranlang/ziran/tests/spec
            input=$source_root/${suite}_test.zi
            ;;
    esac
    if test "$suite" = window_snapshot; then
        source_root=$root/sys/src/cmd/rio9/tests
        input=$source_root/window_snapshot_test.zi
    fi
    if test "$suite" = png_native || test "$suite" = png_raster; then
        source_root=$kryon/tests
        input=$source_root/${suite}_test.zi
    fi
    "$ziran" ir --define NATIVE_PLAN9 --define PLAN9_BUILD --root "$source_root" \
        --module-path "$rill/src" --module-path "$rill/app" --module-path "$root/sys/src/cmd/rio9" \
        --module-path "$kryon/src/ui" --module-path "$kryon/src/backend" --module-path "$std" \
        -o "$stage/$suite-ir" "$input" >>"$log" 2>&1
    "$ziran" build --target=plan9-c --define NATIVE_PLAN9 --define PLAN9_BUILD \
        --root "$source_root" --module-path "$rill/src" --module-path "$rill/app" --module-path "$root/sys/src/cmd/rio9" \
        --module-path "$kryon/src/ui" --module-path "$kryon/src/backend" --module-path "$std" \
        -o "$stage/$suite-source" "$input" >>"$log" 2>&1
    "$ziran" build --target=plan9-c --define NATIVE_PLAN9 --define PLAN9_BUILD \
        --root "$stage/$suite-ir" -o "$stage/$suite-saved" \
        "$stage/$suite-ir/${suite}_test.zir" >>"$log" 2>&1
    mkdir "$stage/$suite-source/data" "$stage/$suite-saved/data"
    if test "$suite" = png_native || test "$suite" = png_raster; then
        python3 "$kryon/tests/png_fixtures.py" --directory "$stage/$suite-source/data"
        python3 "$kryon/tests/png_fixtures.py" --directory "$stage/$suite-saved/data"
    fi
    if test "$suite" = desktop_files_ui; then
        for form in source saved; do
            cp "$kryon/icons/ui.png" "$stage/$suite-$form/data/icon1.png"
            cp "$kryon/icons/language.png" "$stage/$suite-$form/data/icon2.png"
        done
    fi
done
# Keep the filesystem primitive checks first so failures can be distinguished
# from application parsing and interrupted-save behavior.

guest_cmd="
echo rill-ziran-plan9-start
cd $guest_stage
failed=0
if(~ $suites_only 0) {
cd /sys/src/cmd/rill
if(mk -f app/open.mk install 'BIN=$guest_stage/app-bin') {
    echo rill-open-plan9-build-ok
}
if not {
    echo rill-ziran-plan9-compile-failed
    failed=1
}
}
if not {
    cd $guest_stage/open-plan9
    for(source in *.c) {
        if(! 8c -FTVw \$source) {
            echo rill-ziran-plan9-compile-failed
            failed=1
        }
    }
    if(~ \$failed 0) {
        if(8l -o $guest_stage/app-bin/rill-open *.8) echo rill-open-plan9-build-ok
        if not {
            echo rill-ziran-plan9-link-failed
            failed=1
        }
    }
}
if(~ \$failed 0) {
    if($guest_stage/app-bin/rill-open) {
        echo rill-ziran-plan9-run-failed
        failed=1
    }
    if not echo rill-open-plan9-usage-status-ok
}
for(suite in $guest_suites) {
    if(~ \$failed 0) {
    cd $guest_stage/\$suite
    echo rill-ziran-plan9-suite \$suite
    for(source in *.c) {
        if(! 8c -FTVw \$source) {
            echo rill-ziran-plan9-compile-failed
            failed=1
        }
    }
    if(~ \$failed 0) {
        if(8l -o run *.8 -ldraw -lmemdraw -lthread -lflate) {
            echo rill-ziran-plan9-compile-ok
            if(RILL_TEST_ROOT=$guest_stage/\$suite/data ZIRAN_TEST_ROOT=$guest_stage/\$suite/data RILL_OPEN_BIN=$guest_stage/app-bin/rill-open KRYON_OFFSCREEN=1 KRYON_CAPTURE_PATH=$guest_stage/\$suite/capture.rgba font=/lib/font/bit/pelm/latin1.8.font ./run)
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
    }
}
if(~ \$failed 0 && ~ $suites_only 0) {
    cd /sys/src/cmd/rio9
    if(mk) echo rio-ziran-plan9-build-ok
    if not {
        echo rill-ziran-plan9-compile-failed
        failed=1
    }
}
if(~ \$failed 0 && ~ $suites_only 0) {
    cd /sys/src/cmd/rill
    if(mk test $rill_objects) echo rill-ziran-plan9-build-ok
    if not {
        echo rill-ziran-plan9-compile-failed
        failed=1
    }
}
for(application in $applications) {
if(~ \$failed 0) {
    cd /sys/src/cmd/rill
    if(mk -f app/\$application^.mk install 'BIN=$guest_stage/app-bin') {
        echo rill-^\$application^-plan9-build-ok
        if(home=$guest_stage/^\$application^-app-home KRYON_OFFSCREEN=1 KRYON_CAPTURE_PATH=$guest_stage/^\$application^-app.rgba font=/lib/font/bit/pelm/latin1.8.font $guest_stage/app-bin/rill-^\$application)
            echo rill-^\$application^-plan9-app-ok
        if not {
            echo rill-ziran-plan9-run-failed
            failed=1
        }
    }
    if not {
        echo rill-ziran-plan9-compile-failed
        failed=1
    }
}
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
        for application in $applications; do
            if test ! -f "$stage/$application-app.rgba" || test "$(wc -c <"$stage/$application-app.rgba")" -ne 2304000; then
                echo "rill-ziran-plan9: actual native $application application did not render its frame" >&2
                exit 1
            fi
            cp "$stage/$application-app.rgba" "$root/build/rill-$application-native.rgba"
        done
        for suite in png_raster run_ui applications_ui calendar_ui desktop_ui desktop_files_ui settings_ui; do
            case " $suites " in
                *" $suite "*)
                    expected=576000
                    if test "$suite" = png_raster; then expected=12288; fi
                    for input in source saved; do
                        capture=$stage/$suite-$input/capture.rgba
                        if test ! -f "$capture" || test "$(wc -c <"$capture")" -ne "$expected"; then
                            echo "rill-ziran-plan9: missing or incomplete native $suite capture" >&2
                            exit 1
                        fi
                    done
                    cmp "$stage/$suite-source/capture.rgba" "$stage/$suite-saved/capture.rgba"
                    cp "$stage/$suite-source/capture.rgba" "$root/build/rill-$suite-native.rgba"
                    ;;
            esac
        done
        trap cleanup EXIT HUP INT TERM
        echo "rill-ziran-plan9: ok ($(( $(date +%s) - start ))s, native 8c/8l)"
        exit 0
    fi
    if rg -q 'rill-ziran-plan9-(compile|link|run)-failed|rill-(shell|persistence|platform|document-open|file-transfer|folder-transfer|trash|run|run-ui|applications|applications-ui|clock|calendar-ui|desktop-ui|desktop-files-ui|preferences|settings-ui)-test-failed|kryon-png-(native|raster)-test-failed|(file|process|date-time)-plan9-test-failed|plan9-switch-test-failed|calendar-test-failed|rc: .*syntax error' "$log"; then
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
