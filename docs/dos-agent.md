# DOS agent

A cut-down build of the guest agent for real-mode DOS boxes (MS-DOS, PC-DOS,
FreeDOS), compiled for 32-bit protected mode with the DOS/4GW extender and using
[Watt-32](https://www.watt-32.net/) for TCP/IP. It speaks the **identical wire
protocol** as the Windows 9x agent, so the existing host tooling
(`scripts\v9xctl.ps1` and the MCP server) drives a DOS box unchanged.

Source lives in `src\guest-dos\`; it reuses the shared protocol headers
(`include\v9xremote\`) and portable code (`src\common\`) with the Win9x agent, so
the protocol has a single source of truth.

## Supported operations

| Area | Messages | Notes |
|------|----------|-------|
| Handshake / health | HELLO, PING, INFO | INFO reports the DOS version and the reduced capability mask. |
| Execution | EXEC (direct + shell) | Runs to completion, then returns stdout. |
| File transfer | STAT, LIST, MKDIR, upload, download-read | CRC32-verified, transactional upload. |

Dropped versus the Win9x agent (DOS has no equivalent, capability bits cleared):
live exec streaming, cancel, timeout, stderr capture, detach; screenshot, input
injection, power control, hot-update, HTTP download; concurrent connections.

### Execution differences

DOS is single-tasking with no `CreateProcess`/pipes/threads, so a command runs
synchronously with stdout redirected to `C:\V9XREMOT\TEMP\OUT.TMP`, and the agent
streams that file back as `EXEC_STDOUT` frames only after the command exits. The
exit code is the DOS ERRORLEVEL that `system()` returns. Consequences:

- Output arrives in one batch at the end, not live.
- No timeout and no cancel - the agent is blocked while the child runs.
- stderr is not captured: COMMAND.COM redirects handle 1 (stdout) only.
- "Bad command or file name" is printed by COMMAND.COM to stdout and may leave
  the exit code at 0, since DOS does not report a spawn failure the way Win32
  does.

### Paths and filenames

All state is under `C:\V9XREMOT` (the Win9x `C:\V9XREMOTE` is 9 characters and
illegal on FAT 8.3). Uploads stage as `V9XUP.TMP` and back up as `V9XUP.BAK` in
the destination's own directory (the Win9x `.PART` / `.V9X.BAK` suffixes are not
8.3-legal), so the final rename stays on one volume.

## Build

Requires Open Watcom (`WATCOM` env or `C:\WATCOM`) and Watt-32 (`WATT_ROOT` or
`WATT32` env, or vendored at `third_party\watt32`). Watt-32 must be built with
its BSD-socket API and provide a 32-bit flat Watcom import library
(`lib\wattcpwf.lib`).

```powershell
.\scripts\build-guest-dos.ps1 -BuildId dev-dos-001
```

This compiles `src\common\*` + `src\guest-dos\*` with `wcc386 -bt=dos -mf`, links
with `wlink system dos4g` against Watt-32, and assembles `build\dos-install\`
with `V9XAGNT.EXE`, `DOS4GW.EXE`, `AGENT.INI`, `WATTCP.CFG`, `INSTALL.BAT`,
`README.TXT` and `SHA256.TXT`. There is no PE import audit (that check in
`build-guest.ps1` is Win32-specific).

## Deploy

1. Copy `build\dos-install\` into the DOS box (floppy image, shared folder, or an
   ISO you build from it) and run `INSTALL.BAT`.
2. Edit `C:\V9XREMOT\WATTCP.CFG` for the machine's IP (static or `my_ip = dhcp`).
3. Ensure a packet driver for the NIC loads in `AUTOEXEC.BAT` (e.g.
   `NE2000.COM 0x60 0x0A 0x300` for an 86Box NE2000), then add:

   ```
   SET WATTCP.CFG=C:\V9XREMOT
   C:\V9XREMOT\V9XAGNT.EXE
   ```

   The agent runs in the foreground and never returns, so it is the last line of
   `AUTOEXEC.BAT`; the box becomes a dedicated agent.

## Ready-made 86Box test VM

A pre-built 86Box VM lives in the local 86Box VMs folder as `FreeDOS-Agent` (its own
`README.txt` has the full details). It is FreeDOS 1.3 pre-installed on a
`ym430tx` Pentium-MMX 200 with an `ne2kpnp` (Realtek RTL8019AS) NIC on SLiRP,
forwarding host TCP 9869 into the guest. The agent, `NE2000.COM`, `RSET8019.EXE`,
and mTCP diagnostics ride on `AGENT.ISO` (mounted as the guest's D: drive).

Launch it from 86Box Manager (it shows up as "FreeDOS-Agent"). First boot has two
one-time prompts that need a real keypress/decision at the window: a "CMOS
checksum error - press F1" (normal for a new VM) and a Windows Firewall prompt
(decline is fine - loopback is never firewalled, so `127.0.0.1:9869` works
regardless; only allow it to reach the guest from other LAN machines). Then at
the FreeDOS `C:\>` prompt run `D:` then `GO`, which installs the agent to
`C:\V9XREMOT`, loads the packet driver, and starts it on TCP 9869.

To rebuild the agent CD (`AGENT.ISO`), stage the `build\dos-install` package plus
`NE2000.COM`/`RSET8019.EXE`/mTCP tools and a `GO.BAT`, then generate the ISO with
Windows' built-in IMAPI2 (no external tools needed).

## Security

Protocol v1 is unauthenticated - anyone who can reach the port controls the box.
Keep the NIC on an isolated network (an 86Box SLiRP/NAT is ideal); never bridge
it onto an untrusted LAN or the internet. `allowed_client` in `AGENT.INI`
optionally restricts access to one source IP.

## Verify

The wire protocol is unchanged, so the fixture host tests
(`tests\host\Test-Protocol.ps1`) still cover the shared framing. End-to-end
against a live DOS VM (86Box with an NE2000 + packet driver, SLiRP/NAT):

```powershell
.\scripts\v9xctl.ps1 ping   -Host <ip>
.\scripts\v9xctl.ps1 info   -Host <ip>
.\scripts\v9xctl.ps1 exec   -Host <ip> --shell "dir C:\"
.\scripts\v9xctl.ps1 put    -Host <ip> local.bin C:\V9XREMOT\TEMP\up.bin
.\scripts\v9xctl.ps1 get    -Host <ip> C:\V9XREMOT\TEMP\up.bin back.bin
.\scripts\v9xctl.ps1 ls     -Host <ip> C:\
.\scripts\v9xctl.ps1 mkdir  -Host <ip> C:\V9XREMOT\TEMP\sub
```

(Check `scripts\v9xctl.ps1` / `AGENTS.md` for the exact verb and flag spellings.)
Confirm the put/get round-trip reports a matching CRC and leaves no stray
`V9XUP.TMP` / `V9XUP.BAK`.
