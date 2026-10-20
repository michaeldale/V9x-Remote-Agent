# Remote help and self-update

Agent 0.12.0 can talk to the update server, `michaeldale.com.au` (the
`v9x_update_checker` plugin of its Bluetrait site), for two things:

- **Self-update**: tray menu "Check for updates..." (and, optionally, a quiet
  check at startup) finds, verifies and installs a newer agent package.
- **Remote help** (opt-in, Off by default): the owner of a real Windows 9x or
  2000 machine lends it to the developer, who can then drive it exactly as if
  it were on the local network, with every `v9xctl` verb and MCP tool, to debug or
  write drivers for hardware the developer does not own.

Both use plain HTTP, because Windows 9x has no usable TLS. Nothing is trusted
for being on the wire: releases and commands are Ed25519-signed, and the
signatures are checked by the agent itself.

## Remote help modes

| Mode | Set where | What happens |
|---|---|---|
| Off (default) | SETUP.EXE `/remote` page | The agent never contacts the server for remote help. |
| On | SETUP.EXE `/remote` page | The agent registers once, then checks in (a "poll") every 60 s, every 2 s while a session or chat is active. Relay sessions are accepted. |
| Paused | Tray menu "Pause remote help" | Still checks in (so chat works and the dashboard shows "paused"), but every relay session is refused and an open one is closed. Not saved: a restart or reboot returns to On. |

The tray tooltip, the `/remote` page and the developer's dashboard all show
when the last command arrived and from where (LAN address or `relay`), so
the owner can see whether the machine is in use.

## Security model

- **Commands are signed on the developer's PC.** Every chunk sent towards
  the agent is signed with a private key that never leaves that PC
  (`%USERPROFILE%\.v9x\relay-signing.key`, made by `v9xsign keygen`). The
  agent verifies each chunk against the public key compiled into it
  (`include\v9xremote\remote_key.h`) before a byte reaches the protocol
  handlers. The server, or anyone on the path, can delay or drop traffic
  but cannot run anything.
- **No replay.** The agent picks a fresh random nonce for every session and
  the signature covers it, the session id and a sequence number that must
  rise by exactly one per chunk.
- **Replies are not signed.** Someone on the path could forge what the
  developer sees, never what the machine does.
- **Not confidential.** Plain HTTP: anyone on the path can read the session.
  Owners are told so on the `/remote` page.
- The machine secret the server issues at registration only identifies the
  machine to the server (who may post replies and chat for it). It grants no
  control over the machine.

## Agent configuration (`AGENT.INI`)

```ini
[remote]
mode=off              ; off | on
server=http://michaeldale.com.au
machine_id=           ; issued by the server at registration (M-XXXXXX)
secret=               ; issued by the server, 32 hex
display_name=
github=
email=

[update]
check_on_startup=0    ; 1 = quiet check at startup, tray shows an update
```

## Agent to server (plain HTTP)

Every call is `<server>/index.php?bt_type=v9xa_<op>` (no rewrite rules
needed, so a test site in a subfolder works too); the headings below name
the op. Text replies are INI-shaped, 7-bit ASCII,
CRLF. Values that may contain anything (chat, names) are percent-encoded.
Every call except `register` carries `?machine=M-XXXXXX` and the header
`X-V9X-Secret: <secret>`; a wrong secret is `status=bad_secret`.

### `POST register`

Body (INI):

```ini
[register]
install_id=0f8e3c2a-1b4d-4e6f-8a9b-0c1d2e3f4a5b
display_name=Jo%20Bloggs
github=jobloggs
email=
version=0.12.0
os=Windows%2098%204.10.2222
computer=RETRO
```

Reply: `[register] status=ok|error|disabled`, `machine=`, `secret=`,
`message=`. Registering an `install_id` the server already knows returns the
same machine id with a new secret (a reinstall keeps its identity).
Rate-limited per address.

### `POST poll?machine=`

Body:

```ini
[poll]
version=0.12.0
state=on              ; on | paused
busy=0                ; 1 while a command arrived in the last 60 s
last_command_age=-1   ; seconds since the last command, -1 never
last_command_source=  ; LAN address or "relay"
session=              ; the session the agent has open, if any
display_name=...
github=...
email=...

[chat]
count=1
1=Using%20it%20now%2C%20please%20wait
```

Reply:

```ini
[poll]
status=ok             ; ok | unknown_machine | retired | bad_secret
next_poll=60

[session]             ; only when the developer has a session for us
id=<32 hex>
state=pending         ; pending | open | closed

[chat]
count=1
1=Back%20in%2010%20min
```

`unknown_machine` makes the agent forget its id and register again;
`retired` stops remote help until the developer sets the machine active
again on the dashboard (or with `remote_set_machine`) and the agent restarts.

### `POST accept?machine=&session=&nonce=<32 hex>`

The agent takes a pending session: it picks the nonce, opens a loopback
connection to its own protocol handlers and tells the server. With
`&refuse=paused` (or another reason) instead of a nonce it declines.
Reply `[session] status=ok|closed`.

### `GET down?machine=&session=&after=<seq>&wait=<s>`

Binary body: zero or more `V9XD` records (see `include\v9xremote\relay.h`)
with seq above `after`, at most about 1 MiB in total but always at least one
record when there is one. Header `X-V9X-Session-State: open|closed`.
Asking with `after=n` also tells the server records up to `n` arrived.
`wait` (0-5) holds the request until a record arrives: the agent asks with
`wait=3` after a quiet round and `wait=0` while traffic flows.

### `POST up?machine=&session=&seq=<n>`

Body: up to 1 MiB of raw bytes the agent's protocol handlers produced, seq
1, 2, 3, ... per session. A repeated seq is accepted and ignored, so a
retry is safe. Reply `[up] status=ok|closed`.

### `POST close?machine=&session=&reason=`

The agent ended the session (loopback closed, paused, bad signature).

## Developer to server (HTTPS)

Header `X-V9X-Operator: <operator key>` (generated on the plugin's
Machines tab, shown once, stored only as a hash; keep it in
`%USERPROFILE%\.v9x\operator.key`). Every call is
`<server>/index.php?bt_type=v9xr_<op>`. Replies are JSON unless stated. The
operator key only opens sessions and reads machines and chat; it cannot
sign anything the agent will run.

| Call | Purpose |
|---|---|
| `GET machines` | Every registered machine: id, names, version, OS, state, online, busy, last command, open session. |
| `POST open?machine=` | Start a session. Closes any earlier one for that machine. Returns `{session, machine, install_id, state}`. |
| `GET status?session=&wait=<s≤20>` | Session state; once `open` it includes the agent's `nonce`. Waits up to `wait` seconds for a change. |
| `POST down?session=` | Body: `V9XD` records already signed by `v9xsign relay`. Seq must continue exactly. Returns `{next_seq}`. |
| `GET up?session=&after=<seq>&wait=<s≤20>` | Binary: `V9XU` u32 seq, u32 length, data, repeated. Header `X-V9X-Session-State`. Acknowledges up to `after`. |
| `POST close?session=` | End the session. |
| `GET chat?machine=&after=<id>` | Chat history. |
| `POST chat?machine=` | Body: message text (UTF-8). |

`mcp\v9x_mcp.py` (`relay` argument on every tool, plus `v9x_relay_machines`
and `v9x_chat`) and `v9xctl.ps1 -Relay M-XXXXXX` wrap all of this; the key
file and `build\tools\v9xsign.exe` do the signing.

Typical use from the developer's PC:

```powershell
.\scripts\v9xctl.ps1 shell -Command "ver" -Relay M-4F7K2Q
.\scripts\v9xctl.ps1 put -Source .\DRIVER.ZIP -Destination C:\WORK\DRIVER.ZIP -Relay M-4F7K2Q
```

The first call of a while waits for the machine's next idle check-in (up to
a minute); for ten minutes after that it polls every 2 s, so later calls
start within seconds. Uploads keep 1 MiB of chunks in flight: about 3 MB in
25 s through an 86Box NAT to a local server. VNC is not tunnelled.

One-time setup on the developer's PC: `build\tools\v9xsign.exe keygen
%USERPROFILE%\.v9x\relay-signing.key` (its public key is in
`include\v9xremote\remote_key.h`), and the operator key from the plugin's
Machines tab in `%USERPROFILE%\.v9x\operator.key`.

## Self-update

`SETUP.EXE /update` (tray: "Check for updates...") asks
`index.php?bt_type=v9xu_check&app=v9x-remote-agent&family=win32&version=<this>`.
The installed copy first copies itself to `C:\V9XREMOTE\TEMP\SETUPUPD.EXE`
and runs that, so the new package's setup can replace `SETUP.EXE`. When the
reply says `status=update` it fetches the `signed=` file (SIGNED.TXT),
checks its Ed25519 signature against `include\v9xremote\release_key.h`,
checks the signed version equals `latest`, is newer than this one and names
the offered file, shows the release notes and asks. On yes it downloads the
zip, checks its size and SHA-256 against the signed file, unpacks it to
`C:\V9XREMOTE\UPDATE\` and runs the new `SETUP.EXE /S` from there, which
stages the new agent exactly like a manual upgrade (it takes over at the
next restart; AGENT.INI and the install ID are kept), then offers to
restart. Any mismatch stops with "Nothing was changed".

With `[update] check_on_startup=1` (a tickbox on the `/remote` page) the
agent checks quietly a minute after starting; when a newer version exists
the tray menu item reads "Install update x.y.z...".

Releases are signed on the developer's PC after CI has built them:
`scripts\sign-release.ps1 -Version x.y.z -Upload` downloads
`v9x-remote-agent-x.y.z-win32.zip` from the GitHub release, writes and signs
SIGNED.TXT with `%USERPROFILE%\.v9x\release-signing.key` (refusing a key
that does not match `release_key.h`) and attaches it to the release; then
the plugin's `updates_publish_github_release` (app `v9x-remote-agent`)
mirrors both. The signing key never goes near CI.
