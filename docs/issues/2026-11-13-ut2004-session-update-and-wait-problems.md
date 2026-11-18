# One Velocity9x game-test session: a hot update that left no agent, and four smaller problems

Status: addressed in 0.18.0; original item 1 incident not reproduced
Date: 2026-11-13

## Resolution (0.18.0)

- Item 1: the staged swapper retains `V9XAGNT.OLD`, monitors its child until
  listener readiness or a 60-second deadline, and restores/relaunches the
  previous agent after copy/start/early-exit/timeout failure. The previous
  screenshot helper stays in place until readiness. CLI/MCP check the
  installed agent's SHA-256 after reconnect when supported, rejecting a
  rollback rather than treating a new boot counter as success. This
  hardening does not prove the original incident's cause. Listener readiness
  does not prove LAN/relay reachability; rollback can still fail if process
  termination or disk operations fail. `HOTSWAP.LOG` records these paths.
- Item 2: backoff saturates before computing the exponential. Regression
  test simulates 40 immediate refusals without wall-clock sleeps.
- Item 3: `wait-fault -DialogTitle 'Critical Error'` (also on
  `exec -WaitForExit`) returns `Outcome = Dialog`, exit 50, text in `Fault`,
  and honors `-Dismiss`. Filters are opt-in, literal, case-insensitive and
  restricted to the watched PID(s); ordinary dialogs remain ignored by
  default. MCP calls the array `dialog_titles`. Non-fault dialog controls
  are re-read so a changing message under the same HWND/title is detected.
- Item 4: the guest redacts password Edit/RichEdit controls before requesting
  their text. This protects both window lists/text and fault reports. Kept
  all-window enumeration for compatibility; `-Title` is recommended.
- Item 5: `windows -Controls` and fault text use the PowerShell success
  pipeline. `host-cli.md` documents JSON, formatted and raw console output.
- Observations: `host-cli.md` now documents the active download's
  `<destination>.<slot>.DLPART` file, and explains that `PendingJob` is the
  last persisted reboot/resume token, not a current execution or transfer.

Validation: native failure-injection coverage for rollback and password
redaction; CLI protocol fixtures for title filtering/dismissal and pipeline
capture; MCP regression tests; guest build/import audit. Physical-machine
and live Win9x/NT validation remain to be done.

The original session report follows.

From one Velocity9x session installing and running the Unreal Tournament
2004 demo on a physical Windows 98 SE machine (W9QI5A83, a Dell i5-4570,
agent reached directly at its LAN address over a USB wifi key, no relay).
Host: `v9xctl` 0.17.0 from this checkout at `1314da5`. Guest agent at the
start: 0.16.0, build `dev-fault-3`.

Ordered by cost.

## 1. Hot update 0.16.0 `dev-fault-3` to 0.17.0 left the machine with no agent

`v9xctl update -Source build\install -WaitSeconds 90` at boot 231, uptime
about 70 minutes, desktop idle (one earlier program had been closed):

```
Protocol error: Agent did not relaunch with a new boot counter within 90
seconds. Last error: ... actively refused it 10.0.1.254:9869
exit=42
```

The port stayed refused for the next 20 minutes or more, while the
machine answered ICMP throughout. The owner restarted it by hand; it came
back at boot 234 as `0.17.0` / `v0.17.0`, so the new binary was in place
and starts at boot. What failed is the relaunch, not the file
replacement.

Not known: whether the old agent exited and the new one failed to start,
or started and failed to listen; `C:\V9XREMOTE` logs from boot 231 were
not read before the restart (and may not have survived it). Whether
`dev-fault-3` differs from released 0.16.0 in the update path is not
known either. The same build hot-updated on `M-KYHT4Q` on 2026-11-12
(0.12.0 to `dev-fault-3`) without this.

Cost: the machine was unreachable until a person went to it.

Wanted: the cause; and an update that cannot end with no agent, e.g. the
old agent keeps listening until the new one has answered, or a watchdog
relaunches the previous binary.

## 2. `-WaitForAgent` throws an Int32 conversion error after 32 attempts

`v9xctl info -ConnectTimeoutSeconds 20 -WaitForAgent 240` against the
refused port, exit 22 after about two and a half minutes:

```
Protocol error: Cannot convert argument "val2", with value: "2147483648",
for "Min" to type "System.Int32"
```

`Connect-V9xSessionWithRetry` in `scripts\v9xctl.ps1` computes
`[Math]::Min(5, [Math]::Pow(2, $attempt - 1))`. At attempt 32 the power
is 2^31, which PowerShell cannot bind to the `Min(Int32, Int32)` overload
it picked. Refusals return at once, so 32 attempts fit inside a 240 s
wait; a timing-out port would not get that far. Read from the source,
with the one failure above as the evidence.

Cost: a wait meant to span a slow boot (this machine takes over five
minutes) dies early with a message that reads as a protocol fault. The
session fell back to a host-side retry loop.

## 3. `wait-fault` does not see a program's own error dialog

Unreal reports a failure in its own dialog: class `#32770`, title
`Critical Error`, owned by the game's pid, with the message in an Edit
control. The process stays alive behind it. `wait-fault -Name UT2004.EXE
-WaitSeconds 400` sat out all 400 s and returned exit 49, "still running
... no fault dialog", for a failure that happened 7 s after launch.

The session's workaround: probe in a loop, `wait-fault -Pid <pid>
-WaitSeconds 6`, then `windows -Title 'Critical Error' -Json`, then
`windows -Controls` and `close-window` when it is there.

Wanted: one guest-side wait that also returns when the watched process
shows a dialog, e.g. `wait-fault -DialogTitle <text>` (repeatable), or a
general rule: a new visible `#32770` window owned by the watched pid
ends the wait with `Outcome = Dialog` and its control text. `exec
-WaitForExit` would take the same option.

## 4. `windows -Controls` with no `-Title` prints a password field

Run to find an installer's buttons, it dumped every control of every
top-level window, including the wifi utility's passphrase Edit control in
clear text. The value went into the session transcript.

Wanted: skip the text of Edit controls with `ES_PASSWORD`, and consider
requiring `-Title` (or an explicit `-All`) with `-Controls`.

## 5. Text-mode output of `windows -Controls` is not on the pipeline

`$text = & v9xctl.ps1 windows -Title 'Critical Error' -Controls |
Out-String` gave an empty string while the dialog text appeared on the
console. A script that tested `$text -match 'Critical Error'` therefore
never saw the dialog and ran to its deadline. `-Json` works and is what
scripts should use; the text form going to the host only is the trap.
`docs\host-cli.md` does not say which verbs write objects and which
write host text.

## Smaller observations

- `download` of a 296 MB file over this machine's wifi took about 20
  minutes (0.27 MB/s) in one blocking call with no progress. The
  destination file did not exist under its name until the end, so `stat`
  could not show progress; free space from `inventory` could. A
  `Bytes so far` in `info`'s pending job, or a temporary name documented
  in `host-cli.md`, would do.
- The call did return correctly after 20 minutes with
  `-ConnectTimeoutSeconds 120`, with the size and CRC32, and `hash
  -Source` matched the local file.
- `info` reported `PendingJob = m4-20261113-183121-425e` throughout, on
  both agent versions and across the restart, with no job of this
  session by that name. Not looked into.
