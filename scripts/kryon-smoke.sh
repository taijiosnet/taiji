#!/bin/sh

# Kryon native Plan 9 smoke: boot the tree, rebuild libkryon.a from the
# pinned kryon sources, compile+link+run scripts/kryon-probe.c against
# it, and link Rill (which embeds ktrem and shelf, so the link step
# covers the app-facing library surface end to end). Catches kryon
# master regressions that only show up under the native 8c.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

outdir=${TAIJI_KRYON_SMOKE_DIR:-boot/q9}
log=${TAIJI_KRYON_SMOKE_LOG:-$outdir/kryon-smoke.log}
timeout=${TAIJI_KRYON_SMOKE_TIMEOUT:-1800}

mkdir -p "$outdir"
: >"$log"

# The native build cannot run k2c in the guest; generate the 8c-safe
# runtime modules and the embedded asset table on the host first (same
# step applications take, see kryon's mk/plan9-app.mk).
make -C sys/src/kryon kry-c-plan9 >>"$log" 2>&1

kryon_flags='-I/sys/src/kryon/src/platform/plan9/include -I/sys/src/kryon/include -I/sys/src/kryon/build/plan9/generated -DKRYON_BACKEND_LIBDRAW -DKRYON_PLATFORM_PLAN9 -DKRYON_NATIVE_PLAN9'

guest_cmd="
echo kryon-smoke-start
fail=0

echo 'kryon smoke: rebuilding libkryon'
cd /sys/src/kryon
if(mk install >/tmp/kryon-smoke-lib.log >[2=1])
	echo kryon-lib-ok
if not {
	echo kryon-lib-failed
	grep -v warning /tmp/kryon-smoke-lib.log | tail -30
	fail=1
}

echo 'kryon smoke: compiling probe'
if(cpp -+ $kryon_flags /scripts/kryon-probe.c > /tmp/kryon-probe.i && 8c -FTVw -o /tmp/kryon-probe.8 -c /tmp/kryon-probe.i && 8l -o /tmp/kryon-probe /tmp/kryon-probe.8 -lkryon)
	echo kryon-probe-link-ok
if not {
	echo kryon-probe-link-failed
	fail=1
}

if(/tmp/kryon-probe)
	echo kryon-probe-run-ok
if not {
	echo kryon-probe-run-failed-\$status
	fail=1
}

echo 'kryon smoke: linking rill'
cd /sys/src/cmd/rill
if(mk install >/tmp/kryon-smoke-rill.log >[2=1])
	echo rill-link-ok
if not {
	echo rill-link-failed
	grep -v warning /tmp/kryon-smoke-rill.log | tail -30
	fail=1
}

if(~ \$fail 0)
	echo kryon-smoke-ok
if not
	echo kryon-smoke-failed
fshalt
"

start=$(date +%s)

setsid env Q9_BOOT_TIMEOUT="$timeout" ./q9 --raw tty-run "$guest_cmd" >"$log" 2>&1 &
q9_pid=$!

stop_vm()
{
	kill -TERM -"$q9_pid" 2>/dev/null || kill -TERM "$q9_pid" 2>/dev/null || true
	wait "$q9_pid" 2>/dev/null || true
}

trap stop_vm EXIT HUP INT TERM

i=0
while [ "$i" -lt "$timeout" ]; do
	if grep -q 'kryon-smoke-ok' "$log"; then
		elapsed=$(( $(date +%s) - start ))
		echo "kryon-smoke: ok (${elapsed}s)"
		grep -E 'kryon-lib-ok|kryon-probe-(link|run)-ok|rill-link-ok' "$log" |
			sed 's/^/kryon-smoke: /'
		exit 0
	fi
	if grep -q 'kryon-smoke-failed\|kryon-smoke-fail:' "$log"; then
		echo "kryon-smoke: failed; tail follows" >&2
		tail -60 "$log" >&2
		exit 1
	fi
	if ! kill -0 "$q9_pid" 2>/dev/null; then
		echo "kryon-smoke: q9 exited before success marker; tail follows" >&2
		tail -60 "$log" >&2
		exit 1
	fi
	sleep 5
	i=$(( i + 5 ))
done

echo "kryon-smoke: timed out after ${timeout}s; tail follows" >&2
tail -60 "$log" >&2
exit 1
