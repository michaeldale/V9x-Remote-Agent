# Relay: a guest running full-screen 3D stops checking in, and a wait cannot tell that from a hang

Status: open; items 1-4 done; measured 2026-11-06 on a VM with software D3D: the agent stays in touch but the relay slows to 30-80 s per call; hardware-3D (M-ZJQ2DX's HD 4000) not yet measured
Date: 2026-11-05

## Symptom

`M-ZJQ2DX` (Windows 98 SE, agent 0.14.0 `dev-0140`, Intel HD 4000 on the
Velocity9x driver). 3DMark 2000's default benchmark was started at about
11:42Z and a background wait was left on it:

```powershell
.\scripts\v9xctl.ps1 wait-window -Title 'Overall Score' -WaitSeconds 1800 -Relay M-ZJQ2DX
```

- The wait printed once and then nothing:
  `[connection lost (Exception calling "Read" with "3" argument(s): "Timed out waiting for the agent through the relay."); reconnecting]`
- `GET machines` showed `last_seen` 11:42:28Z, frozen; `online=false`;
  `busy=true` with `last_command_age=0`; and the session `pending`, with a new
  session id after the reconnect.
- Ten minutes on, nothing distinguished "3DMark is still running and the agent
  is starved" from "the guest has hung".

The Velocity9x Ironlake machine (`M-J0F0SJ`) showed the same thing with
3DMark 2000 on 2026-10-27: "relay `get` times out while 3DMark runs; fetch
after it ends". There the guest came back each time.

## What is known

- `V9xRelayStream.Read` throws "Timed out waiting for the agent through the
  relay" when nothing arrives within the stream's read timeout
  (`scripts/V9xRelayStream.cs`). `v9xctl`'s wait loop catches it and reopens a
  session for the time left (`Invoke-V9xWaitLoop`, `[connection lost ...;
  reconnecting]`). That is correct behaviour for a dropped link.
- A reopened session stays `pending` until the agent's next poll accepts it.
  If the agent is not polling, the wait spends the rest of its deadline there,
  silently.
- `busy` in `GET machines` is the agent's own value from its last poll
  (`v9x_remote_poll`: a command in the last 60 s). The server shows it
  unchanged however old that poll is, so it read `true` with the machine ten
  minutes offline.

## Not known

- **Whether the agent stops polling, or polls and cannot be heard.** The
  agent polls from its own thread (`v9x_remote_thread`, `src/guest/remote.c`)
  over its own Winsock HTTP client. A full-screen
  exclusive DirectDraw application on Windows 98 can hold the Win16Mutex for
  long stretches; whether that stalls the agent's thread has not been
  measured. `AGENT.LOG` after a run that recovers would say when the last
  poll before and the first poll after happened.
- Whether this is specific to 3DMark 2000's exclusive mode, or any
  full-screen Direct3D title.

## What would help

1. **Say what is happening.** While reconnecting, print once a minute what
   the server reports: `last_seen` age and session state. "Agent silent for
   600 s; session pending" is the fact the operator needs.
2. **Age the poll fields on the server.** Past one missed poll interval,
   show `busy` as unknown rather than the last poll's value.
3. **Measure the starvation** on a VM: run 3DMark 2000 under a relay session
   with `log_level` high enough to log each poll, and read `AGENT.LOG`
   afterwards. If polls stop, consider raising the poll thread's priority or
   moving the poll off anything the Win16Mutex can block.
4. **Optionally, a guest-side heartbeat file** the agent writes each poll, so
   a recovered guest can show how long it was deaf.

## Progress (0.15.0, 2026-11-06)

- **1, done (host side).** While a relay session it opened stays
  `pending`, `V9xRelayStream` prints once a minute on stderr what `GET
  machines` reports, e.g. `[relay: M-ZJQ2DX session pending 600 s; agent
  last checked in 612 s ago (offline; still waiting)]`. That covers every
  v9xctl verb, reconnecting waits included. MCP calls do not print it yet.
- **2, done (2026-11-06, server, not yet deployed).** `GET machines` and
  `remote_list_machines` report `busy: null` once the machine is offline
  (`last_seen` older than two poll intervals plus 15 s), and the Machines tab
  hides the busy pill then.
- **3, instrumented, not yet measured.** The agent now logs
  `remote-poll-gap <elapsed>s expected <due>s` in `AGENT.LOG` whenever a
  successful check-in comes more than 30 s after it was due (15 s during a
  session, the server's `next_poll` otherwise). Run 3DMark 2000 under a
  relay session on a 0.15.0 guest and read `AGENT.LOG` afterwards: one gap
  the length of the run means the poll thread was starved; normal polls
  through the run mean the replies were lost elsewhere.
- **4, done.** Past 120 s the same gap also goes into the tester's
  `EVENTS.LOG`: "Remote help: back in touch with the server after N s
  without contact".

## Measurement (2026-11-06, item 3)

Setup: 86Box `Win98SE-Fast-D3D` (Celeron Mendocino, Voodoo3 3500 card, but
the Velocity9x VBE-generic display driver, so 3DMark's D3D is **software
rendering**, full-screen exclusive, 800x600x16), agent 0.15.0 `dev-0150b`,
local copy of the update server. 3DMark 99 Max, default benchmark, from
15:21:56 for 13 minutes (stopped early: enough data). A probe made a relay
`info` (new session each time), a LAN `ping` and read `last_seen` every 15 s.

| Phase | Server `last_seen` | relay `info` | LAN `ping` |
|---|---|---|---|
| Idle, before | 0 s | 3.7 s | 0.6 s |
| Game tests (to ~15:28) | 0-4 s | 7-17 s | 0.9-2.9 s |
| Later tests (15:29-15:35) | 0-6 s | 35-81 s | 3.5-9.7 s |

- **The agent never stopped checking in**: `last_seen` stayed under 7 s and
  `AGENT.LOG` has no `remote-poll-gap` line. The poll thread is not starved,
  on this setup.
- **Everything else got slow**: after 15:29 a relay session took 20-50 s to
  start (`relay-session-open` in `AGENT.LOG`) and ~30 s to finish a one-frame
  `info`. 1 call of 28 failed (exit 20 after 40 s, its session never opened).
  Every other call succeeded.
- So "silent" on M-ZJQ2DX was probably not a deaf agent but a slow one plus
  the old one-session relay: v9xctl's reconnect then sat in a session that
  could not open while the first was being replaced. Both of those are now
  addressed (several sessions, no stealing, minute-by-minute progress line).
- Not covered: hardware 3D. Velocity9x has no Voodoo3 3D path yet; the
  Ironlake/HD 4000 machine is the one to repeat this on, with
  `cat -Path C:\V9XREMOTE\AGENT.LOG` afterwards for `remote-poll-gap`.

## Related

`2026-11-05-relay-one-session-per-machine.md`: during the silence no second
call could be made to check on the guest.
