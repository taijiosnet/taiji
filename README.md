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

The Rill shell and stub adapter now use Ziran implementations. Run
`make rill-ziran-plan9-smoke` to generate their behavior tests through
`plan9-c`, compile and link with native `8c`/`8l`, and run them in the private
TaijiOS guest. The graphical desktop, Rill's other services, T9's remaining
legacy modules, and the wider OS migration remain unfinished.
