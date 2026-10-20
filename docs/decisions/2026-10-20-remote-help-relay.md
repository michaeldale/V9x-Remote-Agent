# Remote help: a signed byte relay through the update server

Status: accepted; implemented in 0.12.0, verified on Windows 98 SE and Windows 2000 VMs against a local copy of the server (2026-10-20)
Date: 2026-10-20

## Context

The developer writes drivers for hardware they do not always own. Owners of
that hardware, anywhere on the Internet, should be able to lend a Windows 9x
or 2000 machine for a while: the developer then drives it with the same
tools as a VM on their desk. The machine sits behind the owner's NAT, so the
agent has to dial out, and the only server available is the Bluetrait site
that already runs the `v9x_update_checker` plugin (PHP, plain HTTP for 9x
clients, no long-lived connections). Windows 9x has no usable TLS.

Requirements from the request: Off by default; Off / On / Paused (Paused
blocks commands until the next restart); opt-in page from the tray with
name, GitHub user and email; the server issues a machine id; the developer
gets a list of online machines and a dashboard; two-way chat; the owner can
see when the machine was last commanded; new agent verbs must not need a
server change. The same release adds a self-update check from that server.

## Options considered

1. **Server-side verbs.** An MCP tool on the server queues `{verb, args}`;
   the agent interprets JSON verbs and returns JSON. Works from the
   Bluetrait MCP alone, but needs a verb interpreter (and JSON parser) in the
   runtime-free agent, and the signing key on the server: whoever controls
   the server controls every lent machine.
2. **Opaque byte relay, signed on the developer's PC (chosen).** The server
   stores and forwards bytes. Each chunk towards the agent is Ed25519-signed
   by `v9xsign.exe` on the developer's PC over a canonical text (machine,
   install id, agent nonce, session, sequence, SHA-256 of the bytes); the
   agent checks it against the compiled-in public key before feeding it to a
   loopback connection served by the ordinary `v9x_serve_client`. Every
   existing and future verb works unchanged, and `v9x_mcp.py` / `v9xctl.ps1`
   only gain a transport.
3. **Plain secret per machine.** Simplest, but anyone who sniffs plain HTTP
   or holds the server could command the machine.

The developer initially preferred driving machines only through the
Bluetrait MCP (option 1 with a server-held key), then asked whether local
signing was better; option 2 was chosen because it lets owners be told,
truthfully, that a compromised server cannot run anything on their machine.

## Decision

- Transport: short HTTP polling (60 s idle, 2 s for ten minutes after any
  session or chat; the server sets `next_poll`). During a session the agent
  long-polls `down` (up to 3 s) and posts `up` as soon as its handlers
  answer; the developer side long-polls `up` (up to 20 s).
- Security: commands signed with a key that lives only in
  `%USERPROFILE%\.v9x\relay-signing.key`; replay prevented by an
  agent-chosen nonce per session plus strictly consecutive sequence numbers;
  replies unsigned (forging them misleads the developer, it cannot touch the
  machine). The machine secret only authenticates the machine to the
  server. Plain HTTP means sessions are readable on the path, and the opt-in
  page says so.
- Execution: a loopback socket pair per session, served as a normal
  connection (relay connections skip `idle_timeout`; the server closes
  sessions idle for an hour).
- The server's operator key (header `X-V9X-Operator`) only opens sessions
  and reads machines and chat. Its MCP tools (`remote_*`) list machines,
  chat and manage notes; none can command a machine.
- Paused is in memory only, as asked: a restart is On again.
- Self-update reuses the Velocity9x updater's design and code (Ed25519
  SIGNED.TXT, SHA-256, zip reader), with a separate release key, and stages
  through the existing upgrade path rather than hot-swapping, so a bad
  update never replaces a running agent mid-session.

## Consequences

- The first command after an idle period waits for the machine's next
  check-in (up to a minute).
- Uploads needed pipelining over the relay (stop-and-wait per 32 KiB chunk
  took 188 s for 3 MB; a 1 MiB window takes about 25 s).
- The developer needs `v9x_mcp.py` or `v9xctl.ps1` and the signing key on
  their PC; the Bluetrait MCP alone is not enough to drive a machine, by
  design.
- VNC is not tunnelled.
- `scripts\publish-github.ps1` now strips the exact server host name before
  its private-name check, since the agent must carry that host.
