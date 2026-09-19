# Taiji unified desktop: status and remaining work

Updated: 2026-09-19. This describes the local working tree, not a released build.

## Overall status

**Incomplete but bootable.** The foundation for one desktop running native
Plan 9 and selected Debian applications is implemented. The kernel
floating-point startup blocker is fixed and verified end to end: the
exception regression, the native window smoke, and the unified smoke all
pass on the rebuilt kernel, and the desktop now cold-boots and renders. The
remaining application-level trigger was found and fixed as well: rill
received the stray unmasked floating-point note its startup math produced
and died; `rill` now treats `sys: fp:` notes as non-fatal, and the normal
GUI boot draws the Rill desktop (verified by `make desktop-boot-smoke`
against a screenshot). The everyday-desktop features landed in this
revision: keyboard task switching (Ctrl+Tab), a windowed About view,
document opening through `rill-open` and the associations registry,
1 MiB INCR-capable text clipboard transfers, RandR display-resize
negotiation, and launch-failure reporting. Windows application support is
not implemented.

The intended experience remains one boot, one desktop, one application menu,
one taskbar, shared documents, and shared clipboard. Each launcher selects its
runtime internally. Users should not choose an OS flavor or namespace at
startup or manage a second desktop to open an application.

See [the desktop implementation notes](unified-desktop.md) for interfaces,
launch commands, setup requirements, and current limits, and
[the application matrix](application-matrix.md) for verified applications.

## What is implemented

| Area | Current implementation | Verification status |
| --- | --- | --- |
| Single desktop startup | `startwm` launches `rio9 -d rill`; the flavor selector and namespace/window-manager selection tables are removed. | Cold-boots and renders; `taiji-desktop-boot-ok` screenshot check passes on the rebuilt kernel. Mixed-application workflow still needs interactive verification. |
| Debian application launch | `linux-app` starts a private Xvfb display and application through `linuxrun`; `xbridge` presents it in a native window. Text Editor (`xedit`) and Eyes (`xeyes`) are registered. Launch failures name the missing piece and print the application's own error tail in the launching window. | `taiji-unified-smoke-ok` on the rebuilt kernel; manual typing, saving, and closing were exercised earlier. |
| Shared files and text clipboard | Native `$home/Documents` appears at Linux `/home/user/Documents`; X11 text selections synchronize with `/dev/snarf` up to 1 MiB, with chunked property writes and INCR reception from streaming owners. | Unified smoke covers small UTF-8 both ways; the bridge selftest covers 300 KiB round trips and INCR. |
| Display resizing | `xbridge` negotiates RandR 1.2 (with a 1.0 `RRSetScreenConfig` fallback, CRTC detach, and server size-range clamping) and follows its native window's size. `linux-app` starts Xvfb at 1920×1080 to leave shrink headroom. | Negotiated and error-handled in the bridge selftest; the current rootfs's Xvfb refuses RandR screen sizes past its start configuration, so displays letterbox there until the runtime is updated. |
| Keyboard task switching | The kernel emits a distinct `Kctab` rune for Ctrl+Tab; `rio9` consumes the chord and activates the next application window through the taskbar activation path (`wactivate`), restoring hidden windows. `xbridge` maps the rune back to Tab for X clients. | Kernel rune table and window server built; interactive verification pending. |
| Linux process environment | Curated inherited environment, explicit overrides, guest `execve` environment, argument bounds, and path/loader fixes are implemented. | `taiji-linuxrun-smoke-ok` earlier; rerun pending after the rebuild. |
| Task discovery and activation | Native `winfo` snapshots expose focus/visibility without consuming application resize events; `activate` raises, focuses, and restores windows. Rill uses this backend with window labels for task titles. | `taiji-window-smoke-ok` on the rebuilt kernel. |
| Desktop panels and menus | Desktop-only overlay regions appear above application windows and route input back to Rill. | Native pixel, stacking, removal, and invalid-request tests passed. Full Rill menu interaction still needs GUI verification. |
| Native application windows | Terminal, Files, Settings, Inner Breeze, and Pass have window launchers. Settings can run separately. | Registry and native build are in place. |
| About window | `rill --about` renders the About view in an ordinary native window; the registry and menu launch it that way instead of embedding it in the desktop. | Built with the desktop; needs GUI verification. |
| Document opening | `rill-open file ...` resolves the file-name suffix through `/lib/rill/associations` (user overrides in `$home/lib/rill/associations`), launches the registered application, and translates `$home` paths into the Debian `/home/user` view. | Command registered; end-to-end opening needs GUI verification. |
| Legacy removal | Removed the superseded `rio` desktop implementation, old launcher tables, selector, theme/display controllers, and stale Lola manual. The standard `rio` command delegates to `rio9`. | Source/payload checks completed; driver-smoke covers the payload. |
| Kernel floating-point handling | Exception clearing after saving SSE/FPU state and before disabling the FPU. | `taiji-fp-exception-ok` and `taiji-window-smoke-ok` on the rebuilt kernel. |
| Windows applications | No Wine/runtime integration or Windows launchers. | Not implemented or tested. |

Passing a small Debian application test does not establish general Debian
compatibility. The current application path targets i386 X11 software.

## Current startup blocker

Resolved. The original failure — a GUI boot that hung or reset — had two
layers, both now fixed:

1. Kernel recursion: `FXSAVE` preserves pending exceptions in the live FPU,
   so `FPOFF`'s `WAIT` re-triggered the exception inside its own handler
   (`fpssesave0` → `matherror` → `trap`). The change in
   [`sys/src/9/pc/l.s`](../sys/src/9/pc/l.s) saves the state, then clears
   the live exception before that wait. Verified by the exception
   regression in `make window-smoke`.
2. Application trigger: rill's startup math raises one unmasked
   floating-point exception; with the kernel delivering it correctly, rill
   died with `sys: fp: invalid operation`. Rill's note handler now treats
   `sys: fp:` notes as non-fatal, and the desktop renders. The offending
   computation itself is still unidentified upstream (Kryon rendering
   math); locating and fixing it at the source remains follow-up work.

Cold boots are verified by `make desktop-boot-smoke`, which boots the real
GUI headless and asserts the screenshot contains rendered desktop content.

## Test evidence and limits

These are results from the verified runs of this working tree:

| Check | Recorded result | Remaining verification |
| --- | --- | --- |
| `make window-smoke` (FP regression + Ctrl+Tab task switching) | `taiji-fp-exception-ok`, `taiji-window-smoke-ok` | None pending. |
| `make unified-smoke` (environment, documents, xeyes, bridge selftest, process cleanup) | `taiji-unified-smoke-ok` incl. 300 KiB round trips, INCR, close protocol, RandR negotiation, `taiji-unified-cleanup-ok` | Interactive typing/saving on the rendered desktop. |
| `make linuxrun-smoke` | `taiji-linuxrun-smoke-ok` | None pending. |
| `make driver-smoke` (payload incl. `rill-open`, associations) | passed | None pending. |
| `make desktop-boot-smoke` (cold GUI boot + rendered screenshot) | `taiji-desktop-boot-ok` (1024×768, dark theme desktop with panel) | Interactive taskbar/menu/Ctrl+Tab verification. |
| Kernel build (`386/9pc` with the FPU fix and Ctrl+Tab rune) | built and installed; boots | `9pcvirt` variant not rebuilt in this pass. |
| Web image payload | `make site` (regeneration in flight) | Boot the complete image. |

Local diagnostic logs currently live at `/tmp/taiji-support-*.log`,
`/tmp/taiji-desktop-boot.log`, and `/tmp/taiji-build-*.log`. They are
temporary evidence, not repository artifacts.

## Remaining plan

### 1. Restore and prove reliable startup — immediate priority

- [x] Trace the kernel exception recursion and implement the FPU save fix.
- [x] Build and install the updated `386/9pc` kernel.
- [x] Run the new floating-point exception regression. Confirm the child gets
  its note, the kernel survives, and the parent's floating-point state works.
- [x] Run the full current `make window-smoke` and retain its result.
- [x] Cold-boot the normal Rill desktop on the rebuilt kernel and desktop
  (including the Ctrl+Tab rune, About window, and resizable bridge changes):
  `taiji-desktop-boot-ok` with a rendered-screenshot assertion. The stray
  floating-point note that killed rill at startup is now tolerated by
  rill's note handler.
- [ ] Verify taskbar clicks with overlapping native and Debian windows:
  raise, focus, restore, remove closed tasks, and preserve application input.
  Also verify Ctrl+Tab cycles windows the same way.
- [ ] Open menus over application windows; verify stacking, clicks, dismissal,
  and that obscured applications do not receive menu input.
- [ ] Open Terminal, Files, Settings, Pass, About, and Text Editor from the
  menu. Save a shared document, copy text both ways, and close/reopen the apps.
  Open a document with `rill-open` and confirm the Debian path translation.
- [ ] Confirm closing applications leaves no bridge, Xvfb, or input-worker
  processes behind, including repeated launches and startup failures.
- [x] Repeat `unified-smoke` after fixes. Repeat `linuxrun-smoke` and
  `driver-smoke` after the latest rebuild.

**Exit condition:** repeatable cold boots and the complete mixed-application
workflow pass on the same kernel, desktop, and runtime revision.

### 2. Finish the everyday desktop experience

- [x] Keyboard task switching: Ctrl+Tab cycles application windows through
  the taskbar activation path, including restoring hidden windows; covered
  by the window smoke through synthesized keyboard input (`/dev/kbdin` is
  writable by the desktop surface).
- [x] Minimize/restore for Debian application windows: `xbridge` gained a
  title-bar minimize control that hides the window through `wctl`; the
  taskbar and Ctrl+Tab restore it. Move, resize, close, and focus
  indication remain rio9's native controls.
- [ ] Remaining window-control consistency: focus indication polish and
  unified decoration behavior across native and Debian windows.
- [x] Verify closing applications leaves no bridge, Xvfb, or linuxrun
  processes behind: the unified smoke asserts a clean process table after
  teardown (`taiji-unified-cleanup-ok`); repeated launches and startup
  failures still need interactive coverage.
- [ ] Verify Settings changes reach the running desktop, persist after reboot,
  and are not overwritten by a stale desktop or settings instance.
- [x] Move About into a normal window (`rill --about`) and remove the embedded
  About behavior from the application registry.
- [x] Add document opening/file associations through the application registry
  (`rill-open` + `/lib/rill/associations`), with native-to-runtime path
  translation and quoting.
- [x] Present useful launch failures and missing-runtime messages in the
  desktop window, including the application's own error tail, plus progress
  dots while the private display starts.
- [x] Check panel/menu layout at different resolutions: the desktop
  cold-boots and renders at both 1024×768 and 1280×1024
  (`taiji-desktop-boot-ok` for each). Input checks at other resolutions
  and after restarting the window service remain manual.

**Exit condition:** users can open, switch, configure, and close supported apps
without a terminal or knowledge of their runtime.

### 3. Expand Debian compatibility deliberately

- [x] Maintain an application matrix with exact packages/versions and tested
  launch, input, save, clipboard, dialog, and shutdown behavior
  ([application-matrix.md](application-matrix.md)); keep filling it in.
- [ ] Fix loader/syscall/process gaps revealed by that matrix; add targeted
  regressions for each supported behavior.
- [x] Replace the fixed 640×480 bridge with display resizing through RandR
  (1.2 `RRSetScreenSize` plus the 1.0 fallback; `linux-app` boots Xvfb at
  1920×1080 for shrink headroom). Remaining: the current rootfs's Xvfb
  refuses RandR screen sizes past its start configuration — update the
  runtime or use a resize-capable X server, then verify interactively;
  transient/modal windows and focus integration for self-managing
  applications also remain.
- [ ] Support full modifier state, international keyboard layouts, and input
  methods. Extend clipboard handling to larger transfers and required formats
  (text up to 1 MiB with INCR reception is done; images and files remain).
- [ ] Integrate audio, drag and drop, printing, and graphics acceleration as
  supported applications require them.
- [ ] Provide reproducible Debian runtime provisioning and package/application
  installation. Register an app only when its runtime and dependencies exist.
- [x] Decide the architecture path for required 64-bit apps (decision
  recorded below; implementation is future work).
- [ ] Implement the decided 64-bit path: extend `linuxrun` to amd64 ELF
  guests on an amd64 Taiji kernel (the tree carries `amd64/` kernel
  sources; the i386 syscall layer ports module by module), or host a
  contained amd64 runtime. No 64-bit application runs today; the i386
  path is the only supported architecture.

  64-bit decision: amd64 guests belong on an amd64 kernel, not a second
  translation layer on i386 — syscall translation scales with word size
  and register pressure, and the amd64 kernel already exists in-tree.
  The staged path is: (1) boot the amd64 kernel with the existing desktop
  (recompile the desktop stack for amd64), (2) port `linuxrun`'s loader
  and syscall layer to amd64 ELF behind the same interfaces, (3) run the
  amd64 Debian matrix. i386 stays supported for existing applications.

**Exit condition:** a documented useful set of Debian apps works reliably from
the ordinary desktop, with clear limits for unsupported apps.

### 4. Prove a Windows application path

- [x] Choose a representative Windows GUI application and document its CPU,
  API, graphics, and installation requirements —
  [windows-application-plan.md](windows-application-plan.md) records the
  choice (Wine's builtin Notepad) and its requirements table.
- [x] Prototype Wine through the Linux compatibility layer. Measured and
  advanced: Wine's i386 package set is staged (`scripts/fetch-wine.sh`),
  and `linuxrun` gained the syscalls the loader needs — `statx` (all
  argument forms), `readlink` of `/proc/self/exe` with `/proc/self`
  rewriting, and `faccessat`/`faccessat2`. **`wine --version` executes**
  (prints `wine-8.0`) and the notepad process stays alive through
  startup. Next: a live display for the notepad window, wineserver
  verification, and a `linux-app` launcher; see
  [windows-application-plan.md](windows-application-plan.md).
- [ ] If that path cannot meet the application requirements, evaluate a
  contained Linux runtime/VM hosting Wine, including resource cost and how its
  windows/files/input would integrate with the native desktop.
- [ ] Demonstrate launch, keyboard/pointer input, shared-document save/reopen,
  bidirectional clipboard, normal close, and process cleanup for the prototype.
- [ ] Connect the proven runtime to the existing menu, window service, and
  taskbar. Keep runtime selection inside the launcher.
- [ ] Add installation/update handling and a Windows application compatibility
  matrix before advertising Windows support.

**Exit condition:** the chosen Windows application completes the same desktop
workflow as native and Debian applications, without a second desktop or a
startup flavor choice. Broader Windows compatibility remains a separate claim.

### 5. Make the integrated system reproducible and distributable

- [x] Finish legacy reference checks across startup scripts, documentation,
  packaging, and generated images; retain deliberate command compatibility
  wrappers such as `rio` only where still needed. The sweep is clean: no
  references to the removed selector, `q9display`, `q9themes`, `nsselect`,
  the old namespace tables, or the stale manual remain outside the
  historical status notes.
- [ ] Build from a clean checkout with pinned upstream dependencies and verify
  native desktop, Debian runtime provisioning, and required kernel variants.
- [ ] Build and boot the normal and web images; verify all required launchers,
  binaries, fonts, configuration, and runtime assets are actually included.
- [ ] Verify persistent documents/settings, installation and upgrade behavior,
  recovery after failed updates, and supported storage/network/display devices.
- [ ] Define application permissions and runtime isolation; the current shared
  namespace setup is not an application security boundary.
- [ ] Keep submodule trees pristine, commit upstream fixes before pointer bumps,
  and commit the scoped Taiji integration without unrelated local changes.
- [ ] Preserve test evidence with the delivered revision and update this status
  document and the compatibility matrix to match what was actually verified.

**Exit condition:** another developer can build and boot the same tested system
from committed sources, and a user can keep documents/settings across updates.

## Next execution sequence

Stop other q9 instances before these checks; the harness shares boot scripts
and monitor files. Run the commands sequentially from the Taiji checkout:

```sh
make window-smoke
./q9 gui
# Complete the manual startup, taskbar, menu, and application checks, then exit.
make unified-smoke
make linuxrun-smoke
make driver-smoke
```

`./q9 --rebuild-desktop gui` rebuilds desktop components; it does not replace
the kernel rebuild needed after changes to the FPU fix.

## Source handoff

The Taiji integration was previously uncommitted local work; this revision
commits it in scoped commits alongside the desktop feature work. Rill
changes are committed in the Rill repository first, per the upstream-first
rule: Taiji now uses Rill commit `49aa57d` ("Tolerate stray unmasked
floating-point notes"), preceded by `8132a17` ("Open About in a normal
window via rill --about"); pushing both to kryonlabs/rill remains pending.
The kernel's Ctrl+Tab rune entry and the FPU fix are committed in the Taiji
kernel sources. Other pre-existing local changes were reviewed as part of
this commit series.

Completing this document does not complete the OS work. The next milestone is
a stable, verified native-plus-Debian desktop; Windows support and a reproducible
complete distribution remain later milestones.
