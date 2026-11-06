# Relay: one session per machine, so a long wait blocks every other call

Status: fixed 2026-11-06 in agent 0.15.0 + plugin DB version 5 (three sessions at once; busy refusal for older agents); tested locally, not yet deployed
Date: 2026-11-05

## Symptom

On a remote-help machine (`M-ZJQ2DX`, agent 0.14.0 `dev-0140`) a
background `v9xctl wait-window -Title 'Overall Score' -WaitSeconds 1800
-Relay M-ZJQ2DX` was waiting for 3DMark 2000 to finish. Nothing else could be
asked of the machine for as long as it ran:

- no `screenshot`, `info`, `windows` or `cat` to see whether the run was
  progressing or the guest had hung;
- no `kill` or `reset` to recover it.

The operator had to choose between leaving the wait alone and killing the
progress check the wait was itself performing. The only visibility left was
the server's `GET machines` row (`last_seen`, `online`), which is outside the
relay.

The same limit already costs time elsewhere: the Ironlake note in the
Velocity9x project reads "the relay takes one session at a time: never run
two v9xctl calls (or a Monitor ping) at once", and a probe run and a
screenshot cannot overlap.

## Cause

By design, in `docs/remote-help.md`:

- `POST open?machine=` "closes any earlier one for that machine", so a second
  `v9xctl ... -Relay` call ends the first one's session rather than queueing
  or sharing it;
- the agent accepts one pending session per poll (`[session]` in the poll
  reply) and bridges it to one loopback connection.

A LAN agent has no such limit: it serves up to four concurrent client
connections (`docs/protocol.md`), so the restriction is the relay's, not the
protocol's.

Seen once more today: a `screenshot` issued while 3DMark was starting returned
`Relay session closed (agent:loopback-closed)`, exit 20, and the next `info`
reopened a session normally.

## What would help

Any of these, cheapest first:

1. **Refuse instead of stealing.** `POST open` while a session is open returns
   a clear "busy: session `<id>` opened `<age>` s ago by `<verb>`" error, and
   `v9xctl` exits with its own code. The running wait survives and the second
   caller knows why.
2. **Several channels in one session.** Multiplex connections inside a
   session (a channel id in the `V9XD`/`V9XU` record header), and have the
   agent open one loopback connection per channel. `v9xctl` reuses an open
   session for the machine when one exists.
3. **A read-only side door.** Let `GET machines` (or a new `GET
   probe?machine=`) carry what the agent already reports in its poll body,
   plus the foreground window title and uptime, so a stuck wait can be
   judged without a second session.

Option 1 is a server change only. Options 2 and 3 need the agent too.

## Review (2026-11-06)

Option 1 belongs in the update server's `v9x_update_checker` plugin
(`v9xr_endpoint_open` in `inc/agent.php`, in the blog repository), not in
this repository; `V9xRelayStream` already reports a refused open as "refused
the session (<reason>)", so a `busy` reason would reach the operator with no
host change beyond a dedicated exit code. Option 2 is the real fix and is a
protocol-level change (a channel id in `V9XD`/`V9XU` records, several
loopback connections per session in `src/guest/remote.c`, session reuse in
`V9xRelayStream` and `mcp/v9x_relay.py`). Until then: while a long wait
runs on a remote-help machine, make no second call to it. 0.15.0's
minute-by-minute "session pending; agent last checked in N s ago" line (see
the silent-guest issue) at least shows whether the waiting guest is alive.

## Option 1 done (2026-11-06)

- **Server** (`v9x_update_checker`, DB version 4): relay sessions record an
  `owner` (32 hex the client picks), a `label` (the verb or MCP tool) and
  `operator_seen`, touched by the operator's `status`, `down` and `up`.
  `POST open` refuses with HTTP 409 `busy: true` while another owner's
  session was touched in the last 60 s, naming the label and ages. The same
  owner replaces its own session (`reconnected`: v9xctl's wait loop opens
  the new session before it drops the old one), an untouched one is
  `replaced` as before, and `replace=1` is `taken-over`.
- **v9xctl**: one owner per process, label = verb, exit **21** "Relay busy:
  ..." (not retried as a transport failure), `-RelayTakeOver` to recover a
  stuck wait.
- **MCP**: one owner per tool call, label = tool name, `RelayBusyError`,
  `relay_take_over` on every relay-capable tool.
- Tested: the PHP open logic against in-memory SQLite (refusal, reconnect,
  take-over, abandoned replace, column upgrade), MCP unit tests with a stubbed
  server, host suites. **Not yet tested against the live server**, which
  still runs DB version 3 until the plugin is deployed; until then v9xctl
  sends the new parameters and the old server ignores them (old behaviour).

## Option 2, done as several sessions (2026-11-06)

Instead of channels inside one session (which would change the `V9XD`/`V9XU`
records and what is signed), the agent runs **up to three independent
sessions**, each on its own thread, buffers and loopback connection, with its
own nonce and sequence. Nothing in the record format, signing, v9xctl or MCP
changed: every call already opens its own session.

- Agent (`src/guest/remote.c`): check-ins say `sessions=a,b` and
  `max_sessions=3`; each pending session in the reply's `[sessions]` (or the
  old `[session]`) starts a worker. The check-in thread keeps its own pace
  during sessions; a worker ends on `X-V9X-Session-State: closed`, a 404/410
  from `down`, pause, retirement, or 5 minutes without the server.
- Server: machines remember `max_sessions` (1 when a poll does not say);
  `open` admits that many, then replaces an unused one, then refuses (busy).
- Compatibility, tested live on the `Win98SE-Fast-D3D` VM against the local
  server: agent 0.12.1 got one session, the second call exited 21 and
  `-RelayTakeOver` worked; after updating it to 0.15.0 `dev-0150b`, a
  `screenshot` and `windows` ran while a `wait-window` held its session, and
  the wait finished normally. A 0.15.0 agent on an old server only ever sees
  the single `[session]`, which is the old behaviour.
- Found on the way: the agent percent-encodes poll values, so the comma in
  `sessions=` arrives as `%2C`; the server decodes it (`v9xa_held_sessions`).
  Before that fix two live sessions were both closed as `agent-dropped`.
- Still true: a screenshot refuses while an `exec`/`shell` is running
  (`execution active`, exit 43), on the LAN too. Waits (`wait-window`,
  `wait-process`, `exec -WaitForExit`) hold no execution, so they are fine.

## Related

`2026-11-05-relay-silent-guest-during-3d.md`: why the guest stopped answering
during the same wait.
