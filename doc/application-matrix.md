# Taiji application matrix

Tested applications on the unified desktop, with the behavior verified for
each runtime tier.  Passing a row means the listed behaviors worked on the
recorded revision; unlisted behavior is unverified.  Register a new
application only after its row is filled in.

Native applications run as Plan 9 programs in `rio9` windows.  Debian
applications run as i386 X11 clients on a private `Xvfb` display presented
by `xbridge`; see [the desktop notes](unified-desktop.md) for the
architecture and [the status page](unified-desktop-status.md) for the
current verification state.

## Native applications

| Application | Launcher id | Launch | Input | Save | Clipboard | Close | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Terminal (`t9`) | `terminal` | menu, taskbar | keyboard, pointer | shell history | `/dev/snarf` both ways | window `x` or `exit` | default terminal for `window -m` |
| Files (`shelf`) | `files` | menu, taskbar | keyboard, pointer | file tree edits | text selections | window `x` | shared `$home` tree |
| Settings (`rill --settings`) | `settings` | menu, Control Panel entries | pointer | persists in `$home/lib/rill/settings`; applied at next desktop start | n/a | window `x` | can run beside the desktop |
| About (`rill --about`) | `about` | menu | pointer | n/a | n/a | window `x` | normal window; no longer embedded in the desktop |
| Inner Breeze (`inbe`) | `inbe` | menu, taskbar | pointer | habit data | n/a | window `x` | |
| Pass (`pass`) | `pass` | menu, taskbar | keyboard | generated passwords copy to snarf | `/dev/snarf` | window `x` | |
| Document opening (`rill-open`) | n/a | command line | n/a | n/a | n/a | n/a | opens files by suffix through `/lib/rill/associations` |

## Debian applications (i386 X11)

Environment: Debian runtime at `/debian/rootfs`, `Xvfb` 24-bit display,
`xbridge` window with XTEST input and text clipboard synchronization.

| Application | Launcher id | Launch | Input | Save | Clipboard | Close | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Text Editor (`xterm` + `nano`) | `text-editor` | menu, taskbar, `rill-open` for `.txt` and other text suffixes | keyboard, pointer; US core layout, navigation keys, control characters | writes the shared `$home/Documents`; either runtime sees the changes | UTF-8 text both directions up to 1 MiB, INCR receive supported | window `x` uses `WM_DELETE_WINDOW` when the app offers it; otherwise Quit | unsaved-change dialogs stay inside the app display; the bridge negotiates RandR and follows the window size when the X server permits |
| Eyes (`xeyes`) | `eyes` | menu, taskbar | pointer tracking | n/a | n/a | window `x` or Quit | simple X client; smallest runtime requirement |

## Verification record

| Date | Revision | Result |
| --- | --- | --- |
| 2026-09-19 | FPU-fixed kernel + rebuilt desktop | `taiji-fp-exception-ok`, `taiji-window-smoke-ok`, `taiji-unified-smoke-ok` |
| 2026-09-19 | Desktop feature build (Ctrl+Tab, About window, rill-open, 1 MiB/INCR clipboard, RandR negotiation, launch-failure reporting) | `taiji-unified-smoke-ok` incl. 300 KiB round trips, INCR, close protocol; RandR negotiated but refused by this rootfs's Xvfb (letterbox fallback verified) |

Add a row per verified build; do not delete prior rows.

## Registering another Debian application

1. Install the application into `/debian/rootfs` with its runtime
   dependencies (i386, X11, software rendering).
2. Add a `[Desktop Entry]` to `/lib/rill/applications` following the
   Text Editor pattern; keep the title in `-t '...'` so the taskbar shows
   it correctly.
3. Exercise launch, typing, saving under `$home/Documents`, clipboard in
   both directions, and a clean close with no leftover `xbridge`, `Xvfb`,
   or input-worker processes.
4. Fill in the matrix row and the verification record.
