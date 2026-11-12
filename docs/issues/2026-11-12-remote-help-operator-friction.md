# Remote help: six things that cost round trips in one Velocity9x session

Status: released in 0.17.0 (2026-11-12); live-tested on Win98 SE and Win2000 in 86Box; physical network-password logon and relay path untested; original item 5 incident unreproduced
Date: 2026-11-12

From one Velocity9x session driving three remote-help machines over the
relay: `M-6B21TJ` (HP t5740, agent 0.15.0), `M-KYHT4Q` (TOMMOHP, an HP
laptop, 0.12.0 hot-updated to 0.16.0 `dev-fault-3`) and the release-check
runner. Each item below cost one or more relay round trips (30-80 s each,
`2026-11-05-relay-silent-guest-during-3d.md`) or a workaround script.
Ordered by how often it bit.

## Implementation (0.17.0 development)

- Implemented items 1-4 and 6, with timeout fixes and a new wait path for
  item 5. Opt-in `-DismissLogon` on `wait-desktop`,
  `reboot` and `reset`, MCP `dismiss_logon`, relay forwarding in
  `set-autologon.ps1`, and positional paths (including aliases).
- The desktop wait is one bounded guest request, not a host watcher.
  Cancellation targets a visible, enabled `#32770` dialog with an enabled
  Cancel button and exactly `Enter Network Password` or `Enter Windows
  Password` as its title, before Explorer is ready. It posts `IDCANCEL`,
  never Enter or credentials. It reports `LogonDismissed` only after the
  targeted dialog disappears. Unknown/localized dialogs are left alone.
  No persistent INI setting was added; dismissal remains explicit per call.
- `set-autologon -Dismiss` now uses that cancellation path, requires a
  0.17.0+ Win9x agent, and performs no registry/PWL work. `-Enable` retains
  its separate, intentional blank-logon behavior.
- Guest package builds as 0.17.0. Host tests cover cancellation, refusal,
  timeout, reboot proof ordering, helper behavior and positional paths.
  Real Win9x boot testing is still required, especially the network logon
  variant on the physical machine. This is not a released/validated fix yet.
- `reg -Set -Name <name> -String|-Dword` and `reg -DeleteValue` return the
  previous value and audit it in `EVENTS.LOG`, including full old bytes for
  binary/long data. Limits are 1024 bytes per new/old value. The registry
  protection policy refuses hive roots, HKLM SYSTEM/SECURITY/SAM/HARDWARE,
  HKCC/HKDD and the agent's startup/update entries. Filesystem path
  protections cannot be copied literally to registry keys.
- `display` reports current and registry modes. `display -Restore` restores
  the saved mode; `kill -RestoreDisplay` verifies process exit first.
- `input -Window <title>` requires one literal substring match and verified
  foreground focus. New replies name the before/after foreground windows
  and the last key-press/typed-character window. A focus loss stops targeted input.
- Item 5 investigation found a definite host deadline bug: relay reconnects
  imposed at least 150 seconds even with less time left, and the relay open
  clock excluded the initial HTTP request. Both are fixed; a slow local
  relay fixture took about 5.1 seconds for a five-second budget including a
  three-second HTTP open. This does not prove the original incident's cause.
  New agents also support bounded waits on real process handles, avoiding
  repeated PROCESS_LIST relay round trips for PID/unique exact-name waits.
  Wildcard/multiple-name waits and older agents keep the original path.
- Verification: guest package build, native simulated registry/display/logon
  lifecycle, combined CLI fixture, MCP regression suite and operator wire
  smoke. The MCP suite has 63 existing tests plus two new focused checks.
  Real guest input/display/registry validation and a rerun of the original
  relay workload are still needed before release.

## 1. Every reset stops at "Enter Network Password"

TOMMOHP shows the Windows network logon box (`#32770` "Enter Network
Password" over an `MPRTransparentDesktop`) on every boot; `M-ZJQ2DX` did the
same in an earlier session ("logon dialog needs ESC every boot").
`wait-desktop` then times out, and so does the release-check runner's
post-reset wait, until someone presses Escape. This session needed a
watcher script that polled `windows` after the runner's `reset:` log line and
sent `input -Sequence 'key ESCAPE'`, and the same in a reset wrapper.

`scripts\set-autologon.ps1 -Dismiss` does the right thing but takes
`-Host`/`-Port` only, so it cannot reach a relay machine, and it is a separate
step. `-Enable` changes the owner's logon provider, which on a lent machine
is not the operator's to change.

Proposal:

- `reset`, `reboot` and `wait-desktop` take `-DismissLogon`: after the agent
  starts (it runs before logon), it cancels a logon box itself, guest-side,
  no host round trip, and reports that it did (`LogonDismissed: true`).
  Cancel, never type: no credentials ever.
- Or an agent INI/remote-help setting, `dismiss_logon=1`, set per machine by
  the operator once, so every boot clears it.
- `set-autologon.ps1` gains `-Relay` like every v9xctl verb.

## 2. `reg` is read-only, so settings go through `regedit /s`

Two changes needed a registry write: Half-Life's renderer
(`HKCU\Software\Valve\HLDemo\Settings` `EngineType` 3, then back to 2) and the
desktop resolution (`HKLM\Config\0001\Display\Settings` `Resolution`). Both
went as a `.REG` file put to the guest and `exec REGEDIT.EXE /s`. One `.REG`
built with `printf` turned `\0001` into an octal NUL, and Win98's regedit
wrote a stray `Resolution` value onto `HKLM\Config`, which REGEDIT4 cannot
delete.

Proposal: `reg -Set -Key -Name -String|-Dword` for one value, logged to the
activity log (`note`) with the old value, and refused under the same roots
`delete` refuses. A `-DeleteValue` for the case above.

## 3. Killing a full-screen DirectX program leaves its display mode

`kill -Name HLDEMO.EXE` on M-6B21TJ ended Half-Life (Direct3D, 640x480,
exclusive) and left the desktop at 640x480; the owner's 1024x768 came back
only with a `reset`. A program that exits normally restores the mode; one
terminated does not.

Proposal: a `display` verb: report the current mode against the registry's,
and `display -Restore` calls `ChangeDisplaySettings(NULL, 0)`. Or
`kill -RestoreDisplay`, which does that after the process is gone.

## 4. Input goes to whichever window has the focus

On M-6B21TJ the NETGEAR wireless utility's window came to the front while
Half-Life ran; the `input -Sequence` that typed Half-Life's console `quit`
went to it instead, and Half-Life kept running. Nothing in the reply said so.

Proposal: `input -Window <title>`: bring that window to the foreground first
and refuse if it cannot; and every `input` reply names the foreground window
it typed into, so a miss is visible in the first reply rather than three
calls later.

## 5. `wait-process` over the relay hung

M-6B21TJ, agent 0.15.0, 2026-11-11: `wait-process` on a running game over the
relay never returned and had to be stopped; `ps` in a loop worked. Not
reproduced or narrowed. Agent 0.16's `wait-fault` may cover the case that
mattered (a fault dialog); the hang itself is unexplained.

## 6. Path verbs reject a positional path

`v9xctl list 'C:\V9XDIAG'` and `cat 'C:\...'` exit 10 ("requires -Path"),
which over the relay is a wasted call. Accept the first positional argument
as `-Path` for `list`, `cat`, `stat`, `get` and `hash`.
