# Host client

`scripts\v9xctl.ps1` supports control, execution, and file operations, defaults to
`127.0.0.1:9869`, and performs a new handshake for each invocation.

Use `-Json` for compact machine-readable output. Use `-Host`, `-Port`, and
`-ConnectTimeoutSeconds` to override connection settings.

For a physical target, pass its LAN address directly:

```powershell
.\scripts\v9xctl.ps1 info -Host 192.168.10.98 -Port 9869 -Json
```

Getting help:

- `.\scripts\v9xctl.ps1 help` (or no verb at all) prints the verb list, the
  connection options and worked examples. With no verb the text goes to
  stderr and the exit code is 10; `help` writes to stdout and exits 0.
- A verb without its required parameter (`exec` without `-Application`, `put`
  without `-Destination`, and so on) prints that verb's usage line, an
  example, and the target it would have used, then exits 10.
- `Get-Help .\scripts\v9xctl.ps1 -Examples` shows the comment-based help;
  `-?` works too. Verbs tab-complete.
- A connection failure (exit 20) names the address and port it tried and
  reminds you of `-Host`/`-Port`, since a forgotten `-Host` is the usual
  cause.

See [physical-machine.md](physical-machine.md) for guest configuration and
containment requirements.

Finding agents by discovery (agent 0.8.0+ with `discovery=1`):

```powershell
.\scripts\v9xctl.ps1 discover                         # table of agents that answered
.\scripts\v9xctl.ps1 discover -DiscoverSeconds 5 -Json
.\scripts\v9xctl.ps1 info -Id 11d8b52d-b5e6-4bf2-ae56-8d39cd39c0ca
```

- `discover` opens no agent connection. It multicasts one mDNS query for
  `_v9xremote._tcp.local` on every up IPv4 interface and lists each agent's
  instance name, address, port, version and install ID.
- It exits 20 when nothing answers, with a hint about `discovery=1` and
  SLiRP.
- `-Id <install id>` works with every verb: discovery finds that install and
  sets `-Host`/`-Port` from its SRV and A records. `reboot`, `update` and
  their reconnect loops look it up again while waiting, so a guest that comes
  back on a new DHCP address is still followed.
- `-QueryAddress <ip[:port]>` sends the query unicast to one address instead
  of multicasting, for routed subnets or a forwarded UDP port.
- `-Relay M-XXXXXX` works with every verb except `discover` and `vnc`: it
  drives a remote-help machine through the update server instead of
  `-Host`/`-Port` (agent 0.12.0+ with Remote help On; see
  [remote-help.md](remote-help.md)). It needs the operator key in
  `%USERPROFILE%\.v9x\operator.key` (or `V9X_OPERATOR_KEY`), the signing key
  in `%USERPROFILE%\.v9x\relay-signing.key` and `build\tools\v9xsign.exe`.
  `-RelayServer` (or `V9X_RELAY_SERVER`) points at another copy of the
  server, e.g. `http://localhost/bluetraitblog` for testing. The first verb
  after an idle spell waits up to a minute for the machine to check in.

`update -Source <package dir>` hot-swaps agents from 0.6.0 on. For an older
agent (INFO `AgentVersion` below 0.6.0; the capability bit cannot tell) it
takes the staged route instead (0.9.1+): copy the package to `C:\V9XPKG`, run
its `INSTALL.BAT` there, check `V9XNEW.EXE` is staged, reboot so `UPDATE.BAT`
swaps it in, confirm the agent reports the package's version with nothing left
staged, then remove `C:\V9XPKG`. That takes about a minute and one reboot
(waiting at least 300 s whatever `-WaitSeconds` says), creates DOS VMs, and
reports `Mode = Staged`. It exits 41 when staging fails and 42 when the update
did not apply (`C:\V9XREMOTE\UPDATE.LOG` on the guest says why).
- `-DiscoverSeconds` (default 3) is how long to listen. With `-Id`, listening
  stops as soon as the install answers.
- `info` shows `InstallId` for 0.8.0+ agents (empty for older ones).

Exit codes:

- `0`: success
- `20`: connection or timeout failure
- `22`: handshake or protocol violation
- `23`: guest protocol error or missing capability
- `30`: guest process could not start
- `31`: unexpected child exit code
- `32`: child timed out
- `33`: child was cancelled
- `40`: file transfer, file I/O, or CRC failure
- `41`: power-control guest rejection
- `42`: reboot reconnect proof failed
- `43`: screenshot capture failure
- `44`: desktop-ready timeout or query failure

Examples:

```powershell
.\scripts\v9xctl.ps1 shell -Command "VER"
.\scripts\v9xctl.ps1 exec -Application C:\V9XREMOTE\TEST.EXE `
    -Arguments "/quiet" -WorkingDirectory C:\V9XREMOTE `
    -TimeoutSeconds 60 -ExpectedExitCode 0 -Json
```

`-ShowWindow` makes the child's window visible; without it the agent launches
the child hidden. The flag only affects shell commands and console-subsystem
executables: a direct-mode GUI-subsystem executable always controls its own
window (hidden GUI windows never paint on Windows 9x), and the result reports
`GuiWindow = True` when that rule applied.

`-Detach` launches the child and returns immediately without capturing output
or waiting for exit; the result reports `Detached = True` and a nominal exit
code 0. Use it instead of `shell -Command "start FOO.EXE"` for installers and
other programs that outlive the request: a START-spawned child keeps the
Windows 9x `COMMAND.COM` wrapper alive, which would otherwise hold the guest's
single execution slot until the timeout. When a non-detached shell command
finishes its output but leaves such descendants behind, the agent completes
the request after a short grace and reports `Orphaned = True`.

File examples:

```powershell
.\scripts\v9xctl.ps1 stat -Path C:\V9XREMOTE -Json
.\scripts\v9xctl.ps1 list -Path C:\V9XREMOTE -Json
.\scripts\v9xctl.ps1 mkdir -Path C:\V9XREMOTE\JOBS\demo
.\scripts\v9xctl.ps1 put -Source .\TEST.EXE `
    -Destination C:\V9XREMOTE\JOBS\demo\TEST.EXE
.\scripts\v9xctl.ps1 get -Source C:\V9XREMOTE\AGENT.LOG `
    -Destination .\agent.log
.\scripts\v9xctl.ps1 push-tree -Source .\package `
    -Destination C:\V9XREMOTE\JOBS\demo
```

Uploads and gets are limited to 2 GiB per file on agent 0.11.0+
(256 MiB on older agents and 64 MiB before 0.6.4, which reject larger
transfers with exit 40); a guest `download` is limited to 2 GiB on agent
0.8.1+. `put` commits only after
the guest verifies size and CRC32. `get` writes a uniquely named local `.part`
and replaces the requested destination only after matching the guest's final
size and CRC32.

File, process and registry tools (agent 0.13.0+; exit 23 on older agents):

```powershell
.\scripts\v9xctl.ps1 cat -Path C:\V9XREMOTE\AGENT.LOG -Tail 40 -Match error
.\scripts\v9xctl.ps1 hash -Path C:\WINDOWS\SYSTEM\V9X.DRV -Source .\build\V9X.DRV -Json
.\scripts\v9xctl.ps1 delete -Path C:\V9XREMOTE\JOBS\demo -Recurse -Json
.\scripts\v9xctl.ps1 ps
.\scripts\v9xctl.ps1 kill -Name NOTEPAD.EXE -Json
.\scripts\v9xctl.ps1 reg -Key HKLM\System\CurrentControlSet\Services\Class\Display -Json
```

- `cat` prints the text itself (with `-Json`: the text plus `Lines`,
  `Matched`, `Returned`, `Truncated`). `-Tail N` keeps the last N lines after
  `-Match` filtering (case-insensitive substring); `-MaxBytes` (default 16384,
  at most 65512) bounds the answer, and a cut is reported on stderr.
- `hash` returns `Size`, `Crc32` and `Sha256`; with `-Source <local file>`
  also `LocalSha256` and `Matches`.
- `delete` removes a file, an empty directory, or with `-Recurse` a tree;
  `-Force` clears read-only/hidden/system attributes. It refuses drive roots,
  the Windows and system directories, `C:\V9XREMOTE` and any folder holding
  one of them (exit 40).
- `ps` lists processes with `Agent` and `AgentJob`/`JobRequestId` (a process
  an `exec` started); `-Name` filters with wildcards. `kill -Pid <id>` or
  `-Name <image>` (exactly one match) terminates it; a killed exec job's
  `exec` call then returns. Exit 45 for no match, several matches, or a
  refused target (the agent, Windows core processes).
- `reg` returns `Subkeys` and `Values` (`Name`, `Type`, decoded `Data`) of one
  key; hives `HKLM`, `HKCU`, `HKCR`, `HKU`, `HKCC`, `HKDD`. Read-only. Exit 46
  when the key does not exist.
- `stat`, `get`, `hash` and `cat` flush Windows 9x's INI write cache first, so
  an INI a running program just wrote reads current (see docs\protocol.md).

Waiting without sleeping:

```powershell
.\scripts\v9xctl.ps1 ping -WaitSeconds 120 -Json             # until the agent answers
.\scripts\v9xctl.ps1 info -WaitForAgent 60 -Json            # any verb: retry the connection
.\scripts\v9xctl.ps1 exec -Application C:\SETUP\SETUP.EXE -Detach -WaitForWindow "Setup" -Json
.\scripts\v9xctl.ps1 screenshot -Window "Setup" -OutFile .\setup.png -Json
.\scripts\v9xctl.ps1 wait-window -Title "Setup" -Gone -WaitSeconds 300 -Json
.\scripts\v9xctl.ps1 exec -Application C:\JOBS\RUN\BENCH.EXE -Detach -WaitForFile C:\JOBS\RUN\RESULT.TXT -StableMs 1000 -Json
.\scripts\v9xctl.ps1 wait-file -Path C:\JOBS\RUN\LOCK -Gone -WaitSeconds 600 -Json
.\scripts\v9xctl.ps1 windows
```

- Every connection is retried when it is dropped before HELLO (a port forward
  that accepts while the agent is not listening yet), twice, 0.5 and 1 s
  apart. `-WaitForAgent <seconds>` retries any transport failure (refused,
  reset, timed out) for that long, backing off 1, 2, 4, then 5 s. Without it
  a refused connection still fails at once with exit 20. `wait-desktop`
  waits for the agent too, for its `-WaitSeconds`, and so does `ping` when
  given `-WaitSeconds`.
- `wait-file -Path` polls `stat` on one connection (every 250 ms; a relay
  round trip is its own pace) until the file exists, with `-Gone` until it
  does not, and with `-StableMs N` until its size has not changed for N ms.
  Works with every agent. `exec`/`shell` take the same as `-WaitForFile`,
  run after the program returns (immediately with `-Detach`). Exit 47 at the
  `-WaitSeconds` deadline (default 120).
- `windows` lists visible top-level windows; `wait-window -Title <text>`
  waits for one whose title contains the text (case-insensitive, literal) and
  returns its position and size, `-Gone` for it to close. `exec
  -WaitForWindow <text>` does the same after launching. `screenshot -Window
  <text>` captures that window's rectangle only (it becomes `-Region`); add
  `-WaitSeconds` to wait for it first. Agent 0.13.0+; exit 48 when there is
  no such window or the deadline passed.
- Usage: `help <verb>` prints one verb; an unknown verb gets one line (the
  verb list and this copy's version and path), not the full help. Verbs
  agents often guess are accepted: `ls`/`dir` (list), `pull` (get), `push`
  (put), `status` (info), `rm`/`del` (delete), `type`/`tail` (cat),
  `tasklist` (ps), `taskkill` (kill), `wait-ready` (wait-desktop), and
  more. `-Target host:port` sets `-Host` and `-Port` together; `-HostName`
  and `-EndpointPort` are aliases.

Boot and capture examples:

```powershell
.\scripts\v9xctl.ps1 reboot -JobId driver-abc123 -WaitSeconds 180 -Json
.\scripts\v9xctl.ps1 reset -JobId driver-abc124 -WaitSeconds 240 -Json
.\scripts\v9xctl.ps1 shutdown -JobId maintenance-01 -Json
.\scripts\v9xctl.ps1 wait-desktop -WaitSeconds 120 -Json
.\scripts\v9xctl.ps1 screenshot -Destination .\desktop.bmp -Json
.\scripts\v9xctl.ps1 screenshot -Path C:\V9XREMOTE\TEMP\MODE.BMP `
    -Destination .\mode.bmp
```

`BootCounter` increments on every agent start and is not reboot proof alone.
`reboot` succeeds only after disconnect/reconnect when a new HELLO has both a
changed counter and the requested persisted job token. `reset` (agent 0.10.0+,
Windows 9x) is proven the same way; use it only on a guest whose Windows restart
hangs, see [guest-agent.md](guest-agent.md). It exits 23 against an agent that
does not advertise it. `wait-desktop` must
then confirm Explorer readiness before post-reboot work; it polls INFO until the Explorer
desktop (`Progman` or `Shell_TrayWnd`) appears. `screenshot` writes on the guest
and then transactionally downloads and verifies the BMP.

```powershell
.\scripts\v9xctl.ps1 screenshot -OutFile .\dialog.png -Region 200,150,400,300 -Json
.\scripts\v9xctl.ps1 screenshot -OutFile .\overview.png -Scale 0.5 -Gray -Json
```

`-Region x,y,w,h` captures one rectangle (clipped to the screen; one that
starts off screen exits 10), `-Scale 0.01-1` downscales with a box filter, and
`-Gray` stores 8-bit greyscale. The result then also carries `RegionX`,
`RegionY`, `RegionWidth`, `RegionHeight`, `ScalePercent`, `Gray` and
`ShapedBy`; a point in the image maps to the screen as
`RegionX + x * RegionWidth / Width`. Agents 0.13.0+ (capability `0x00020000`)
do the shaping before sending (`ShapedBy: guest`), and v9xctl always asks
them to pack the image losslessly: 8-bit with an exact palette, RLE8 when that
is smaller, whenever the screen has at most 256 colours. `Bytes` is what
crossed the wire. Against older agents v9xctl shapes the full frame itself
(`ShapedBy: host`). The same options work on `input -OutFile`.

Input injection:

```powershell
.\scripts\v9xctl.ps1 input -Sequence "key CTRL+ESC" -Json
.\scripts\v9xctl.ps1 input -Sequence "move 80,486; click left; delay 500; type notepad; key ENTER"
.\scripts\v9xctl.ps1 input -Sequence "move 40,40; down left; move 300,220; up left"
```

`input -Sequence` is a semicolon- or newline-separated list of steps applied in
order as one atomic batch (at most 256 steps):

- `move X,Y` moves the cursor to absolute screen pixels
- `moverel DX,DY` moves the mouse by DX,DY pixels as real mouse motion (agent
  0.8.1+). Use it for games that read motion deltas and recentre the cursor
  (UT99's menus), which never see a `move` jump. The agent sends it in steps
  of at most 4 pixels so Windows' mouse acceleration leaves the distance
  unscaled; the game's own sensitivity still applies
- `click [left|right|middle]`, `doubleclick [button]`, and `down`/`up [button]`
  for press/release (a `down`, one or more `move`s, then `up` is a drag)
- `wheel N` scrolls N notches (negative scrolls toward the user)
- `key NAME` or `key MOD+MOD+KEY` presses a key or hotkey; modifiers are held
  while the final key is pressed, e.g. `CTRL+ESC`, `ALT+F4`, `CTRL+ALT+DELETE`
- `type TEXT` types ASCII text (shifted characters handled; the rest of the
  step is literal, so spaces are preserved)
- `delay MS` waits between steps

The result reports the actions performed and the final cursor position. Input
needs a ready desktop; run `wait-desktop` first after a reboot.

Add `-OutFile` to make one GUI step one call: after the input, `input` waits
`-SettleMs` (default 500, 0-30000) for the guest to repaint, then captures a
screenshot on the same connection exactly as `screenshot -OutFile` does. The
result carries both the screenshot fields and the input fields plus `SettleMs`.
Raise `-SettleMs` for dialogs that open slowly; the screenshot caveats
(no fullscreen transitions, no suspected display wedge) still apply.

```powershell
.\scripts\v9xctl.ps1 input -Sequence "move 80,486; click left" -OutFile .\after.png -Json
```

The guest's VNC server (for a person with a VNC viewer):

```powershell
.\scripts\v9xctl.ps1 vnc status -Json
.\scripts\v9xctl.ps1 vnc start
.\scripts\v9xctl.ps1 vnc stop
```

`vnc` is built from existing verbs, with no protocol change:

- it reads `[vnc]` from `C:\V9XREMOTE\AGENT.INI` with `get`;
- it asks `V9XVNC.EXE -status` with `exec`;
- `start` launches the server detached, and `stop` runs `V9XVNC.EXE -stop`.

Both `start` and `stop` then wait up to 10 s for the change. The result
reports `Installed`, `Enabled`, `Running`, `WasRunning`, `VncPort` and
`PasswordSet` (whether a VNC password is set; the password itself is never
shown). Exit
codes:

- 23: the server is not installed, or `start` was asked of a guest whose
  owner has not enabled it (SETUP.EXE's VNC option);
- 30: it did not start;
- 32: it did not stop.

`run-driver-cycle.ps1` uploads and preflights a driver package. It does not
modify the driver unless both `-Apply` and `-ConfirmAlreadyAssociated` are
present. By default it preflights with `V9X16LD.EXE /quiet`; a future truly
unattended consolidated probe can be selected with `-PreflightProgram` and
`-PreflightArguments`. The applied cycle invokes the package's `V9XFIX.INF`
directly through Windows SetupX, performs a proven reboot, waits for the
desktop, and stores `DESKTOP.BMP` plus `cycle.json` under
`build\driver-results\<job-id>`. It never performs first device association.

## set-autologon.ps1

`set-autologon.ps1` reports and changes Windows 9x autologon on a guest, and
clears a logon dialog that is already on screen. It is a host-side composition
of existing verbs (`exec`, `put`, `get`, `stat`, `shell`, `input`,
`wait-desktop`) with no protocol change, so any agent that supports those verbs
supports it.

```powershell
.\scripts\set-autologon.ps1                       # status only, changes nothing
.\scripts\set-autologon.ps1 -Dismiss              # clear a dialog that is up now
.\scripts\set-autologon.ps1 -Enable  -Json
.\scripts\set-autologon.ps1 -Disable -Host 10.0.1.172
```

| Switch | Effect |
|---|---|
| none | Reports `PrimaryProvider`, user name, `.PWL` path and presence, `DesktopReady`, and an `Autologon` boolean. Works while the guest sits at the logon dialog. |
| `-Enable` | Clears `HKLM\Network\Logon\PrimaryProvider` to Windows Logon if a network provider is set, and completes a pending logon dialog so Windows writes the password list. |
| `-Disable` | Parks `C:\WINDOWS\<user>.PWL` as `C:\WINDOWS\V9XLOGON.PWL` (not deleted, so `-Enable` can restore it). The guest prompts from the next boot. |
| `-Dismiss` | Sends one `ENTER` to a logon dialog that is up and waits for the desktop. Changes no configuration. |

Autologon on Windows 9x needs two conditions together: an empty
`PrimaryProvider`, and a `C:\WINDOWS\<user>.PWL` holding a blank password.
The password list is only written when a logon completes, so it cannot be
fabricated - `-Enable` on a guest with no `.PWL` and no dialog on screen
reports that a reboot plus one `-Dismiss` is still needed. Reasoning and
measurements:
[decisions/2026-09-04-prelogon-reachability-and-autologon.md](decisions/2026-09-04-prelogon-reachability-and-autologon.md).

## capture-emulator-window.ps1

`capture-emulator-window.ps1` captures the emulator's own window to PNG from
the host, with no involvement from the guest.

```powershell
.\scripts\capture-emulator-window.ps1 Win98SE-Native-S3 -OutFile .\screen.png
.\scripts\capture-emulator-window.ps1 -ProcessId 4812 -OutFile .\screen.png -Json
```

The positional argument is matched against the emulator process command line,
so an 86Box VM profile directory name identifies its window; `-ProcessName`
defaults to `86Box`, and `-ProcessId` is the fallback when a name matches more
than one running instance.

Reach for this when `screenshot` cannot help by definition: at a logon dialog,
a BIOS prompt or a shutdown hang, the guest has no ready desktop, so
`screenshot` returns exit 43 for exactly the states worth looking at. It
captures the host's rendering of the window, so the emulator's menu bar,
toolbar and any window scaling are included - it is a diagnostic, not a
substitute for the in-guest capture path.
