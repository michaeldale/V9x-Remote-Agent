# Changelog

All notable changes to the V9x Remote Agent. Dates are in YYYY-MM-DD.

## 0.6.3 (2026-09-24)

### Added
- The notification-area icon now shows activity. Its body flashes lime while
  any client is connected or any execution is running, and a short hold keeps
  it flashing after the last frame so even a bare `ping` blinks once. The
  agent counts claimed connection slots and every frame it receives or sends;
  the tray worker samples those counters every 100 ms and swaps the icon at
  250 ms intervals. Nothing about the protocol changes.
- The icon itself is now drawn by the agent as a 16-colour icon resource in
  memory (`CreateIconFromResource`) instead of borrowing `IDI_APPLICATION`:
  the agent may not import GDI32, so this is the only way to have two colour
  variants of one picture. If icon creation fails the agent logs
  `tray-createicon-failed` and falls back to the stock icon without flashing.
- `scripts\v9xctl.ps1` explains itself. Running it with no verb, an unknown
  verb, or `help` prints the verb list, the connection options (`-Host`,
  `-Port`, `-ConnectTimeoutSeconds`) and worked examples instead of an
  interactive prompt for `Action`; a verb missing its required parameter
  prints that verb's usage line and an example. Comment-based help backs
  `Get-Help .\scripts\v9xctl.ps1 -Examples`, verbs tab-complete, and a
  connection failure now names the target it tried and how to override it.

## 0.6.2 (2026-09-04)

### Fixed
- The notification-area icon, which had never appeared on any guest in any
  release that shipped it. `v9x_tray_start` passed a NULL `lpThreadId` to
  `CreateThread`, which Windows 9x rejects with `ERROR_INVALID_PARAMETER`
  (Windows NT accepts it, so no modern host ever flagged the call). The window
  and icon were created fine; the worker thread never existed, so
  `Shell_NotifyIcon` was never reached. Measured as `tray-thread-failed gle=87`
  on 98 SE. The owner window now also belongs to the worker thread and that
  thread pumps its own message queue, instead of the window being created on
  the main thread that blocks in `accept()` forever.
- Tray failures are no longer silent. `v9x_tray_start` returned 0 for three
  different reasons behind one `tray-icon-failed` line, and a failing
  `Shell_NotifyIcon` was retried every second with its result discarded, so a
  wholly broken feature could not be diagnosed from the log. Each failure now
  names itself with its `GetLastError` (`tray-window-failed`,
  `tray-loadicon-failed`, `tray-thread-failed`, `tray-notify-failed`) and
  success logs `tray-icon-added`.

### Added
- `scripts\set-autologon.ps1`: report, enable or disable Windows 9x autologon
  on a guest, and clear a logon dialog that is already up (`-Dismiss`). Purely
  host-side, composed from existing verbs, no protocol change.
- `scripts\capture-emulator-window.ps1`: capture the emulator's own window from
  the host. `screenshot` needs Explorer, so it returns exit 43 for precisely
  the states worth seeing - a logon dialog, a BIOS prompt, a shutdown hang.
  This reads the window instead and needs nothing from the guest.
- Documented what the agent can and cannot do before anyone logs on.
  `RunServices` does start it before the logon dialog and everything except
  `screenshot` and `wait-desktop` works there, so a guest that prompts at boot
  stalls automation at its first `wait-desktop` while `ping` keeps succeeding.
  `AGENTS.md` now says to call `info` before concluding a guest is hung.

## 0.6.1 (2026-08-21)

### Fixed
- Windows 95 support. The agent imported `KERNEL32:InterlockedCompareExchange`,
  which Windows 95 does not export (it arrived in Windows 98 and NT4), so the
  loader refused to start `V9XAGNT.EXE` ("linked to missing export"). Every
  compare-exchange was a 0/1 flag acquire or a plain read, so they are now the
  `v9x_flag_acquire`/`v9x_flag_read` macros built on `InterlockedExchange`
  (exported since Windows 95) and an aligned volatile read. Verified on a real
  Windows 95 486; behaviour on 98 SE is unchanged.
- `packaging/` text files are CRLF on disk again and a `.gitattributes` pins
  them (`packaging/** text eol=crlf`, plus `*.BAT`/`*.REG`/`*.INI`/`*.CFG`).
  They had been LF-only in the working tree since 16 Aug, so anything copied
  straight from `packaging/` (bypassing `build-guest.ps1`, which normalises)
  produced batch files that Win9x `COMMAND.COM` misparses with "Bad command or
  file name" on every line.

## 0.6.0 (2026-08-18)

### Added
- Concurrent execution pool. The agent now runs up to four `EXEC_REQUEST`
  jobs at once instead of one; `V9X_STATUS_BUSY` is returned only when every
  slot is in use. `EXEC_STDOUT`/`EXEC_STDERR`/`EXEC_COMPLETE` are already keyed
  by request id, so streams interleave, and `CANCEL_REQUEST` is scoped to the
  owning connection and request id.
- Multiple simultaneous client connections. The listener serves each accepted
  socket on its own thread with its own send lock and buffers, up to four
  concurrent connections (excess are rejected). Reboot, shutdown, and
  screenshot remain machine-wide and are refused while any execution is active.
  Request ids are unique only per connection.
- Upgrade without reboot. `UPDATE_REQUEST` (`0x0042`) verifies the size and
  CRC32 of already-uploaded `V9XNEW.EXE`/`V9XSNEW.EXE`, writes `HOTSWAP.BAT`,
  launches it detached, replies `UPDATE_ACCEPTED`, and exits so the batch can
  swap the now-unlocked binaries and relaunch the agent. The RunServices entry
  is never repointed, so an interrupted swap still recovers on the next boot.
  Wires the previously reserved `V9X_CAP_DRIVER_UPDATE` capability bit.
- HTTP download. `DOWNLOAD_REQUEST` (`0x0027`) fetches an `http://` URL straight
  to a guest path over a hand-rolled HTTP/1.0 client (no new DLL dependency),
  streaming `DOWNLOAD_PROGRESS` and committing through the same transactional
  `.PART`/backup path as uploads. HTTPS is rejected with a clear error. New
  `V9X_CAP_HTTP_DOWNLOAD` capability bit.
- Structured audit log. `AGENT.LOG` lines now carry a local timestamp, the boot
  counter, and per-event detail (executed command, transferred path and CRC,
  download URL and HTTP status, and a full per-command dispatch trail). The log
  rotates to `AGENT.OLD` past 256 KB.

## 0.5.3 (2026-08-16)

### Fixed
- `INSTALL.BAT` no longer aborts on real Windows 98 SE hardware. It guarded its
  target directory with a bare `IF NOT EXIST C:\V9XREMOTE`, but Win98 SE
  `COMMAND.COM` evaluates `IF EXIST <directory>` as false for a directory that
  exists. The guard therefore fired on every run, `MD` failed with "Unable to
  create directory", and the install exited with "ERROR: V9x Remote Agent files
  could not be copied" without installing or staging anything. Both guards now
  test `C:\V9XREMOTE\NUL`. Found installing onto a Gateway Solo 2150.
- `INSTALL.BAT` applies its registry file before clearing read-only attributes,
  and clears them one named file at a time instead of sweeping
  `C:\V9XREMOTE\*.*`. The wildcard sweep also walked the `TEMP` and `JOBS`
  subdirectories and could fail with "General failure reading drive C", which
  aborted the batch *after* the new binaries were staged but *before*
  `UPDATE.REG` was applied - leaving a staged update that would never install
  and a machine that silently kept running the old agent.

## 0.5.2 (2026-08-13)

### Fixed
- Screenshot capture and all GDI screen probing now run in the disposable
  `V9XSHOT.EXE` helper. A display-driver GPF or blocked GDI call can no longer
  directly crash the network agent; the agent terminates a helper that has not
  completed within 7.5 seconds and returns a normal screenshot error.
- Capture is rejected unless Explorer's desktop is ready, and the helper
  refuses a mode whose dimensions change during a short stability sample.
- `V9XAGNT.EXE` no longer imports GDI32. HELLO and INFO use USER screen metrics
  and report bits-per-pixel as unknown (`0`); successful screenshot responses
  still report the source colour depth measured by the helper.
- The Win98 install and staged-update paths deploy the helper with the matching
  agent build.
- `INSTALL.BAT` validates the complete package before modifying the guest,
  checks every required copy, and removes both staged binaries if either copy
  fails. `UPDATE.BAT` installs the helper before activating the matching agent.

### Changed
- Recovery guidance now forbids screenshots during fullscreen mode changes or
  suspected display wedges. Use INFO, direct trace-dump execution, and file
  download first, and capture only after desktop readiness is re-established.
- `BootCounter` is documented as an agent-start counter, not independent reboot
  proof; reconnect, the persisted resume token, and desktop readiness are
  required before post-reboot GUI work.

## 0.5.1 (2026-08-12)

Fixes a live execution wedge: `shell -Command "start SETUP.EXE"` spawned a
detached child that kept the Windows 9x `COMMAND.COM` wrapper (and the
inherited pipe write handles) alive, so the finished request held the guest's
single execution slot for its full timeout and other connections were refused
for 60 to 120 seconds.

### Added
- Detached execution: `v9xctl.ps1 exec`/`shell -Detach` (MCP: `detach: true`
  on `v9x_exec`/`v9x_shell`) launches the child with all standard handles on
  `NUL` and completes immediately without capturing output or waiting. Use it
  instead of `START` for installers and other programs that outlive the
  request. Request option bit `0x0001`, completion flag `0x40`, capability
  bit `V9X_CAP_EXEC_DETACH` (0x800).

### Fixed
- Capture pipes are now created non-inheritable and the child receives
  inheritable duplicates of only the write ends (via `DuplicateHandle`), so
  agent-side read handles can no longer leak into a child's descendants.
- Shell mode no longer burns the whole timeout when the command finished but
  its DOS-VM wrapper lingers because of `START`-spawned or Win16
  descendants: once every pipe write handle has closed and the process
  handle is still unsignaled after a 2 s grace, the agent discards the
  wrapper and completes the request successfully with new completion flag
  `0x80` (`Orphaned`). Windows 9x has no job objects, so waiting on the
  descendant tree is impossible by design.

## 0.5.0 (2026-08-11)

First public release, focused on packaging the project for other people and
for AI coding agents, plus one new guest capability.

### Added
- A Windows notification-area icon now shows that the guest agent is running;
  its hover tooltip reports the agent version, listening port, and IPv4 address.
- Input injection: a new `INPUT_REQUEST` protocol operation and guest handler
  (`src/guest/input.c`) inject batched mouse and keyboard events (move, click,
  drag, wheel, key/hotkey, typed ASCII, delay), applied atomically after
  whole-batch validation. Exposed as `v9xctl.ps1 input -Sequence "..."` and
  MCP tools `v9x_click`, `v9x_move`, `v9x_type`, `v9x_key`, `v9x_scroll`, and
  `v9x_input`. This lets an agent drive dialogs and installers that only the
  GUI exposes; capability bit `V9X_CAP_INPUT_INJECT` (0x400) advertises it.
- MCP server (`mcp/v9x_mcp.py`): standard-library-only Python 3.9+, speaks
  the V9XR protocol natively (no PowerShell needed, so macOS/Linux hosts
  work), and exposes 20 tools including screenshots returned as PNG images
  the model can see. Offline test suite in `mcp/test_v9x_mcp.py`.
- Install CD builder (`scripts/make-install-media.ps1`): writes a plain
  ISO9660 image of the guest package with no external tools, so a fresh
  Windows 9x guest can be set up by mounting `V9XREMOTE.ISO` and running
  `INSTALL.BAT`. `-Validate` mounts the ISO on the host and verifies every
  file hash.
- Release tooling: `scripts/make-release.ps1` (zip + ISO + SHA256SUMS) and
  GitHub Actions workflows for CI and tagged releases with prebuilt
  binaries.
- Public documentation: rewritten `README.md`, `docs/quickstart-86box.md`
  (zero-to-working 86Box guide), `docs/ai-workflows.md`, `AGENTS.md`
  (operating manual for AI agents), `SECURITY.md`, and `CLAUDE.md`.

### Changed
- License changed from all-rights-reserved to BSD 2-Clause.
- `INSTALL.BAT` clears the read-only attribute after copying, so installs
  from CD media no longer leave read-only files that would break staged
  updates.
- The package version is stamped from `include/v9xremote/version.h` at build
  time, and the build fails if the changelog disagrees.
- The design plan moved to `docs/design.md` with environment-specific paths
  generalized.

## 0.4.2 (2026-08-10)

### Fixed
- Direct execution of GUI applications no longer hangs paint-driven programs.
  The agent previously always set `STARTF_USESHOWWINDOW` with `SW_HIDE` by
  default, which Windows substitutes into the child's first `ShowWindow`
  call; a hidden GUI window never receives `WM_PAINT`, so programs such as
  `V9XGDI.EXE /auto` idled until the execution timeout. Direct mode now reads
  the target's PE optional header and, for Windows GUI subsystem executables,
  leaves the initial window state to the application, matching launch via
  `START`. Shell mode and console-subsystem targets keep the previous
  hidden-by-default behaviour.
- `v9xctl.ps1 exec` accepts an empty or omitted `-Arguments` value, so
  programs that take no arguments can be launched in direct mode.

### Added
- New `EXEC_COMPLETE` flag `0x20` (`V9X_EXEC_FLAG_GUI_WINDOW`) reports when
  the GUI-window rule was applied; `v9xctl.ps1` surfaces it as `GuiWindow`.
- `CHANGELOG.md` (this file).

### Changed
- The guest package is now written to `build\install`, which always holds the
  most recently built version. The old `build\package` default and the
  accumulated per-milestone package folders were removed, and the docs now
  point at `build\install`.

## 0.4.1 (2026-08-09)

### Added
- Physical-machine support: configurable IPv4 listen address and TCP port,
  plus a single-client IPv4 allowlist for isolated development networks, all
  read from `C:\V9XREMOTE\AGENT.INI`. `HELLO` and `INFO` report the configured
  values. See `docs/physical-machine.md`.

## 0.4 (2026-08-09)

### Added
- Controlled reboot and shutdown with persisted resume tokens
  (`PENDING.DAT`) and boot-counter proof, so the host can verify that the
  intended boot completed.
- Desktop readiness reporting for post-boot sequencing.
- CRC32-verified 24-bit BMP screenshot capture and transfer.
- `run-driver-cycle.ps1` host orchestration for the Velocity9x
  build/install/reboot/test loop.

## 0.3 (2026-08-09)

### Added
- Initial public feature set consolidated from the M1-M3 development builds:
  framed TCP protocol with HELLO/PING/INFO, direct and `COMMAND.COM /C` shell
  execution with bounded piped output, timeouts and cancellation, and
  CRC32-verified file stat/list/mkdir/upload/download with `.PART` staging
  and rollback backups.
- Windows 98 packaging with install, staged-update, and uninstall scripts
  registered through `RunServices`.
