# One Taiji desktop

For the latest implementation status, test results, startup blocker, and
prioritized remaining work, see [status and plan](unified-desktop-status.md).

Taiji starts Rill directly, inside `rio9 -d rill`. Rill is the borderless
desktop surface; rio9 supplies the native window service for external apps.
Runtime selection belongs to each application launcher. `startwm` starts
this same desktop from a text session. `debian-session` prepares the filesystem
view for Linux applications and remains available as a development tool.
The old flavor selector and its namespace/window-manager tables are removed.
The superseded desktop shell, its launcher tables, and its theme/display
controllers are also removed. The standard `rio` command delegates to `rio9`;
Control Panel's Display and Themes entries open Rill's Settings window.

The current implementation integrates native Plan 9 applications and an
experimental Debian i386 X11 runtime. It is a foundation for a unified OS,
not complete compatibility with Debian or Windows. Wine and Windows programs
are not supported yet.

## Using it

From the repository on the host, boot the installed desktop with `./q9 gui`.
The application menu's search finds **Text Editor** and **Eyes**. These start
Debian's `xedit` and `xeyes` in ordinary native windows without opening another
desktop. **About Rill** also opens as an ordinary window. The `x` in the title
bar asks the application to close through its normal close protocol, so it can
warn about unsaved changes. Applications without that protocol must be closed
through their own Quit command.

The equivalent command from a native terminal is:

```rc
window -m -dx 648 -dy 516 linux-app -t 'Text Editor' xedit -geometry 640x480+0+0
```

`window -m` preserves argument boundaries, including titles with spaces.
Register additional tested applications in `/lib/rill/applications` using
the same pattern. A bare program name passed to `linux-app` resolves under
`/usr/bin` in the Debian root; absolute Linux paths also work.

Documents open through the same registry with `rill-open file ...`: the
file-name suffix selects the application from `/lib/rill/associations`
(`$home/lib/rill/associations` overrides), and files under the native
`$home` are translated into the Debian runtime's `/home/user` view. The
Text Editor launcher opens empty; pass a document through `rill-open` or
the Files application.

The Debian root at `/debian/rootfs` must already contain the i386 runtime,
`Xvfb`, its libraries and fonts, and the requested application. The launcher
does not install packages. Existing Debian setup tools remain responsible
for preparing that root. Launch failures name the missing piece and stay
visible in the launching window.

## Integration

- `linux-app` creates a private temporary filesystem and Xvfb display for
  each launch. It starts the display and application through `linuxrun`,
  then stops both when the bridge closes.
- `xbridge` is a native libdraw client. It transfers Xvfb's image into the
  native window, sends keyboard and pointer events through XTEST, and
  synchronizes CLIPBOARD/PRIMARY text with `/dev/snarf`. It negotiates RandR
  (1.2 `RRSetScreenSize`, with the 1.0 `RRSetScreenConfig` fallback) and
  follows its native window's size when the X server permits resizing.
- `debian-session` binds the native user's `$home/Documents` at
  `/home/user/Documents` and supplies Linux `HOME=/home/user`. Document
  changes go to the same files from either runtime.
- `linuxrun` inherits selected session environment variables and accepts
  `-e NAME=value`. Linux `execve` preserves the environment passed by the
  guest. This keeps separate application displays independent.
- Rill lists native windows in its taskbar, including Debian applications
  hosted by `xbridge`. Clicking a task brings it forward and restores it if
  hidden. Closed windows disappear on the next refresh. Ctrl+Tab cycles
  application windows through the same activation path.
- Terminal, Files, and Settings open in native windows and use the same
  task switching as the other applications.
- The panel and open menus are copied from the desktop into foreground
  layers. Their input still goes to Rill, so application windows can stay
  visible behind menus.

Namespaces organize paths here; this is not an application security sandbox.
The runtime and its syscall implementations are experimental.

Application isolation policy (current state, Track 5 groundwork): every
application — native, Debian, or future Windows — runs in the user's
namespace with full access to the user's files. `linux-app` gives each
Debian launch a private temporary filesystem and display, and
`debian-session` binds only the user's Documents tree into the Linux view,
but nothing prevents a malicious program from reading or writing outside
those bindings. A real isolation boundary requires per-application
namespaces with explicit grants (documents, clipboard, specific devices)
and a user-facing permission dialog; designing it is open work and is a
prerequisite for running untrusted software, not for the supported
application set.

## Build and verify

`./q9 --rebuild-desktop gui` rebuilds the installed desktop components,
including `rio9`, `linuxrun`, and `xbridge`. To rebuild those three alone,
run `mk install` in their directories under `/sys/src/cmd` inside Taiji.

With other q9 instances stopped, run on the host:

```sh
make unified-smoke
make window-smoke
make linuxrun-smoke
```

The unified check builds the changed components, tests environment propagation
through a guest exec, reads a shared document, renders Debian's xeyes, and
checks path resolution, saving, XTEST input, the application close protocol,
and UTF-8 clipboard transfers in both directions against
a real Xvfb server. Its clipboard fixture uses a private file. A screenshot
is saved as `usr/glenda/tmp/unified-screen.ppm` in the host checkout.

The window check runs the Rill platform backend against the real native
window server. It first checks that a floating-point exception reaches the
application's note handler without crashing the kernel. It checks task
discovery, title and focus snapshots while an
app holds its resize reader, switching visible windows, restoring hidden
windows, removing closed tasks, and pixels above and below a desktop popup.

The separate desktop check is manual: boot normally, search for Text Editor,
open it, type and save a document, then close it and launch another app.
This exercises the Rill launcher and native window service as well as X11.

## Current limits

The bridge negotiates RandR and resizes the X screen when the native window
changes size and the X server permits it. The current rootfs's Xvfb refuses
RandR screen sizes past its start configuration, so displays there keep the
letterbox behavior; `linux-app` still starts the display at 1920×1080 so the
bridge can shrink to fit whenever the server allows it. Applications that
assume a fixed screen size at startup keep their initial layout, and refresh
performance is limited. Dialogs belonging to an X11 app stay within its
display. Window dragging and resizing still use rio9's controls; consistent
decorations and richer taskbar controls need further work.

Clipboard support covers text up to 1 MiB, including INCR reception from
owners that stream large selections; images, file transfers, and serving
INCR as an owner are not implemented. Keyboard support covers the core US
layout, common navigation keys and control characters. Ctrl+Tab is consumed
by the window server for task switching. Full modifier state, international
input methods, audio, GPU acceleration, and drag and drop need further
integration. Passing this check does not imply arbitrary Debian packages
work. The application matrix records what was actually verified.

Windows support will require a working Wine/runtime path and the same
window, clipboard, document, and process integration before Windows entries
can be added to the normal application menu.

## Native window interface

`/dev/wsys/<id>/winfo` is a non-blocking snapshot: four rectangle coordinates,
`current`/`notcurrent`, `visible`/`hidden`, and `desktop`/`window`. It does not
consume the application's `/dev/wctl` resize events. Writing `activate` to
the window's `wctl` restores, raises, and focuses it; a request made during
a mouse click is completed after release.

Only the desktop window can write `overlay` followed by up to sixteen
rectangles to its `wctl`. Coordinates are relative to its image. The window
server copies these regions into foreground layers and routes their input
to the desktop. Writing `overlay` alone removes them. Application windows
cannot publish desktop overlays.
