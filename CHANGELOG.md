# Changelog

All notable changes to the V9x Remote Agent. Dates are in YYYY-MM-DD.

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
