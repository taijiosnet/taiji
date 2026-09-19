# Windows application path: groundwork

Status: groundwork only. No Wine runtime, Windows launchers, or Windows
application has run on Taiji. This page records the representative
application choice, its requirements, and the measured blockers, per the
remaining-work plan in [the status page](unified-desktop-status.md).

## Representative application: Notepad (Wine builtin)

`wine notepad` is chosen as the first Windows application target because
it exercises the complete stack — Win32 GDI calls, a message loop, menus,
dialog resources, keyboard and pointer input, clipboard, and file open/save
through Wine's unixification — with a tiny installer footprint and no
per-application quirks to debug first. Once notepad completes the standard
desktop workflow, a real installed application (for example Notepad++ 32-bit)
follows with the same harness.

Requirements:

| Area | Requirement | State on Taiji today |
| --- | --- | --- |
| CPU | i386 Windows PE execution through Wine's loader | i386 Linux ELF execution works (`linuxrun`); PE loading is Wine's job |
| Runtime | `wine` i386 packages plus `wine32` architecture data | **absent from `/debian/rootfs`** — the runtime has X11 clients only |
| Graphics | an X server (GDI → X11 driver) | present (`Xvfb` + `xbridge`) |
| Input | XTEST keyboard/pointer | present |
| Audio | Wine ALSA/OSS output | absent (no audio stack yet, any path) |
| Fonts | corefonts or Wine's builtin fonts | some DejaVu fonts present; corefonts absent |
| Filesystem | `Z:` mapping or `$HOME` bind to share Documents | `debian-session` already provides `/home/user` ↔ `$home/Documents` |

## Measured blockers (updated 2026-09-19, second measurement round)

The Wine package set is now **installed**: `scripts/fetch-wine.sh` stages
Debian bookworm's i386 `wine`, `wine32`, and `libwine` closure (115
packages) into `/debian/rootfs` the same way `fetch-xfce.sh` does. The
probe `cfg/q9/wine-measure.rc` runs Wine through `linuxrun` and captures
the result. Measured progress so far:

| Stage | Result |
| --- | --- |
| Wine ELF loads through `linuxrun` | works |
| `ld.so` dependency search (`openat`, `statx` probes) | works |
| `libc.so.6` and `libwine` load | works |
| Wine locates itself (`realpath` of `/proc/self/exe`) and loads `ntdll.so` | works |
| `wine --version` | **works**: prints `wine-8.0 (Debian 8.0~repack-4)` |
| `wine notepad` startup | **process stays alive**; the probe with a live Xvfb display (`cfg/q9/wine-display-probe.rc`) shows Wine proceeding into PE initialization — a child thread then faults with a general-protection violation at `pc=0x4082531a` inside Wine's address space, before notepad maps a window (the captured X screen stays black) |

Syscall and runtime work landed from these measurements:

- `statx` (i386 nr 383): absolute paths, `AT_FDCWD`, dirfd-relative through
  `/proc/pid/fd`, and `AT_EMPTY_PATH` (flags are the third argument).
- `readlink`/`readlinkat` now answer `/proc/self/exe` and `/proc/<pid>/exe`
  with the guest-visible executable path, but only when it is absolute —
  relative native invocations keep the old answer so multi-call binaries
  like busybox still fall back to `argv[0]`.
- `/proc/self` is rewritten to the numeric `/proc/<pid>` directory for the
  stat and access families, since the native `/proc` has no `self` entry
  and `realpath` probes every component.
- `faccessat` (307) and `faccessat2` (439) are implemented.

The next targets on the Wine path, in order: decode the faulting
instruction behind the general-protection violation (add a code-bytes
dump at the fault pc to linuxrun's crash report; the fault is likely a
segment-register or privileged operation Wine's thread setup uses that
needs emulation), then give notepad a path to map a window, verify
wineserver's socket protocol under the translated syscalls, and wire a
Windows launcher through `linux-app` following the Text Editor pattern.

## Architecture decision to make next

Run Wine directly under `linuxrun` (one translation layer, best
integration, highest syscall surface risk) versus a contained Linux
runtime hosting Wine with its windows bridged like X11 apps (isolated,
heavier, doubles IPC). The plan requires measuring actual `linuxrun`
blockers before committing; the measurement entry point is installing the
i386 `wine` package set into a throwaway rootfs and running
`linuxrun wine notepad` with the existing diagnostics.

## Integration checklist once it runs

The same bar as Debian applications: launch from the application menu,
keyboard/pointer input, shared-document save/reopen, bidirectional
clipboard, normal close, and clean process teardown — then a Windows
application compatibility matrix mirroring
[the Debian matrix](application-matrix.md).
