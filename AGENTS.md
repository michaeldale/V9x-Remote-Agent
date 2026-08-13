# Operating manual for AI coding agents

You (an AI coding agent) can fully control a Windows 9x guest through this
project. Everything is designed for you: one bounded command per step, JSON
output, deterministic exit codes, verified transfers, and proven reboots.
Nothing requires clicking inside the emulator.

## Two ways to connect

- **PowerShell CLI** (Windows hosts): `scripts\v9xctl.ps1`, one process per
  operation, add `-Json` for machine-readable output. Default target
  `127.0.0.1:9869`; override with `-Host`/`-Port`.
- **MCP server** (any host with Python 3.9+): `mcp/v9x_mcp.py` exposes the
  same operations as MCP tools, including screenshots returned as images.
  Setup in [mcp/README.md](mcp/README.md).

## v9xctl verb reference

| Verb | Key parameters | Purpose |
|---|---|---|
| `ping` | | Liveness; returns uptime and boot counter |
| `info` | | Agent version, build ID, computer name, screen mode, pending job |
| `exec` | `-Application`, `-Arguments`, `-WorkingDirectory`, `-TimeoutSeconds`, `-ExpectedExitCode`, `-ShowWindow`, `-Detach` | Run a Win32 EXE directly, capture stdout/stderr |
| `shell` | `-Command`, `-TimeoutSeconds`, `-Detach` | Run through `COMMAND.COM /C` (built-ins, batch files) |
| `stat` | `-Path` | Existence, size, attributes |
| `list` | `-Path` | Directory listing (bounded to 16 KiB) |
| `mkdir` | `-Path` | Idempotent directory creation |
| `put` | `-Source`, `-Destination` | Upload one file, CRC32-verified, transactional |
| `get` | `-Source`, `-Destination` | Download one file, CRC32-verified |
| `push-tree` | `-Source`, `-Destination` | Recursive directory upload |
| `reboot` | `-JobId`, `-WaitSeconds` | Reboot with proof (see below) |
| `shutdown` | `-JobId` | Controlled shutdown |
| `wait-desktop` | `-WaitSeconds` | Block until Explorer's desktop is ready |
| `screenshot` | `-Destination`, `-Path` | 24-bit BMP of the guest screen |
| `input` | `-Sequence` | Inject mouse/keyboard: move, click, drag, wheel, key/hotkey, type, delay |

## Exit-code contract

| Code | Meaning |
|---|---|
| 0 | Success |
| 20 | Connection or transport timeout failure |
| 22 | Handshake or protocol violation |
| 23 | Guest protocol error or missing capability |
| 30 | Guest process could not start |
| 31 | Child exited with an unexpected exit code |
| 32 | Child timed out (and was terminated) |
| 33 | Child was cancelled |
| 40 | File transfer, file I/O, or CRC failure |
| 41 | Power-control rejection (e.g. an execution is active) |
| 42 | Reboot reconnect proof failed |
| 43 | Screenshot capture failure |
| 44 | Desktop-ready timeout |

Treat 20 as "the VM is off, still booting, or the forward is missing"; retry
with backoff before concluding failure. Treat everything else as
deterministic.

## The golden loop

Build on the host, deploy, run, observe, prove. Copy-paste skeleton:

```powershell
# 1. Deploy (put one file, or push-tree for a package)
.\scripts\v9xctl.ps1 mkdir -Path C:\V9XREMOTE\JOBS\myjob
.\scripts\v9xctl.ps1 put -Source .\out\MYAPP.EXE -Destination C:\V9XREMOTE\JOBS\myjob\MYAPP.EXE -Json

# 2. Run with a bounded timeout; parse the JSON
.\scripts\v9xctl.ps1 exec -Application C:\V9XREMOTE\JOBS\myjob\MYAPP.EXE `
    -Arguments "/auto" -WorkingDirectory C:\V9XREMOTE\JOBS\myjob `
    -TimeoutSeconds 60 -ExpectedExitCode 0 -Json

# 3. Collect results the program wrote
.\scripts\v9xctl.ps1 get -Source C:\V9XREMOTE\JOBS\myjob\RESULT.INI -Destination .\result.ini

# 4. Look at the screen when output alone is not enough
.\scripts\v9xctl.ps1 screenshot -Destination .\after.bmp -Json

# 5. When the change needs a reboot (drivers, RunServices, system files)
.\scripts\v9xctl.ps1 reboot -JobId myjob-reboot-1 -WaitSeconds 180 -Json
.\scripts\v9xctl.ps1 wait-desktop -WaitSeconds 120 -Json
```

A fully annotated version with sample outputs is in
[docs/ai-workflows.md](docs/ai-workflows.md).

## Guest conventions

- Work under `C:\V9XREMOTE\JOBS\<job-id>\`; keep `C:\V9XREMOTE` itself for
  the agent's own files (`AGENT.LOG`, `BOOT.DAT`, `PENDING.DAT`, config).
- Prefer uppercase 8.3 file names; paths are ASCII only (the protocol rejects
  non-ASCII). No spaces in guest paths avoids COMMAND.COM quoting pain.
- Limits: 64 MiB per file transfer, 1 MiB captured stdout and stderr each
  (excess is truncated and flagged), 16 KiB directory listings,
  1 hour maximum execution timeout.
- The guest serves one client at a time and one execution at a time; `ping`
  and cancel stay responsive during a running execution.

## Failure handling rules

- **A reboot is proven, never assumed.** `reboot` succeeds only when the
  agent reconnects with a higher boot counter and echoes back the exact
  `-JobId` token from `PENDING.DAT`. If exit code is 42, the guest may be
  hung: capture a screenshot before touching anything else.
- Call `wait-desktop` after every reboot and before any GUI `exec` or
  `screenshot`; services start before the shell does.
- GUI programs: direct `exec` of a GUI-subsystem EXE leaves window state to
  the program (the JSON reports `GuiWindow: true`). Console programs and
  `shell` commands run hidden by default; pass `-ShowWindow` to show them.
- Driving the GUI: prefer `exec`/`shell` when a program can be launched
  headless. Use `input` (mouse/keyboard) when you must click a dialog, drive
  an installer, or reach something only the UI exposes. Screenshot first to
  read coordinates, act, then screenshot again to confirm. Input goes to the
  focused window, so bring it forward (click its title bar) before typing, and
  avoid `ALT+F4` on the bare desktop (it raises the Shut Down dialog).
- A GUI program that only exits from its paint path needs a visible desktop;
  if it times out, screenshot first, then check `wait-desktop`.
- Long operations: `exec` streams output live and enforces `-TimeoutSeconds`
  on the guest side; a timed-out child is terminated (exit code 32). Pick
  timeouts explicitly rather than relying on the 60 s default.
- Launching a program that outlives the request (an installer you will drive
  with `input`, or anything you would reach for `START` to do): pass
  `-Detach`. Never run `shell -Command "start FOO.EXE"`: on Windows 9x the
  detached child keeps the `COMMAND.COM` wrapper alive, so the request holds
  the execution slot until its timeout. A detached exec returns immediately
  with `Detached: true` and captures no output; an `Orphaned: true` result
  means a non-detached shell command left descendants running.
- If the agent refuses connections right after a reboot, it is normal: retry
  `ping` for up to ~2 minutes while Windows boots.

## Hard rules

- Never bridge the guest NIC or expose TCP 9869 beyond localhost; the
  protocol is unauthenticated by design and equals full guest control.
- Never disable the `allowed_client` allowlist on a physical machine
  deployment.
- Do not run `INSTALL.BAT`/`UPDATE.BAT` semantics by hand over exec unless
  asked; agent updates have a staged flow (see
  [docs/guest-agent.md](docs/guest-agent.md)).
- The guest is a development sandbox: assume anything in it can be lost, and
  keep sources and results on the host.
