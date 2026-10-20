# Install ID changes on every boot (LF-only AGENT.INI)

Status: fixed in 0.10.0 (VM verified); the Velocity9x netbook still needs cleanup
Date: 2026-10-12

## Symptom

On the Velocity9x netbook (Windows 98 SE, agent 0.9.x) INFO reported a new
`InstallId` after every start and `AGENT.LOG` showed `install-id-created` at
every boot (b168, b169, ...). Its `C:\V9XREMOTE\AGENT.INI` began with six
LF-only lines (`[agent]`, `port`, `bind_address`, `allowed_client`,
`log_level`, `allow_dos_vm=0`) followed by about 50
`<CR>install_id=...<CR>` entries.

## Cause

- **How the LF file got there.** On 2026-09-25 an agent session wrote a
  minimal `AGENT.INI` to its scratchpad with a file-writing tool that emits LF
  and uploaded it with `v9xctl put` to `C:\V9XREMOTE\AGENT.INI` (the netbook
  was then 10.0.1.254). `put` is byte-exact, so it arrived LF-only. SETUP.EXE,
  INSTALL.BAT and the package build all produce CRLF (`.gitattributes` forces
  CRLF for `packaging/**`; `build-guest.ps1` writes package text files with
  `Set-Content`). The file had no `discovery` key, which also rules out
  SETUP.EXE as the writer.
- **Why the ID was lost.** Windows 98 SE *reads* an LF-only INI correctly:
  the netbook's `allow_dos_vm=0` was in effect, and an LF-only file that
  already holds an `install_id` keeps it across reboots (verified on the VM).
  The fault is in the profile *writer*: adding a key to an LF-only section,
  it writes `<CR>install_id=...<CR>`. The reader splits lines on LF, sees the
  key as `\rinstall_id`, and never matches it, so the next start generates and
  appends another.

Reproduced on Win98SE-Native-S3 (port 9870) with agent 0.10.0 dev-0100a: an
LF-only file without `install_id` gave a new ID on boots 29 and 30 and the
same `<CR>`-wrapped lines as the netbook.

## Fix (0.10.0)

- `v9x_ini_repair` (`src/common/installid.c`) rewrites an INI image with CRLF
  line ends, collapsed blank-line runs, and only the first `install_id` per
  section. `v9x_ini_normalize` (`src/guest/installid.c`) applies it to
  `AGENT.INI` before the agent or SETUP.EXE reads it, saves the original as
  `AGENT.BAK` and logs `config-repaired`. A clean file is not rewritten.
  Native tests in `tests/host/test_protocol.c` include the netbook's byte
  pattern.
- `v9xctl put` and MCP `v9x_put_file` warn when a Windows text file
  (`.INI .BAT .CMD .REG .CFG .INF`) goes over LF-only.
- Verified on the VM: after `update` to dev-0100b the damaged file became CRLF
  with the first ID (`config-repaired`, boot 31), which survived a reboot; an
  LF-only file without an ID got exactly one ID (boot 33) that boot 34 kept.

## Cleanup for an affected machine

Read-only check first:

```powershell
.\scripts\v9xctl.ps1 get -Host <ip> -Source C:\V9XREMOTE\AGENT.INI -Destination .\AGENT.INI.before
```

**Preferred: upgrade the agent to 0.10.0.**
`.\scripts\v9xctl.ps1 update -Host <ip> -Source build\install` hot-swaps the
agent without a reboot; the new agent repairs the file as it starts and keeps
the first (oldest) ID. Then confirm with `info` (InstallId), `get` of
`AGENT.INI` (one `install_id`, CRLF) and `AGENT.LOG` (`config-repaired`).
Delete `C:\V9XREMOTE\AGENT.BAK` once satisfied (it is only a backup).

**Without upgrading** (agent stays 0.9.x): fix the file on the host and put
it back, then restart the agent.

```powershell
$text = [IO.File]::ReadAllText('.\AGENT.INI.before')
$lines = $text -split "`r`n|`n|`r" | Where-Object { $_ -ne '' }
$id = ($lines | Where-Object { $_ -match '^install_id=' } | Select-Object -First 1)
$keep = @($lines | Where-Object { $_ -notmatch '^install_id=' }) + $id
[IO.File]::WriteAllText('.\AGENT.INI.fixed', (($keep -join "`r`n") + "`r`n"))
.\scripts\v9xctl.ps1 put -Host <ip> -Source .\AGENT.INI.fixed -Destination C:\V9XREMOTE\AGENT.INI
```

`put` must not print the LF warning. The ID only stops changing at the next
agent start (a hot `update`, or a reboot). The first ID matches what the
automatic repair would pick; any one of them works, since none was ever
stable.

## Velocity9x netbook (10.0.1.248:9869)

Not done: writing to or restarting the netbook waits for the owner's go-ahead. As
read on 2026-10-12 its file holds the six LF lines plus 50-odd IDs; the first
is `01c5ae56-7f04-4c2c-996d-c7bad032abd7`. A hot `update` to 0.10.0 needs no
reboot, which suits a machine whose Windows restart hangs.
