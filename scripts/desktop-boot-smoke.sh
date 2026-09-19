#!/bin/sh
# Boot the real graphical desktop headless and verify it renders.
# Uses the QEMU monitor for a screenshot; nothing is typed, so this is a
# rendering/boot regression, not an interaction test.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

log=${TAIJI_DESKTOP_BOOT_LOG:-/tmp/taiji-desktop-boot.log}
shot=boot/q9/desktop-boot.ppm
timeout=${TAIJI_DESKTOP_BOOT_TIMEOUT:-150}

: >"$log"
rm -f "$shot"

setsid ./q9 --raw --headless gui >"$log" 2>&1 &
pid=$!

cleanup()
{
	kill -TERM -- -"$pid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

i=0
while [ "$i" -lt "$timeout" ]; do
	if grep -q 'q9: gui ready' "$log"; then
		break
	fi
	if ! kill -0 "$pid" 2>/dev/null; then
		echo 'taiji-desktop-boot-failed: q9 exited' >&2
		tail -60 "$log" >&2
		exit 1
	fi
	i=$((i + 1))
	sleep 1
done

if ! grep -q 'q9: gui ready' "$log"; then
	echo 'taiji-desktop-boot-failed: desktop never became ready' >&2
	tail -60 "$log" >&2
	exit 1
fi

# Poll the screen until the desktop actually renders past the boot splash.
i=0
while [ "$i" -lt 60 ]; do
	if ! echo 'screendump boot/q9/desktop-boot.ppm' | timeout 5 socat - "UNIX-CONNECT:boot/q9/monitor.sock" >/dev/null 2>&1; then
		echo 'taiji-desktop-boot-failed: monitor screendump failed' >&2
		exit 1
	fi
	if [ ! -s "$shot" ]; then
		echo 'taiji-desktop-boot-failed: no screenshot produced' >&2
		exit 1
	fi
	if python3 - "$shot" <<'EOF'
import sys

path = sys.argv[1]
with open(path, 'rb') as f:
    data = f.read()
if data[:2] != b'P6':
    sys.exit(1)
header_end = data.index(b'255\n', 12) + 4
parts = data[:header_end].split()
w, h = int(parts[1]), int(parts[2])
pixels = data[header_end:]
if len(pixels) < w * h * 3:
    sys.exit(1)
step = max(1, (w * h) // 20000)
samples = pixels[::3 * step]
count = len(samples)
mean = sum(samples) / count
var = sum((p - mean) ** 2 for p in samples) / count
if mean < 8 or mean > 247 or var < 64:
    sys.exit(1)
print('desktop screenshot %dx%d mean %.1f variance %.1f' % (w, h, mean, var))
EOF
	then
		break
	fi
	i=$((i + 1))
	sleep 2
done

if [ "$i" -ge 60 ]; then
	echo 'taiji-desktop-boot-failed: desktop never rendered content' >&2
	exit 1
fi

echo taiji-desktop-boot-ok
