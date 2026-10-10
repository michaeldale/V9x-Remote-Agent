# Protocol v1

All integers are unsigned little-endian. Strings are 16-bit byte-length-prefixed
7-bit ASCII. Every frame begins with this 24-byte header:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `V9XR` magic |
| 4 | 2 | version (`0x0100`) |
| 6 | 2 | message type |
| 8 | 4 | nonzero request ID |
| 12 | 4 | flags |
| 16 | 4 | payload length, at most 65,536 |
| 20 | 4 | reserved, zero |

The first request on every connection must be `HELLO_REQUEST` (`0x0001`). Its
payload is minimum version, maximum version, and client-label string.

`HELLO_RESPONSE` (`0x8001`) contains selected version (u16), reserved (u16),
capabilities (u32), maximum payload (u32), boot counter (u32), port (u16),
Winsock version (u16), build-ID string, pending-job string, desktop-ready byte,
three reserved bytes, screen width, height, and source bits-per-pixel (u32),
then configured listen-address and allowed-client strings.

After handshake, `PING_REQUEST` (`0x0002`) returns `PING_RESPONSE` (`0x8002`)
with uptime milliseconds and boot counter (both u32).

`INFO_REQUEST` (`0x0030`) returns `INFO_RESPONSE` (`0x8030`) containing boot
counter, uptime, capabilities, port, Winsock version, raw `GetVersion` value,
then strings for agent version, build ID, computer name, reported Windows
version, system directory, Windows directory, current directory, and pending
job. The strings are followed by the same 16-byte desktop/screen block used by
`HELLO_RESPONSE`. Older M1-M3 responses that end after the original fields
remain accepted by the host parser.

An agent that advertises `V9X_CAP_INSTALL_ID` (`0x4000`, 0.8.0+, Win9x and
DOS) also accepts an `INFO_REQUEST` whose payload is one u32 of flags. Flag
`0x1` (`V9X_INFO_FLAG_EXTENDED`) appends one more string after
allowed-client: the install ID, a lowercase RFC 4122 v4 UUID from
`AGENT.INI [agent] install_id`. Hosts send the flags only when the capability
is present (older agents require an empty INFO payload), and agents append
the ID only when asked (older hosts reject trailing INFO data), so every
host/agent combination keeps working.

`INVENTORY_REQUEST` (`0x0070`, empty payload, capability `V9X_CAP_INVENTORY`
`0x2000`, agent 0.7.0+) returns `INVENTORY_RESPONSE` (`0x8070`): a u16 record
count, then per record a type string, a u16 field count, and that many
key string / value string pairs. The format is deliberately schema-free:
clients must ignore record types and keys they do not know, and a key may
repeat inside one record. Values are printable ASCII (other bytes become `?`);
sizes are decimal strings so they are not limited to 32 bits. Records emitted
today:

| Record | Fields |
|---|---|
| `os` (1) | `product_name`, `version_number`, `sub_version`, `plus_version`, `product_type` (NT only: `WinNT`, `ServerNT`, `LanmanNT`), `major`, `minor`, `build`, `platform`, `csd`, `registered_owner`, `registered_org`, `directx`, `ie_version`, `clean_boot` |
| `cpu` (1) | `count`, `type`, `vendor`, `brand`, `family`, `model`, `stepping`, `features` (space separated: fpu tsc cmov mmx mmxext 3dnow 3dnowext sse sse2 sse3), `mhz`, `reg_vendor`, `reg_identifier` |
| `memory` (1) | `total_phys`, `avail_phys`, `total_page`, `avail_page` (bytes), `load` (percent) |
| `drive` (per letter) | `letter`, `type` (removable/fixed/network/cdrom/ramdisk/unknown); fixed and RAM disks add `label`, `fs`, `serial`, `total`, `free` (bytes) |
| `display` (1) | `desc`, `provider`, `width`, `height`, `bpp`, `refresh` (NT only, Hz) |
| `net` (1) | `hostname`, `ip` (repeated) |
| `bios` (0-1) | `date`, `name` (9x only), `version`, `video_version` (NT only) |
| `device` (many) | `class`, `desc`, `provider`, `driver_date`, `id` (hardware key; on NT the device instance ID), `problem` (Configuration Manager code, only when nonzero) |
| `meta` (1, last) | `devices` (`live`: loaded devices from `HKEY_DYN_DATA\Config Manager\Enum`, or on NT the present instances under `HKLM\SYSTEM\CurrentControlSet\Enum` as reported by `CFGMGR32`; `registry`: every installed instance, used when there is no live tree), `truncated=1` when the reply hit the 16 KiB response buffer, `install_id` (0.8.0+), `discovery` (0.8.0+: `off`, `on`, or `unavailable` when the mDNS socket could not bind or join) |

Fields with no value are omitted. Only fixed and RAM disks are probed, with
critical-error dialogs suppressed, so removable, CD-ROM and network drives
report letter and type only. The clock speed is measured by timing the TSC
against the performance counter for about 200 ms, so the reply takes at least
that long on a TSC-capable CPU.

On Windows 2000 (agent 0.9.0+) the same records come from the NT locations:
`os` from `SOFTWARE\Microsoft\Windows NT\CurrentVersion` (`version_number`
is `CurrentVersion.CurrentBuildNumber`, e.g. `5.0.2195`), `bios` from
`HARDWARE\DESCRIPTION\System` (multi-string values are joined with ` / `),
`bpp` and `refresh` from `EnumDisplaySettingsA`, and device names from
`SYSTEM\CurrentControlSet\Control\Class`. Keys a platform has no value for
are simply absent.

The reported port and network strings reflect `AGENT.INI`. IPv4 filtering is
performed before protocol handshake, so a disallowed source is disconnected
without receiving a HELLO response.

Unknown operations return `ERROR_RESPONSE` (`0x8fff`). Malformed framing closes
the connection. Operational requests before HELLO receive an error and close.

## Discovery (mDNS / DNS-SD)

Separate from the TCP protocol. With `AGENT.INI [agent] discovery=1` the Win9x
agent (0.8.0+) runs a responder on UDP 5353, joined to 224.0.0.251 through
Winsock 1.1, that follows RFC 6762 and RFC 6763. It advertises one instance:

| Name | Type | Data | TTL |
|---|---|---|---|
| `_v9xremote._tcp.local` | PTR | `<COMPUTERNAME>-<id8>._v9xremote._tcp.local` | 4500 |
| `_services._dns-sd._udp.local` | PTR | `_v9xremote._tcp.local` | 4500 |
| `<COMPUTERNAME>-<id8>._v9xremote._tcp.local` | SRV | 0 0 `<port>` `v9x-<id8>.local` | 120 |
| same | TXT | `id=<install id>`, `ver=`, `build=`, `dos=0/1`, `os=<major.minor>`, `name=<computer name>` | 4500 |
| `v9x-<id8>.local` | A | primary IPv4 (the bind address, or the first address the host name resolves to) | 120 |

`<id8>` is the first 8 characters of the install ID. It makes both names
unique to the install, which is why the agent skips RFC 6762 probing (see
[decisions/2026-10-01-mdns-discovery-and-install-id.md](decisions/2026-10-01-mdns-discovery-and-install-id.md)).
SRV, TXT and A are unique records (cache-flush bit set); the PTRs are shared.

- A query about any of these names gets them as answers, with the rest of the
  instance (SRV, TXT, A) as additionals.
- A query from a port other than 5353 is a one-shot legacy query (RFC 6762
  6.7): the reply goes unicast to the sender, echoes its ID, clears the
  cache-flush bit and caps TTLs at 10 s. That is how `v9xctl discover` and the
  MCP server ask, so they never need port 5353 on the host.
- A query with the QU bit set is also answered unicast; everything else is
  answered to the group.
- The agent announces twice, a second apart, when the responder starts and
  whenever its address changes, and sends a goodbye (every TTL 0) before a hot
  update or reboot.
- Responses are written without name compression; queries are parsed with it.

The C codec is `src/common/mdns.c`, with host decoders
`ConvertFrom-V9xMdnsPacket` in `scripts/V9xProtocol.ps1` and
`parse_mdns_packet` in `mcp/v9x_mcp.py`. `tests/host/mdns-announce.hex` is
the C encoder's output for the test fixture; the host build checks that it
still matches, and both host test suites decode it.

The DOS agent has no responder; see [dos-agent.md](dos-agent.md).

## Concurrency

The agent accepts up to four concurrent client connections, each served on its
own thread with its own send lock; additional connections are rejected (accepted
then immediately closed). **Request IDs are unique only per connection**, so host
tooling must not assume a globally unique ID space and must match responses (and
cancellations) within the connection that issued the request.

Execution runs in a pool of up to four concurrent jobs (see below), so
`V9X_STATUS_BUSY` on `EXEC_REQUEST` means every slot is in use rather than a
single global lock. Reboot, shutdown, screenshot, and hot-update are machine-wide
operations: they are refused with `BUSY` while any execution slot is active on
any connection, and screenshots are serialized machine-wide because they share a
single helper and temp files.

## Execution

`EXEC_REQUEST` (`0x0010`) contains:

| Size | Field |
|---:|---|
| 1 | mode: 0 direct, 1 shell: `COMMAND.COM /C` on Win9x, `CMD.EXE /C` on NT (0.9.0+) |
| 1 | show-window flag; ignored for direct-mode GUI-subsystem targets |
| 2 | option flags: bit `0x0001` detach; other bits must be zero |
| 4 | timeout milliseconds, maximum 3,600,000 |
| 4 | stdout limit, maximum 1,048,576 |
| 4 | stderr limit, maximum 1,048,576 |
| variable | application string; required for direct mode |
| variable | arguments or shell-command string |
| variable | working-directory string; empty inherits current directory |

The agent replies with `EXEC_ACCEPTED` (`0x8010`), streams raw captured bytes
in `EXEC_STDOUT` (`0x9010`) and `EXEC_STDERR` (`0x9011`), then sends exactly one
`EXEC_COMPLETE` (`0x8011`). Its 28-byte payload contains result category, child
exit code, native error, elapsed milliseconds, total stdout bytes, total stderr
bytes, and flags. Counts describe all captured bytes even when stream output is
truncated to its requested limit.

Result categories are success, creation failure, timeout, cancellation, and
internal failure. Flags separately record stdout/stderr truncation, timeout,
cancellation, anonymous-pipe capture, GUI-window launch (`0x20`), detached
launch (`0x40`), and orphaned-descendant completion (`0x80`).

The detach option (request option bit `0x0001`, advertised by capability
`0x800`) launches the child with all standard handles redirected to `NUL` and
completes immediately: no output is captured, no wait occurs, and the reported
exit code is a nominal 0. Use it for installers and `START`-style launches
whose target outlives the request; the pipe-capture flag is not set and flag
`0x40` is set instead.

Capture pipes are created non-inheritable and the child receives inheritable
duplicates of only the write ends, so agent-side handles cannot leak into the
child's descendants. In shell mode, if every write handle on both pipes closes
while the child's process handle remains unsignaled, the agent waits a 2,000 ms
grace and then completes the request successfully with flag `0x80`, discarding
the lingering `COMMAND.COM` wrapper: on Windows 9x there are no job objects,
and a detached or Win16 descendant can otherwise hold the wrapper (and the
execution slot) open until the timeout.

The show-window flag hides (0) or shows (1) the child's window and applies to
shell mode and console-subsystem targets. When direct mode targets an
executable whose PE subsystem is Windows GUI, the agent does not apply the
flag at all: a hidden GUI window never receives `WM_PAINT` on Windows 9x, so
the child's initial window state is left to the application, matching
`START`. `EXEC_COMPLETE` sets flag `0x20` when this rule was used. If the
application file cannot be opened or parsed before launch, the agent falls
back to applying the show-window flag as requested.

Up to four `EXEC_REQUEST`s run concurrently, one per pool slot; each streams its
own `EXEC_STDOUT`/`EXEC_STDERR`/`EXEC_COMPLETE` keyed by request ID, so a host
must demultiplex by request ID. `V9X_STATUS_BUSY` is returned only when all four
slots are occupied.

`CANCEL_REQUEST` (`0x0011`) carries the target execution request ID as a u32.
Cancellation is scoped to the connection that owns the job (a request ID is only
matched against jobs started on the same connection). `CANCEL_RESPONSE`
(`0x8012`) echoes that ID and a u32 accepted flag. `PING` and `CANCEL` remain
responsive while execution workers are active.

## Files

Protocol v1 file paths are non-empty length-prefixed ASCII strings. Individual
files are limited to 268,435,456 bytes (256 MiB; 64 MiB before agent 0.6.4)
and data chunks to 32,768 bytes. A guest `DOWNLOAD` is held to 2 GiB instead
(agent 0.8.1+), since it streams straight to the guest disk.

- `FILE_STAT_REQUEST` (`0x0020`) carries a path. Its 16-byte response contains
  existence and directory bytes, reserved u16, size, attributes, and native
  error. A missing path is a successful response with `exists = 0`.
- `FILE_LIST_REQUEST` (`0x0021`) carries a directory path. Its response begins
  with a u32 count followed by entries containing attributes, size, and name.
  The complete listing must fit the agent's bounded 16 KiB listing buffer.
- `FILE_MKDIR_REQUEST` (`0x0022`) is idempotent for an existing directory. Its
  response u32 is 1 when created and 0 when already present.

An upload begins with `FILE_OPEN_WRITE` (`0x0023`): expected size, expected
CRC32, and final path. `FILE_WRITE_READY` (`0x8023`) echoes size and CRC32.
Each `FILE_WRITE_CHUNK` (`0x0024`) contains sequential offset plus bytes and is
acknowledged by `FILE_WRITE_ACK` (`0x8024`) with the new offset. A zero-payload
`FILE_COMMIT` (`0x0025`) validates total size and CRC32 before replacing the
destination; `FILE_WRITE_COMPLETE` (`0x8025`) echoes committed size and CRC32.

The guest writes a request-specific `.PART` file. Existing destinations are
renamed to `.V9X.BAK` only after validation; failed final rename attempts roll
the backup into place. Disconnect removes an active part, while a hard crash
may intentionally leave part/backup evidence for recovery.

`FILE_OPEN_READ` (`0x0026`) carries a path. The guest sends zero or more
`FILE_READ_CHUNK` (`0x9026`) frames containing offset plus bytes, followed by
`FILE_READ_COMPLETE` (`0x8026`) with total size and CRC32. The host must verify
both before publishing its local temporary file.

## HTTP download

`DOWNLOAD_REQUEST` (`0x0027`, capability `V9X_CAP_HTTP_DOWNLOAD` `0x1000`) carries
a length-prefixed URL string then a length-prefixed guest destination path. The
guest fetches the URL itself over a hand-rolled HTTP/1.0 client (its only
outbound network path). **Only `http://` is supported**; an `https://` URL is
rejected with `V9X_STATUS_UNSUPPORTED_OPERATION`. The body streams to a `.PART`
file and is committed through the same backup/rename path as an upload, capped at
67,108,864 bytes. During transfer the guest may send `DOWNLOAD_PROGRESS`
(`0x9027`) frames carrying a u32 byte count. On success `DOWNLOAD_COMPLETE`
(`0x8027`) contains the u32 HTTP status (200), total size, and CRC32. A non-200
status or transport failure returns `ERROR_RESPONSE` with the HTTP status in the
native-error field and leaves any existing destination untouched.

## Boot lifecycle and power

`REBOOT_REQUEST` (`0x0040`) and `SHUTDOWN_REQUEST` (`0x0041`) carry a non-empty
ASCII resume token of at most 63 bytes. Before acknowledging, the guest flushes
the token to `C:\V9XREMOTE\PENDING.DAT`. The corresponding accepted response
(`0x8040` or `0x8041`) contains the current boot counter and echoed token.

The boot counter advances whenever the agent starts and is not reboot proof by
itself. For reboot, the host requires disconnect/reconnect to a fresh HELLO
with a different counter and the exact persisted token, followed by desktop
readiness. Agent availability alone is not proof. Power control is refused while an
execution slot is active on any connection, and the calling connection's active
file transaction is cleaned before the Windows power request.

`UPDATE_REQUEST` (`0x0042`, capability `V9X_CAP_DRIVER_UPDATE` `0x0200`) applies a
new agent without a reboot. The request exists from agent 0.6.0, but the
capability bit is older: 0.5.x advertises `0x0200` too and answers
`UPDATE_REQUEST` with `UNSUPPORTED_OPERATION`, so a host must check the INFO
`AgentVersion` (as `v9xctl update` does from 0.9.1) rather than the bit alone. The host first uploads the new `V9XAGNT.EXE` and
`V9XSHOT.EXE` as `C:\V9XREMOTE\V9XNEW.EXE` and `V9XSNEW.EXE` (normal upload path,
CRC-verified on arrival), then sends a 16-byte payload: staged agent size and
CRC32, then staged helper size and CRC32. The guest re-verifies each staged file
(size, CRC32, and MZ header), launches the staged binary itself as
`V9XNEW.EXE -hotswap` (a Win32 process; agents before 0.6.4 wrote and ran a
`HOTSWAP.BAT` through COMMAND.COM instead), replies `UPDATE_ACCEPTED`
(`0x8042`, current boot counter and staged agent size), and exits. The swapper
waits for the executable lock to clear, swaps the binaries, re-asserts the
RunServices entry, relaunches the agent, and records each step in
`HOTSWAP.LOG`; the agent comes back with an incremented boot counter. The host
confirms the upgrade by reconnecting and observing the new boot counter (and
`INFO` agent version). The RunServices entry is never repointed, so an
interrupted swap still recovers on the next reboot. Refused with `BUSY` while any
execution slot is active or an upload is in flight on the connection.

## Screenshot

`SCREENSHOT_REQUEST` (`0x0050`) carries a destination path in the guest. A
disposable helper captures the display into a bottom-up, 24-bit BI_RGB BMP, flushes
it to disk, and returns `SCREENSHOT_RESPONSE` (`0x8050`) with width, height,
source bits-per-pixel, file size, CRC32, and guest path. The host then downloads
that BMP through the normal file-read protocol and verifies both metadata sets.
Capture is bounded to 4096 by 4096 pixels and 16 MiB of pixel storage.

## Input injection

`INPUT_REQUEST` (`0x0060`) carries an ordered batch of synthetic input actions,
applied to the guest in order so a whole gesture (a drag, a hotkey, a typed
string) is one atomic request. The payload is a u16 action count followed by
that many actions, each a u8 opcode and opcode-specific little-endian fields:

| Opcode | Name | Fields |
|---:|---|---|
| 1 | mouse move | s32 x, s32 y (absolute screen pixels, via `SetCursorPos`) |
| 2 | mouse button | u8 button (0 left, 1 right, 2 middle), u8 down (1 down, 0 up) |
| 3 | mouse wheel | s16 notches (positive scrolls away from the user) |
| 4 | key | u8 virtual-key code, u8 down |
| 5 | type | u16 length, ASCII bytes (guest maps each via `VkKeyScan`, holding shift/ctrl/alt as needed) |
| 6 | delay | u16 milliseconds (at most 10,000) |
| 7 | relative mouse move | s32 dx, s32 dy (pixels, as `mouse_event(MOUSEEVENTF_MOVE)` motion in steps of at most 4 so acceleration does not scale it; agent 0.8.1+, advertised as `V9X_CAP_INPUT_RELATIVE` `0x8000`) |

At most 256 actions per request. The guest validates the whole batch before
applying any of it, then replies with `INPUT_RESPONSE` (`0x8060`): u32 actions
performed, then the s32 cursor x and y after the batch. Buttons and keys carry
explicit down/up so the host composes clicks, drags, and modified hotkeys
(for example CTRL down, ESC down, ESC up, CTRL up to open the Start menu).
Input needs a ready desktop; call the desktop-ready path after a reboot first.
