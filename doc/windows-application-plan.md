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

## Measured blockers (as of 2026-09-19)

1. The Debian runtime at `/debian/rootfs` contains no Wine packages;
   installing them requires package downloads, which in turn requires the
   syscall layer to carry a full TLS/HTTP client workload (apt). Socket
   translation exists (the D-Bus bridge work), but apt has not been
   exercised; that is the next measurement to make.
2. Wine's threading (NPTL futexes, TLS, `vfork` semantics) is heavier than
   any binary currently running under `linuxrun`; the futex work landed
   for D-Bus but Wine stresses it far more broadly.
3. Audio is unimplemented everywhere, so the first target must tolerate
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
