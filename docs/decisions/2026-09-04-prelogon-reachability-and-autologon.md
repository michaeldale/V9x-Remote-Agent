# The agent works before logon; the shell is what stalls

Date: 2026-09-04
Status: accepted

## Context

Two related beliefs were in play. The first was that the agent "does not work
before the Windows network login prompt". The second was the observed symptom
on physical hardware: a session driving 10.0.1.172 sat in
`wait-desktop -TimeoutSeconds 300` on boot 23 and never returned, described as
"stuck on login".

Those two statements cannot both be about the same thing, because
`wait-desktop` is itself a protocol call. A session that is waiting inside
`wait-desktop` has already connected, completed HELLO and had its request
accepted, which means the agent was running and reachable the whole time. What
had not happened was the logon.

## Options

The three candidate mechanisms for "nothing works before logon", and what would
distinguish them:

1. `RunServices` never launches the executable pre-logon, so no agent exists.
   Distinguished by an absent `agent-start` line for that boot.
2. The process starts but Winsock is unusable until logon completes.
   Distinguished by `agent-start` followed by `winsock-start-failed`.
3. The listener binds fine but the guest has no IP yet (DHCP deferred).
   Distinguished by `listening` present, yet host connects refused or timed
   out.

## Decision

None of the three. Measured on a disposable clone of WIN98-S3NATIVE (86Box,
ym430tx, 98 SE, agent 0.6.1), with `C:\WINDOWS\USER.PWL` renamed away so the
`Enter Windows Password` dialog appears at every boot:

| Probe, taken while the logon dialog was on screen | Result |
|---|---|
| `ping` | success at t+6s and t+13s after the reboot, over two runs |
| `info` | success, reporting `DesktopReady: false` |
| `shell -Command "VER"` | success, `Windows 98 [Version 4.10.2222]` |
| `exec` REGEDIT `/E` + `get` the export | success |
| `stat` | success |
| `input -Sequence "key ENTER"` | success, **cleared the dialog** |
| `screenshot` | exit 43, guest error 6 "desktop not ready for screenshot" |
| `wait-desktop` | exit 44, timeout |

The dialog on screen was confirmed independently of the guest by capturing the
86Box window from the host with `PrintWindow` (`PW_RENDERFULLCONTENT`), since
the in-guest screenshot path is exactly what does not work in that state.

So: `RunServices` does launch the agent before the logon dialog, Winsock is up,
DHCP has completed and the listener accepts. Every verb that does not need the
shell works. The two that need Explorer - `screenshot` and `wait-desktop` -
fail by design, and they fail for as long as nobody logs on. That is the real
defect surface: an unattended guest that prompts stalls every automation run at
its first `wait-desktop`, which looks like "the agent is dead" and is not.

The `input` result is the useful one: the logon dialog owns the focus at that
point in boot and `OK` is its default button, so a single injected `ENTER`
completes it with a blank password and the desktop comes up.

## Autologon on Windows 9x

Autologon is two conditions, not one switch, which is why it is easy to set up
incorrectly:

1. `HKLM\Network\Logon\PrimaryProvider` must be **empty** - "Windows Logon".
   Any network provider here (Client for Microsoft Networks) prompts on every
   boot regardless of the password.
2. `C:\WINDOWS\<user>.PWL` must exist and hold a blank password, where `<user>`
   is `HKLM\Network\Logon\username` truncated to 8 characters.

Condition 2 cannot be created from outside Windows: the password list is only
written when a logon actually completes. This is the trap - setting the
registry alone leaves a machine that still prompts, and deleting the `.PWL` to
"reset" it makes a previously silent machine start prompting. The way through
is to complete the dialog once, which is precisely what injected input can do.

`scripts\set-autologon.ps1` implements this, composing existing verbs with no
protocol change:

- no switch: reports `PrimaryProvider`, username, `.PWL` presence,
  `DesktopReady` and a single `Autologon` boolean. Works at the dialog.
- `-Enable`: clears `PrimaryProvider` if a provider is set, and if a dialog is
  up, completes it so the `.PWL` is created.
- `-Disable`: parks the `.PWL` as `C:\WINDOWS\V9XLOGON.PWL` rather than
  deleting it, so `-Enable` can restore it.
- `-Dismiss`: clears a dialog that is up right now and changes no
  configuration. This is the one to reach for when a run is stuck.

Round trip verified on the clone across boots 16 to 19: `-Disable`, reboot,
status read at the dialog, `-Dismiss`, then a provider set to
`Microsoft Networking` plus `-Disable`, reboot, `-Enable` (which fixed both the
provider and the pending dialog), and a final
`reboot -JobId autologon-final -WaitSeconds 240` that reconnected with proof
and reached a ready desktop with no input at all.

## Consequences

- Treat `wait-desktop` exit 44 with a live `ping` as "nobody has logged on",
  not as a dead agent. `info` distinguishes them: it answers, with
  `DesktopReady: false`.
- A guest intended for unattended automation should have autologon on. Physical
  deployments are the ones that tend not to.
- Not reproduced: the `Enter Network Password` variant. This image has no
  Client for Microsoft Networks installed and no Windows 98 CABs on disk, so
  setting `PrimaryProvider` to `Microsoft Networking` changed nothing and the
  Windows dialog kept appearing. The MSNP32 dialog may behave differently, and
  nothing here rules that out - it is the one case still owed a test, ideally
  on the physical machine that shows it.
- Windows 98 hung on its own `Windows is shutting down` splash once during
  these runs, which failed a `reboot` proof for reasons that had nothing to do
  with logon. A hung 9x shutdown and a stalled logon look identical from the
  host: both are "the guest never came back". Capturing the emulator window, or
  a monitor on a physical machine, tells them apart in one look.
