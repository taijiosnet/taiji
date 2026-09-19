#!/bin/sh

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

outdir=${TAIJI_DRIVER_SMOKE_DIR:-boot/q9}
log=${TAIJI_DRIVER_SMOKE_LOG:-$outdir/driver-smoke.log}
timeout=${TAIJI_DRIVER_SMOKE_TIMEOUT:-120}
netdevice=${Q9_NET_DEVICE:-virtio-net-pci}

mkdir -p "$outdir"
: >"$log"

guest_cmd='
echo driver-smoke-start
fail=0

fn must {
	if(! $*) {
		echo driver-smoke-fail: $*
		fail=1
	}
}

if(test -x /bin/q9hw)
	q9hw
if not {
	echo driver-smoke-devices
	if(test -f /dev/drivers)
		cat /dev/drivers
}

must test -d /net/ether0
must test -d /net/tcp
must test -d /sys/src/ktrem
must test ! -d /sys/src/kapsule
must test -x /386/bin/ktrem
must test -x /386/bin/rill
must test -x /386/bin/shelf
must test -x /386/bin/inbe
must test -x /386/bin/pass
must test -x /386/bin/rio9
must test -x /386/bin/linuxrun
must test -x /386/bin/xbridge
must test -x /rc/bin/linux-app
must test -x /rc/bin/rill-open
must test -r /lib/rill/associations
must test -x /386/bin/explorer
must test -x /386/bin/q9taskmgr
must test -x /386/bin/rio

if(~ $fail 0)
	echo driver-smoke-ok
if not
	echo driver-smoke-failed
fshalt
'

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
	if grep -q 'driver-smoke-ok' "$log"; then
		elapsed=$(( $(date +%s) - start ))
		echo "driver-smoke: ok (${elapsed}s)"
		echo "driver-smoke: net-device ${netdevice}"
		awk '
			/#l0:| memory:|driver-smoke-devices|^#[A-Za-z0-9]+|^q9hw	/ {
				if(!seen[$0]++)
					print
			}
		' "$log" | sed 's/^/driver-smoke: /'
		exit 0
	fi
	if grep -q 'driver-smoke-failed' "$log"; then
		echo "driver-smoke: guest checks failed; tail follows" >&2
		tail -80 "$log" >&2
		exit 1
	fi
	if grep -q 'no ethernet interfaces found' "$log"; then
		echo "driver-smoke: no ethernet interface for ${netdevice}; tail follows" >&2
		tail -40 "$log" >&2
		exit 1
	fi
	if ! kill -0 "$q9_pid" 2>/dev/null; then
		echo "driver-smoke: q9 exited before success marker; tail follows" >&2
		tail -80 "$log" >&2
		exit 1
	fi
	sleep 1
	i=$((i + 1))
done

echo "driver-smoke: timed out after ${timeout}s; tail follows" >&2
tail -80 "$log" >&2
exit 1
