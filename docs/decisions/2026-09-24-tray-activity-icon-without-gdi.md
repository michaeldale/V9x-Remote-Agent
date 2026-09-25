# Tray icon: activity feedback drawn without GDI32

Date: 2026-09-24
Status: accepted

## Context

The notification-area icon (working since 0.6.2) was the stock
`IDI_APPLICATION` picture and never changed. The request was for visual
feedback while the agent is talking to a client or running a job: the body of
the little window, under its blue title bar, should flash green.

Two constraints shape the answer:

- `build-guest.ps1` fails the build if `V9XAGNT.EXE` imports GDI32. That rule
  exists because a GDI call from the long-lived agent goes through the Win9x
  display driver, which is exactly what wedged guests before screen capture
  was moved into the disposable `V9XSHOT.EXE`. So `CreateCompatibleBitmap`,
  `FillRect` and every other way of painting an icon at runtime is off the
  table.
- The agent is runtime-free C89 with no resource compiler step in its build,
  so the icons could not simply be `.ico` resources either without adding
  `wrc` to the pipeline and a second place for the picture to live.

## Decision

The agent builds the two icons itself as in-memory `RT_ICON` images (a
`BITMAPINFOHEADER` with doubled height, a 16-entry palette, a 4-bpp XOR image
and a 1-bpp AND mask, 296 bytes each) and turns each into an `HICON` with
`CreateIconFromResource`, a USER32 export present since Windows 95. That
format is device-independent, unlike `CreateIcon`'s colour bitmaps. The
picture is generated pixel by pixel from a tiny function so the idle (white
body) and busy (lime body) variants cannot drift apart. If creation fails the
agent logs `tray-createicon-failed` and falls back to `IDI_APPLICATION`
without flashing, so the worst case is the 0.6.2 behaviour.

Activity is a picture the tray thread samples, not events it is told about.
`V9xAgentState` gained two counters: `active_connections` (claimed connection
slots, `InterlockedIncrement`/`InterlockedDecrement` on claim and release) and
`activity` (bumped for every frame received in `v9x_serve_client` and every
frame sent by `v9x_send_frame`; only ever compared for change). The tray
worker, which already had to wake every 100 ms to pump its message queue,
reads them each slice. It is busy while a connection is open, an execution
slot is active, or the activity counter moved in the last 750 ms; while busy
it swaps the icon every 250 ms. The hold is what makes a `ping` blink: the
whole request finishes inside one sample period.

## Consequences

- No protocol change, no new configuration. The only new imports are
  `CreateIconFromResource`, `DestroyIcon`, `InterlockedIncrement` and
  `InterlockedDecrement`, all present on Windows 95 and added to the required
  import list in `build-guest.ps1`.
- `NIM_MODIFY` traffic rises from one call per 5 s to four per second while
  busy. Shell_NotifyIcon on 98 SE handles that comfortably; the idle cadence is
  unchanged.
- Explorer restarts are handled as before: a failed `NIM_MODIFY` or a missing
  `Shell_TrayWnd` drops back to `NIM_ADD`.
- Verified on the host side by build and import audit only; the flash needs a
  live guest to see. Expect `tray-icon-added` in `AGENT.LOG` as before, and no
  `tray-createicon-failed`.
