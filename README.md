# TaijiOS

[![Cloudflare Pages](https://github.com/taijiosnet/taiji/actions/workflows/pages.yml/badge.svg)](https://github.com/taijiosnet/taiji/actions/workflows/pages.yml)

![TaijiOS desktop banner](site/assets/taijios-banner.png)

TaijiOS is Kryon Labs' flagship operating system: a bootable desktop OS
tree based on the 9legacy branch of Plan 9 from Bell Labs, with Kryon
applications preinstalled as the default working environment.

The tree includes Shelf, Rill, t9, Inner Breeze, Pass, and the
supporting Kryon runtime work needed to boot them together inside
TaijiOS. See [README](README) for the historical upstream README file.

## Bundled Kryon Desktop

TaijiOS is the integration target for the Kryon application stack:

- Shelf provides the file manager and desktop file surfaces.
- Rill provides the graphical shell and application launcher.
- t9 provides the Terminal app.
- Inner Breeze provides breathing, meditation, and habit tracking.
- Pass provides stateless password generation.
- Kryon provides the shared UI/runtime layer used by the applications.

The default QEMU profile opens one Rill desktop immediately. Applications
choose their runtime through their launcher; there is no startup flavor or
namespace selection. The text profile stays available for low-level OS work.

An experimental Linux application path puts Debian X11 applications in native
windows alongside the desktop. The application menu includes Text Editor
(`xedit`) and Eyes (`xeyes`). Native applications and these launchers share
Documents and the desktop's text clipboard. This currently supports a small
set of i386 X11 applications; Windows/Wine execution is not implemented.
Native and supported Debian apps appear together in the taskbar, and Ctrl+Tab
switches between them. Terminal, Files, and Settings use native windows; the
panel and menus stay above apps. Debian application displays follow their
native window size, `rill-open` opens documents through the application
registry, and About opens as an ordinary window.
See [the unified desktop notes](doc/unified-desktop.md) for setup, commands,
tests, and current limits.
The [current status and remaining plan](doc/unified-desktop-status.md) distinguish
implemented features from verified behavior, including
the [application matrix](doc/application-matrix.md) and the work still needed
for Windows support.

## Boot Locally

To boot TaijiOS, install qemu, so that you have `qemu-system-x86_64` in your path.
Then:

	git clone https://github.com/taijiosnet/taiji.git
	./taiji/boot/qemu

The qemu script builds u9fs in taiji/sys/src/cmd/unix/u9fs and then runs
qemu with the right options to boot diskless, using the git clone as the
root file system.

Because the VM shares the files with your host machine, you can edit files in one place
and see the changes instantly in the other place. For example, you can edit files in your
local editor even if you are running tests in the TaijiOS VM.
You can run builds of Go binaries targeting TaijiOS on your host machine
and then test the binaries in the VM.
And you can run more than one VM, all sharing the same file system.

At boot time, the startup disk boot/pxeboot.raw loads a minimal TaijiOS kernel
into memory, which then PXE loads a plan9.ini and new kernel over TFTP (provided by qemu).
So if you make changes to the kernel, you can boot from `ether0!/sys/src/9/pc/9pc`
to test an as-yet-uninstalled kernel.

The plan9.ini is loaded from [/cfg/pxe/525400123456](cfg/pxe/525400123456).
(That number is the VM's MAC address.)
Changes made to that file will be visible on the next VM boot.

## Ziran migration

TaijiOS is being rewritten in current Ziran, with `plan9-c` as the native
transpilation path. Rill must be restored as the main desktop environment,
entirely in Ziran and using current Kryon. Compiler or runtime gaps belong in
Ziran; reusable UI capabilities belong in upstream Kryon; desktop policy
belongs in upstream Rill. App submodules contain committed upstream source.

This migration is not complete. The OS still contains thousands of C files
across commands, kernel and drivers, boot code, compatibility libraries, libc,
graphics, cryptography, networking, and other libraries. Assembly, the native
build/bootstrap path, and the remaining application implementations also
require an explicit migration and verification path. Passing a library or
shell probe does not establish that the OS or desktop has been rewritten.

The Rill shell, panel layouts, settings persistence, stub adapter, Plan 9
platform services, panel clock, Run dialog, Applications menu, Calendar,
Settings, and About now use Ziran implementations. A new `rill-desktop` entrypoint adds the
current Kryon panel, live task/launcher snapshots, in-place menus, all four
panel edges, autohide, scrolling, item moves and persistence, and available
volume/clipboard/session controls. Native desktop overlays use only the
client's own window namespace. The converted screens and desktop share a
native libdraw application host. Settings uses current Kryon controls for
desktop preferences, system control panels, all eight panel configurations,
Xfce panel import, display preview rollback, and all nineteen window shortcuts.
Saved desktop preferences and panel layouts reload without restarting the
new desktop. Desktop file selection, icon layout persistence, group dragging,
folder drops, context menus, rename/New Folder dialogs, and file transfer
controls now use Ziran and Kryon. Native directory listing, private folder
creation, renaming without overwriting, and file/folder clipboard transfers
are implemented. Native PNG icons now load through Kryon's `Image` surface.
Native Move to Trash, restore, and empty services now use Ziran, and the
desktop Trash browser uses current Kryon. The hosted transfer backend remains
C. Native display/input and other missing services still need implementations;
their controls remain unavailable. Rio's
window snapshot and PID ownership check also use Ziran. Run `make rio-ziran-plan9` to generate
rio's native sources, then `make rill-ziran-plan9-smoke` to generate the
behavior tests from source and saved IR through `plan9-c`, compile and link
with native `8c`/`8l`, and run them in the private TaijiOS guest. The gate also
checks native file and process operations, application and icon registry
overrides, and window controls restricted to processes launched by Rill. It
builds rio, compiles Rill's generated service objects through its native
`mkfile`, and runs the persistence checks through `mk test`.
It also checks Run history, application matching, categories and recent
launchers, Unicode editing, scrolling, session confirmation, clock formatting,
Gregorian calendars, month navigation, timestamps after 2038, panel editing,
window action failures, clipboard overflow, autohide, wheel ownership, panel
JSON validation, shortcut collision detection, concurrent preference merging,
exclusive native file leases, stable file selection after refresh, group
positions, folder drops, file operation failures, transfer conflicts, wallpaper
rotation, held pointer clicks, and display rollback. It renders
the converted Kryon screens with native libdraw from source and saved IR and
compares their captures. It builds, installs into private guest directories,
and renders the actual native Run, Applications, Calendar, Settings, About,
and desktop executables through their `app/*.mk` recipes.
PNG checks cover 33 independent reference images across standard color types,
depths, filters, transparency, and Adam7 interlace, plus malformed streams,
cache eviction, alpha composition, real desktop icons, and native Plan 9 image
compatibility. The decoder and native image provider are current Ziran;
bounded decompression uses Ziran's native `libflate` adapter. PNG files are
limited to 64 MiB and 4096 pixels per axis; gamma and color profiles are ignored,
and 16-bit samples use their high byte. Ziran's compiler regressions also cover wide
comparisons, array initialization without adjacent stack writes, aggregate
evaluation order, and file size queries that preserve the read cursor.
`TAIJI_RILL_ZIRAN_SUITES` and `TAIJI_RILL_ZIRAN_APPLICATIONS` can select
focused checks; the default runs every suite and all six applications.
`TAIJI_RILL_ZIRAN_SUITES_ONLY=1` runs native source/saved-IR suites against an
upstream `RILL_DIR` before integrating its commit. This mode omits downstream
service builds and application captures; the default gate verifies those too.
The native `rill-open` command and desktop share the Ziran document resolver.
It chooses each file's extension independently, preserves literal filenames,
retains system applications when the user overrides only part of the registry,
and limits Linux document translation to the normalized home directory.
The gate also builds this command and checks its actual argv and exit status.
Native file and folder Copy/Paste, Duplicate, Move, and Cut/Paste now use
Ziran services with staged publication, cancellation, Skip, Keep Both, Replace, and
preserved permissions and modification times. Folder copies include hidden
entries and empty directories and reject recursive bindings and destinations
inside the source. Moves publish the complete copy before removing source
entries; changed or protected entries stop removal with an explicit error.
Cut/Paste clears its unchanged clipboard selection only after every item
moves successfully. Foreign staging entries and newer clipboard contents are
preserved. Native Move to Trash saves a checksummed recovery record and the
complete payload before removing source entries. The current Kryon Trash
browser shows original paths and deletion times, supports restore with
collision-safe names, and confirms permanent emptying. Storage defaults to
`$home/lib/rill/trash` with an absolute `RILL_TRASH_DIR` override. Each private
entry records every copied node's identity; damaged metadata, replacement
nodes, redirected payloads, and unknown contents are preserved and reported.
Saved identities reanchor to the current store after a legitimate filesystem
remount. Native source/saved-IR checks cover recovery after releasing the
transfer, header and inventory corruption, bound payloads, cancellation before and
after publication, and the desktop controls. The browser displays up to 64
entries at a time; restore and empty currently run synchronously. Replace
stages the incoming entry completely and preserves the old destination in
Trash before publishing without overwriting a newly created entry. It handles
files, folders, differing types, batch choices, and Cut/Paste. Changed
destinations and overlap with selected sources refuse replacement; cancellation
or later publication failure leaves any completed backup recoverable in Trash.
Retry now retains completed and skipped items, restarts unpublished copies
with a fresh snapshot, and verifies a published destination before resuming
removal of unchanged source entries. Recreated source entries and changed
copies are preserved. Folder permissions can be repaired before retrying;
failed staging cleanup must succeed before another attempt starts. A
replacement reuses its published Trash archive after partial removal of the
old destination. Cut/Paste retains its original clipboard snapshot across
retry and clears it only after complete success. Undo now retains one completed
Copy, Duplicate, or Move batch in memory and verifies every result and
replacement backup before changing the batch. Copied results go to Trash;
moved results return to their original directory, recreating missing parents
and keeping both entries when a name is occupied. Replaced destinations return
from their verified Trash backups. Undo advances with the transfer polls and
can be cancelled and retried after partial progress. Changed trees, redirected
paths, and changed backups are preserved and reported. Closing the desktop
releases the journal; another completed Copy, Duplicate, or Move batch replaces
it. Undo does not provide Redo. Recovery of interrupted unpublished staging
entries remains pending.
The new desktop entrypoint is not yet the boot default: remaining file services,
full tray rendering and menus, plugin Properties, notifications,
and remaining services still need conversion and parity checks. The full
graphical desktop, Rill's other services, T9's remaining
legacy modules, and the wider OS migration remain unfinished.
