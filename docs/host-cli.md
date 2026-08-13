# Host client

`scripts\v9xctl.ps1` supports control, execution, and file operations, defaults to
`127.0.0.1:9869`, and performs a new handshake for each invocation.

Use `-Json` for compact machine-readable output. Use `-Host`, `-Port`, and
`-ConnectTimeoutSeconds` to override connection settings.

For a physical target, pass its LAN address directly:

```powershell
.\scripts\v9xctl.ps1 info -Host 192.168.10.98 -Port 9869 -Json
```

See [physical-machine.md](physical-machine.md) for guest configuration and
containment requirements.

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

Uploads and downloads are limited to 64 MiB per file. `put` commits only after
the guest verifies size and CRC32. `get` writes a uniquely named local `.part`
and replaces the requested destination only after matching the guest's final
size and CRC32.

Boot and capture examples:

```powershell
.\scripts\v9xctl.ps1 reboot -JobId driver-abc123 -WaitSeconds 180 -Json
.\scripts\v9xctl.ps1 shutdown -JobId maintenance-01 -Json
.\scripts\v9xctl.ps1 wait-desktop -WaitSeconds 120 -Json
.\scripts\v9xctl.ps1 screenshot -Destination .\desktop.bmp -Json
.\scripts\v9xctl.ps1 screenshot -Path C:\V9XREMOTE\TEMP\MODE.BMP `
    -Destination .\mode.bmp
```

`reboot` succeeds only when a new HELLO has both a changed boot counter and the
requested persisted job token. `wait-desktop` polls INFO until the Explorer
desktop (`Progman` or `Shell_TrayWnd`) appears. `screenshot` writes on the guest
and then transactionally downloads and verifies the BMP.

Input injection:

```powershell
.\scripts\v9xctl.ps1 input -Sequence "key CTRL+ESC" -Json
.\scripts\v9xctl.ps1 input -Sequence "move 80,486; click left; delay 500; type notepad; key ENTER"
.\scripts\v9xctl.ps1 input -Sequence "move 40,40; down left; move 300,220; up left"
```

`input -Sequence` is a semicolon- or newline-separated list of steps applied in
order as one atomic batch (at most 256 steps):

- `move X,Y` moves the cursor to absolute screen pixels
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

`run-driver-cycle.ps1` uploads and preflights a driver package. It does not
modify the driver unless both `-Apply` and `-ConfirmAlreadyAssociated` are
present. By default it preflights with `V9X16LD.EXE /quiet`; a future truly
unattended consolidated probe can be selected with `-PreflightProgram` and
`-PreflightArguments`. The applied cycle invokes the package's `V9XFIX.INF`
directly through Windows SetupX, performs a proven reboot, waits for the
desktop, and stores `DESKTOP.BMP` plus `cycle.json` under
`build\driver-results\<job-id>`. It never performs first device association.
