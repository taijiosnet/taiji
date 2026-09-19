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

## Round 10 (2026-09-19): virtual_init decoded further; two real bugs fixed

With the signal subsystem in place, the probe now shows Wine's own error
before any fault: `virtual_init: Assertion 'alloc_views.base != MAP_FAILED'
failed` — Wine builds its views area from **many small fixed mappings at
low addresses** (0xe40000 onward), not one giant reservation, and every
one failed against the `[0x40000000, +512MB)` mmap window. Changes:

- `mprotect` no longer zeroes the target range unless it was actually a
  recorded reservation — the first cut wiped loaded libraries (libc's
  version tables), which broke every dynamic program (`libbrotlicommon:
  undefined symbol: free`). This bug was caught by the unified smoke
  before it could ship.
- Giant anonymous `PROT_NONE` mappings (256MB+) are recorded as virtual
  reservations; `mprotect` materializes only recorded ranges.
- Low guest memory (`[0x200000, +Lowsize)`) attaches on demand so low
  fixed mappings can succeed; fork children reset the flag because
  segments do not survive `rfork`.

Remaining measured limit: the 1GB VM cannot host a second large segment
next to the 512MB main guest segment — `segattach ...: virtual memory
allocation failed` for every size from 1GB down to 127MB. The clean next
steps are to raise the VM's memory (`q9`'s `-m`) or to restructure the
guest memory into one segment spanning both regions; with low memory
backed, Wine's views build and the notepad path continues. Wine now exits
cleanly instead of crashing.

## The decoded fault and the missing subsystem (2026-09-19, third measurement round)

The round-7 claim that the crash note "never reaches userspace handlers" was
an artifact of reading a `tail`-truncated probe log; the corrected
full-log probe shows `traphandler` running and `FAULTCODE` firing. The
faulting instruction at `pc=0x4082531a` is **`hlt` (0xF4)** — a deliberate
user-mode privileged fault, surrounded by Wine state-machine code:

```
f4                       ; hlt  ← the fault
83 bd bc 12 00 00 05     ; cmp dword [ebp+0x12bc], 5
75 14                    ; jne
c7 85 bc 12 00 00 06 ... ; mov dword [ebp+0x12bc], 6
83 ec 0c / 6a 7f / e8 .. ; push 127; call ...
f4 / eb fd               ; hlt; jmp $
```

Wine's i386 runtime uses deliberate faults as dispatch points and
services them from **SIGSEGV handlers it registers with `rt_sigaction`**.
`linuxrun` stubs `rt_sigaction` (and the whole signal family) to success
without bookkeeping, so no signal is ever delivered and the fault kills
the process.

The prerequisite subsystem is therefore **Linux i386 signal delivery**:

1. `rt_sigaction` (174): record handler, flags, restorer, and mask from
   the guest's `struct kernel_sigaction` (i386: handler, flags, restorer,
   mask; sigsetsize at a4).
2. On privileged-fault notes (and `SIGSEGV`-class guest faults): build an
   i386 `rt_sigframe` on the guest stack — `pretcode`, `sig`, `pinfo`,
   `puc`, 128-byte `siginfo` (si_signo/si_code/si_addr for the fault),
   `ucontext` with the full register set and FP state, the guest's signal
   mask, and the `rt_sigreturn` trampoline
   (`popl %eax; movl $173,%eax; int $0x80`) — then set the guest's pc to
   the registered handler and sp to the frame. Exact layouts come from
   the Linux UAPI headers (`sigcontext_32`, `ucontext_i386`).
3. `rt_sigreturn` (173): restore the saved context from the frame.

With that in place, Wine's own dispatcher services the `hlt` thunk, and
notepad can proceed toward a mapped window. `FAULTCODE` and the corrected
full-log probe (`cat`, not `tail`) stay in the tree as the instruments.


## Round 12 (2026-09-19): clone children keep low memory; next fault isolated

Clone/fork children now re-attach the opt-in low segment in the post-rfork
context (where registernotestack and atnotify already succeed).  The
display probe's frontier fault is unchanged and now precisely placed: a
guest-stack read at 0x6001d874 from a pc inside linuxrun's own early child
path, occurring before the child registers its note handlers - which is why
FAULTCODE cannot observe it.  The next investigation is that pre-atnotify
window of the exec/clone child (argument copying, fork snapshot, or the
child resume itself touching guest memory whose segment attach ordering
has changed).


## Round 13 (2026-09-19): the frontier pc decoded

The faulting pc 0x13fdf is decoded from the binary: `mov eax,0x13; int
$0x40` - a native Plan 9 syscall stub, i.e. linuxrun itself passed the
guest pointer 0x6001d874 to a host read/write-style call while its
segment was not mapped (the neighbouring stubs at 0x12fdf-0x1300f are
the int 0x40 dispatch family).  The investigation therefore moves from
child note dispatch to the native-call sites that accept guest
pointers - file read/readat paths, socket reads, and the exec/fork
child's re-attach ordering - none of which validate the segment before
the call.  That validation (and a diagnostic naming the call site) is
the next increment.


## Round 17 (2026-09-19): the frontier syscall named

The crash-report stack dumps are guarded (guestok), so diagnostics no longer
mask the underlying fault.  The stub table decoded from the binary, checked
against sys/src/libc/9syscall/sys.h, is definitive: the faulting native
stub at pc 0x1435b loads **RFORK (19)** - the clone dispatch's own
rfork(RFPROC|RFFDG|RFNOTEG) call, executed from note-handler context,
faults reading the guest stack (0x6001d874, inside the 128KB Stackbase
segment).  The likely mechanism: the int80 note's saved return state
references the guest sp, and the child's fork-rebuild detaches the guest
stack segment before that state is consumed.  Fixing it needs live kernel
debugging: break on sysrfork with the wine probe running and inspect the
note/ureg state referencing guest addresses.


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
