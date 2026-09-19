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
| `ld.so` dependency search (`openat`, `statx` probes) | works after this round's fixes |
| `libc.so.6` and `libwine` load | works |
| Wine's own `ntdll.so` load | **fails**: `wine: could not load ntdll.so: (null)` |

Syscall work landed from the first measurement: `statx` (i386 nr 383) is
now implemented — absolute paths, `AT_FDCWD`, dirfd-relative through
`/proc/pid/fd`, and `AT_EMPTY_PATH` (flags are the third argument, which
the first cut got wrong; the correction is what let ld.so finish).

The remaining blocker is the deepest one: Wine's loader dlopens
`i386-unix/ntdll.so`, which needs glibc's dynamic-loader TLS machinery
(TLS descriptors, `dl_iterate_phdr`, early TLS setup) that the translated
runtime does not yet provide. That is the next implementation target on
the Wine path; it is loader work in `linuxrun`, not a packaging problem.

Audio remains unimplemented everywhere, so the first target must tolerate
its absence (notepad does).

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
