# Operating manual for AI coding agents

You (an AI coding agent) can fully control a Windows 9x guest through this
project. Everything is designed for you: one bounded command per step, JSON
output, deterministic exit codes, verified transfers, and proven reboots.
Nothing requires clicking inside the emulator.

## Two ways to connect

- **PowerShell CLI** (Windows hosts): `scripts\v9xctl.ps1`, one process per
  operation, add `-Json` for machine-readable output. Default target
  `127.0.0.1:9869`; override with `-Host`/`-Port`, or with `-Id <install id>`
  to find the guest by discovery and follow it across address changes.
  `.\scripts\v9xctl.ps1 help`
  prints the verb list with examples; a verb missing a required parameter
  prints its own usage line and exits 10.
- **MCP server** (any host with Python 3.9+): `mcp/v9x_mcp.py` exposes the
  same operations as MCP tools, including screenshots returned as images.
  Setup in [mcp/README.md](mcp/README.md).

## v9xctl verb reference

| Verb | Key parameters | Purpose |
|---|---|---|
| `discover` | `-DiscoverSeconds`, `-QueryAddress` | Find agents on the LAN by mDNS; no connection (agent 0.8.0+ with `discovery=1`; not under SLiRP) |
| `ping` | | Liveness; returns uptime and boot counter |
| `info` | | Agent version, build ID, computer name, screen mode, pending job |
| `inventory` | | OS build, CPU, memory, drives, display adapter, IPs, BIOS, PnP devices (agent 0.7.0+) |
| `exec` | `-Application`, `-Arguments`, `-WorkingDirectory`, `-TimeoutSeconds`, `-ExpectedExitCode`, `-ShowWindow`, `-Detach`, `-WaitForFile`/`-WaitForWindow` | Run a Win32 EXE directly, capture stdout/stderr; then optionally wait for a file or window |
| `shell` | `-Command`, `-TimeoutSeconds`, `-Detach` | Run through `COMMAND.COM /C` (`CMD.EXE /C` on Windows 2000; built-ins, batch files) |
| `stat` | `-Path` | Existence, size, attributes |
| `list` | `-Path` | Directory listing (bounded to 16 KiB) |
| `mkdir` | `-Path` | Idempotent directory creation |
| `put` | `-Source`, `-Destination` | Upload one file, CRC32-verified, transactional |
| `get` | `-Source`, `-Destination` | Download one file, CRC32-verified |
| `push-tree` | `-Source`, `-Destination` | Recursive directory upload |
| `cat` | `-Path`, `-Tail`, `-Match`, `-MaxBytes` | A guest text file printed inline: last N lines, lines containing text, bounded (agent 0.13.0+) |
| `hash` | `-Path`, optional `-Source` | Size, CRC32, SHA-256 in the guest; with `-Source`, `Matches` against a local file (0.13.0+) |
| `delete` | `-Path`, `-Recurse`, `-Force` | Delete a file or directory; refuses roots, WINDOWS, SYSTEM, `C:\V9XREMOTE` (0.13.0+) |
| `ps` | optional `-Name` | Processes; marks the agent and its exec jobs with their request id (0.13.0+) |
| `kill` | `-Pid` or `-Name` | Terminate a process (e.g. a stuck exec job); refuses the agent and Windows core processes (0.13.0+) |
| `reg` | `-Key HKLM\...` | Subkeys and values of one registry key, read-only (0.13.0+) |
| `wait-file` | `-Path`, `-WaitSeconds`, `-Gone`, `-StableMs` | Wait until a guest file exists (or is gone, or stops growing); any agent |
| `windows` | optional `-Title` | Visible top-level windows: title, class, position, size, pid (0.13.0+) |
| `wait-window` | `-Title`, `-WaitSeconds`, `-Gone` | Wait until a window whose title contains the text is shown (or closed) (0.13.0+) |
| `download` | `-Url`, `-Destination` | Guest fetches an `http://` URL to a guest path (http only, CRC32-verified) |
| `update` | `-Source`, `-WaitSeconds` | Upgrade the agent from an install-package folder: hot, no reboot; agents before 0.6.0 get the staged route with one reboot (`Mode = Staged`, v9xctl 0.9.1+) |
| `reboot` | `-JobId`, `-WaitSeconds` | Reboot with proof (see below) |
| `reset` | `-JobId`, `-WaitSeconds` | Win9x guest whose restart hangs: flush, hardware reset (CF9h), same proof as `reboot` (agent 0.10.0+) |
| `shutdown` | `-JobId` | Controlled shutdown |
| `wait-desktop` | `-WaitSeconds` | Block until Explorer's desktop is ready |
| `screenshot` | `-OutFile` (alias of `-Destination`), `-Path`, `-Window <title>` or `-Region x,y,w,h`, `-Scale`, `-Gray` | Guest screen capture; saved as PNG/JPEG/GIF/TIFF/BMP by the output file extension; window/region/scale/gray keep it small |
| `input` | `-Sequence`, optional `-OutFile`/`-SettleMs` (+ `-Region`/`-Scale`/`-Gray`) | Inject mouse/keyboard: move, click, drag, wheel, key/hotkey, type, delay; with `-OutFile`, settle and screenshot in the same call |
| `vnc` | `status`, `start` or `stop` | The guest's VNC server for a person (only where SETUP.EXE enabled it; agent package 0.11.0+) |

Any verb but `discover` and `vnc` also takes `-Relay M-XXXXXX` to drive a
remote-help machine (an owner's real hardware, lent over the Internet)
through the update server instead of `-Host`/`-Port`; MCP tools take
`relay`. Turning remote help on is the owner's consent to testing, so do not
wait for a reply before starting. A short `v9x_chat` heads-up that you are
starting is welcome. Those machines still belong to other people: expect the
owner to pause you. See [docs/remote-help.md](docs/remote-help.md).

## Host scripts beyond v9xctl

| Script | Purpose |
|---|---|
| `scripts\set-autologon.ps1` | Report/enable/disable Windows 9x autologon; `-Dismiss` clears a logon dialog that is up right now |
| `scripts\capture-emulator-window.ps1` | Grab the emulator's own window from the host, for states `screenshot` cannot reach (logon dialog, BIOS, shutdown hang) |
| `scripts\run-driver-cycle.ps1` | Upload, preflight and optionally apply a driver package |

`C:\V9XREMOTE\V9XVNC.EXE` in the guest is a VNC server for a person to watch
and drive the guest live. It is off unless the machine's owner enabled it in
SETUP.EXE; `vnc start` on such a guest exits 23, and enabling it is their
call, not yours. See [docs/guest-agent.md](docs/guest-agent.md). As an
agent, keep using `screenshot` and `input`.

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
| 45 | `ps`/`kill` failure: no such process, ambiguous `-Name`, or a refused target |
| 46 | `reg` failure (key not found) |
| 47 | `wait-file` / `-WaitForFile` deadline passed |
| 48 | `wait-window` / `-WaitForWindow` / `screenshot -Window`: no such window, or the deadline passed |

Treat 20 as "the VM is off, still booting, or the forward is missing". Do not
retry it in a loop of your own: add `-WaitForAgent <seconds>` and v9xctl
retries the connection with backoff (a connection dropped before HELLO is
retried twice even without it). Treat everything else as deterministic.

## Never sleep and poll

Every wait has a verb or option that returns the moment the condition holds.
A `Start-Sleep` between v9xctl calls is a bug: it is either too short (flaky)
or too long (slow), and each extra call costs a connection.

| You want to wait for... | Use |
|---|---|
| the agent to answer (guest booting) | `ping -WaitSeconds 120`, or `-WaitForAgent N` on any verb |
| a reboot or reset to finish | `reboot`/`reset -JobId <token> -WaitSeconds N` (proves it; no BootCounter loops) |
| the desktop after boot | `wait-desktop -WaitSeconds N` (also waits for the agent) |
| a program's window, then to look at it | `exec -Detach -WaitForWindow <title>`, then `screenshot -Window <title>` |
| a dialog to close | `wait-window -Title <text> -Gone` |
| a program to write its result file | `exec ... -WaitForFile <path>` or `wait-file -Path <path> [-StableMs 1000]` |
| a GUI step to repaint | `input -Sequence ... -OutFile shot.png` (`-SettleMs`, default 500) |

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
.\scripts\v9xctl.ps1 screenshot -OutFile .\after.png -Json

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
- Text files for the guest (`.INI`, `.BAT`, `.REG`, `.CFG`, `.INF`) must be
  CRLF before `put`: it is byte-exact, and a file written with an editor or
  tool that emits LF goes over as LF (the Write tool does). `put` warns when it
  sees one; convert the local file to CRLF and put it again. An LF-only
  `AGENT.INI` made agents before 0.10.0 append a new `install_id` on every
  start.
- Limits: 2 GiB per file for put and get (agent 0.11.0+, 256 MiB before), 2 GiB for a guest `download` (agent 0.8.1+), 1 MiB captured stdout and stderr each
  (excess is truncated and flagged), 16 KiB directory listings,
  1 hour maximum execution timeout.
- The guest serves up to four connections and four concurrent executions;
  `ping` and cancel stay responsive during a running execution. Reboot,
  shutdown, screenshot and update are refused with BUSY while any execution
  runs. When that BUSY is a job you no longer want, `ps` shows it
  (`AgentJob`, `JobRequestId`) and `kill -Pid` ends it; its `exec` then
  completes normally.
- Use the verb, not a workaround (agent 0.13.0+): `cat -Tail 40 -Match error`
  rather than `get` + reading the file, `hash -Source` rather than a
  download to verify a push, `delete` rather than `shell DEL`, `reg` rather
  than `REGEDIT /E` + `get`. `stat`, `get`, `hash` and `cat` flush Windows
  9x's INI write cache first, so an INI another program just wrote with
  `WritePrivateProfileString` reads as it is now; never use `shell TYPE` for
  that (it starts a DOS VM).

## Failure handling rules

- **A reboot is proven, never assumed.** `BootCounter` increments whenever the
  agent starts, so a larger value alone is not reboot proof. Require the old
  connection to end, a new connection with the exact `-JobId` token from
  `PENDING.DAT`, and `wait-desktop` success. If recovery is uncertain, use
  `info`, execute a trace dumper directly, and download trace files first.
- Call `wait-desktop` after every reboot and before any GUI `exec` or
  `screenshot`; services start before the shell does.
- **`reset` is for a guest whose Windows restart hangs, not a faster
  `reboot`.** If `reboot` (or Start > Shut Down > Restart) stops at a flashing
  cursor or a fatal exception 0C because a real-mode driver is still resident,
  use `reset`: it flushes the registry and disks and resets the hardware
  without returning to real mode, with the same proof as `reboot`. Everywhere
  else use `reboot`. Windows 9x only (exit 23 elsewhere). See
  [docs/guest-agent.md](docs/guest-agent.md).
- GUI programs: direct `exec` of a GUI-subsystem EXE leaves window state to
  the program (the JSON reports `GuiWindow: true`). Console programs and
  `shell` commands run hidden by default; pass `-ShowWindow` to show them.
- Driving the GUI: prefer `exec`/`shell` when a program can be launched
  headless. Use `input` (mouse/keyboard) when you must click a dialog, drive
  an installer, or reach something only the UI exposes. Screenshot first to
  read coordinates, then act and confirm in one call: `input -OutFile
  after.png` (MCP: `screenshot: true` on any input tool) settles for
  `-SettleMs`/`settle_ms` (default 500) and captures. Do not write your own
  input-wait-screenshot helper. Input goes to the
  focused window (see below).
- Keep screenshots small: an image costs tokens in proportion to its pixels,
  and a full 1024x768 frame is the most expensive thing you can look at. Add
  `-Region x,y,w,h` to look at one dialog, `-Scale 0.5` when layout is enough,
  `-Gray` to read text (MCP: `region`, `scale`, `gray`). Do not crop or
  convert with your own System.Drawing code. With a region or scale, map an
  image point back with `screenX = RegionX + imageX * RegionWidth / Width`.
  Agents 0.13.0+ shape and losslessly pack the image before sending it, which
  is what makes screenshots over `-Relay` fast. Input goes to the
  focused window, so bring it forward (click its title bar) before typing, and
  avoid `ALT+F4` on the bare desktop (it raises the Shut Down dialog).
- A GUI program that only exits from its paint path needs a visible desktop;
  if it times out, check `info` and `wait-desktop` before considering capture.
- Never request a screenshot during a fullscreen transition or a suspected
  display wedge. A screenshot invokes the Win98 display driver through GDI.
  Capture only on a confirmed stable desktop/window. If the agent disappears,
  recover externally and check trace files before launching more graphics code.
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
- **A guest with `allow_dos_vm=0` in `AGENT.INI` refuses `shell` (exit 23,
  "does not advertise shell support") and `exec` of console or DOS programs
  (`UNSUPPORTED_OPERATION`).** Its display driver corrupts the screen when a
  DOS VM starts, so this is protection, not a fault. Use `get`/`put`/`stat`/
  `list`/`mkdir` for file work and `exec` only Win32 GUI executables there.
- **`wait-desktop` timing out (44) while `ping` still succeeds means nobody has
  logged on, not that the agent died.** `RunServices` starts the agent before
  the logon dialog, so on a guest that prompts at boot everything except
  `screenshot` (43) and `wait-desktop` (44) works while that dialog sits there;
  `info` answers with `DesktopReady: false`. Clear it with
  `.\scripts\set-autologon.ps1 -Dismiss`, or turn the prompt off for good with
  `-Enable`. Never conclude a guest is hung from `wait-desktop` alone: call
  `info` first.

## Hard rules

- Never bridge the guest NIC or expose TCP 9869 beyond localhost; the
  protocol is unauthenticated by design and equals full guest control. The
  same goes for the VNC port (5900) while `V9XVNC.EXE` runs.
- Never disable the `allowed_client` allowlist on a physical machine
  deployment.
- Upgrade an agent of any age with `update` alone. Do not run
  `INSTALL.BAT`/`UPDATE.BAT` semantics by hand over exec unless asked: for an
  agent before 0.6.0 `update` already drives that staged route itself with
  fixed 8.3 paths, and a hand-picked long folder name stops the boot-time
  `UPDATE.BAT` at a prompt (see [docs/guest-agent.md](docs/guest-agent.md)).
- The guest is a development sandbox: assume anything in it can be lost, and
  keep sources and results on the host.
