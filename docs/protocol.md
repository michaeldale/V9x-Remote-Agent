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

The reported port and network strings reflect `AGENT.INI`. IPv4 filtering is
performed before protocol handshake, so a disallowed source is disconnected
without receiving a HELLO response.

Unknown operations return `ERROR_RESPONSE` (`0x8fff`). Malformed framing closes
the connection. Operational requests before HELLO receive an error and close.

## Execution

`EXEC_REQUEST` (`0x0010`) contains:

| Size | Field |
|---:|---|
| 1 | mode: 0 direct, 1 `COMMAND.COM /C` |
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

`CANCEL_REQUEST` (`0x0011`) carries the active execution request ID as a u32.
`CANCEL_RESPONSE` (`0x8012`) echoes that ID and a u32 accepted flag. `PING` and
`CANCEL` remain responsive while the execution worker is active.

## Files

Protocol v1 file paths are non-empty length-prefixed ASCII strings. Individual
files are limited to 67,108,864 bytes and data chunks to 32,768 bytes.

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

## Boot lifecycle and power

`REBOOT_REQUEST` (`0x0040`) and `SHUTDOWN_REQUEST` (`0x0041`) carry a non-empty
ASCII resume token of at most 63 bytes. Before acknowledging, the guest flushes
the token to `C:\V9XREMOTE\PENDING.DAT`. The corresponding accepted response
(`0x8040` or `0x8041`) contains the current boot counter and echoed token.

For reboot, the host disconnects and accepts success only after a fresh HELLO
reports a different boot counter and the exact persisted token. Agent
availability alone is not reboot proof. Power control is refused while an
execution request is active, and any active file transaction is cleaned before
the Windows power request.

## Screenshot

`SCREENSHOT_REQUEST` (`0x0050`) carries a destination path in the guest. The
agent captures the current display into a bottom-up, 24-bit BI_RGB BMP, flushes
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

At most 256 actions per request. The guest validates the whole batch before
applying any of it, then replies with `INPUT_RESPONSE` (`0x8060`): u32 actions
performed, then the s32 cursor x and y after the batch. Buttons and keys carry
explicit down/up so the host composes clicks, drags, and modified hotkeys
(for example CTRL down, ESC down, ESC up, CTRL up to open the Start menu).
Input needs a ready desktop; call the desktop-ready path after a reboot first.
