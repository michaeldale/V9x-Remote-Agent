# AI development workflows

How an AI coding agent develops and debugs Windows 9x software with this
project. The short version lives in [AGENTS.md](../AGENTS.md); this document
walks the loop with real outputs and explains why each guarantee matters.

## The loop

```text
edit source on the host
      |
      v
build (Open Watcom or any cross-toolchain targeting Win32/Win16)
      |
      v
put / push-tree ................ CRC32-verified upload
      |
      v
exec / shell ................... bounded run, streamed stdout/stderr, JSON
      |
      v
get / screenshot ............... collect artifacts, look at the screen
      |
      v
reboot + wait-desktop .......... only when the change needs it, with proof
      |
      v
interpret JSON, fix code, repeat
```

Every step is one process invocation with a documented exit code, so an agent
can script the whole loop without prompts, screen scraping, or sleeps.

## An annotated session

Deploy and run a freshly built test program:

```powershell
.\scripts\v9xctl.ps1 put -Source .\build\SMOKE.EXE -Destination C:\V9XREMOTE\JOBS\smoke\SMOKE.EXE -Json
```

```json
{"Success":true,"Bytes":6144,"Crc32":"8C3A11F2","Destination":"C:\\V9XREMOTE\\JOBS\\smoke\\SMOKE.EXE"}
```

The CRC32 is computed independently on both sides; a corrupt transfer cannot
silently land. Now run it:

```powershell
.\scripts\v9xctl.ps1 exec -Application C:\V9XREMOTE\JOBS\smoke\SMOKE.EXE `
    -Arguments "/auto" -TimeoutSeconds 60 -ExpectedExitCode 0 -Json
```

```json
{"Success":true,"Mode":"Direct","ExitCode":0,"ElapsedMilliseconds":91,
 "Stdout":"","Stderr":"","TimedOut":false,"GuiWindow":true,"CaptureMode":"pipes"}
```

Field meanings that matter to an agent:

- `ExitCode` is the child's real exit code; `-ExpectedExitCode 0` makes the
  CLI itself exit 31 on mismatch, so a wrapper only needs `$LASTEXITCODE`.
- `TimedOut: true` (CLI exit 32) means the child was terminated at the
  deadline; the partial stdout/stderr captured up to that point is included.
- `GuiWindow: true` says the target was a GUI-subsystem EXE, so the agent let
  it manage its own window instead of forcing one (a hidden GUI window never
  receives `WM_PAINT` on Windows 9x, which deadlocks paint-driven programs).
- `StdoutTruncated`/`StderrTruncated` flag output that exceeded the byte
  limits; totals are still reported.

When output alone is not enough, look at the screen:

```powershell
.\scripts\v9xctl.ps1 screenshot -Destination .\after.bmp -Json
```

The BMP is CRC-verified like any download. Via the MCP server the same
operation returns a PNG image block the model can see directly.

## Proven reboots

Driver and system-file changes need a reboot, and "the VM seems back" is not
evidence. The reboot flow persists a caller-chosen token inside the guest
before the reboot is acknowledged:

```powershell
.\scripts\v9xctl.ps1 reboot -JobId driver-fix-7 -WaitSeconds 180 -Json
```

```json
{"Success":true,"JobId":"driver-fix-7","PreviousBootCounter":65,"BootCounter":66,
 "PendingJob":"driver-fix-7","Reconnected":true,"DesktopReady":true}
```

Success requires all three: a fresh connection, a strictly increased boot
counter, and the exact token echoed back from disk. A hung guest, a crashed
boot, or a VM that never went down all fail this check (CLI exit 42). After
any reboot, call `wait-desktop` before GUI work; services start before the
shell finishes loading.

## Debugging patterns

- **Program under test writes an INI/log**: have it write results to its job
  directory, `get` the file, parse on the host. Survives crashes better than
  stdout.
- **Visual regressions**: screenshot before and after, compare on the host.
  The desktop is 800x600 or whatever the guest mode is; `info` reports the
  current mode.
- **Hang diagnosis**: on exec timeout, take a screenshot (is there an error
  dialog?), then `get C:\V9XREMOTE\AGENT.LOG` for the agent's view.
- **Crash recovery**: if the guest wedges hard, a VM restart is fine; the
  agent starts with Windows, `BOOT.DAT` keeps the boot counter monotonic, and
  interrupted transfers leave only `.PART`/`.V9X.BAK` files, never a corrupt
  destination.
- **State hygiene**: give every task its own `C:\V9XREMOTE\JOBS\<id>`
  directory so runs stay comparable and cleanup is one recursive delete.

## Determinism guarantees, summarized

| Guarantee | Mechanism |
|---|---|
| Transfers are intact | CRC32 verified end-to-end, transactional commit with rollback backup |
| Runs are bounded | Guest-side timeout terminates the child; output byte limits with truncation flags |
| Exit status is real | Child exit code returned verbatim; CLI maps outcomes to documented exit codes |
| Reboots are proven | Persisted token + monotonic boot counter, checked on reconnect |
| The screen is inspectable | BMP capture of the real framebuffer contents via GDI |
| One command, one result | Single-client, single-execution agent; no queuing surprises |
