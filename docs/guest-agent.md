# Guest agent

`V9XAGNT.EXE` is a runtime-free Win32 executable linked for Windows runtime 4.0.
It acquires a named mutex, increments `C:\V9XREMOTE\BOOT.DAT`, retries Winsock
startup, and listens on TCP 9869. It handles one client at a time and returns to
`accept` when that client disconnects.

Execution uses one fixed worker thread. Direct mode calls `CreateProcessA` with
the supplied application, while shell mode uses `COMMAND.COM /C`. Separate
anonymous pipes are drained while the child runs to avoid pipe-buffer deadlock.
The control thread continues servicing `PING` and `CANCEL`. Output, execution
time, and command-line sizes are bounded; timeout or cancellation terminates
the child process created for the request. Windows 9x has no job objects, so a
complete descendant process tree cannot be guaranteed to terminate.

The show-window flag is applied through `STARTF_USESHOWWINDOW`, which replaces
the child's first `ShowWindow` call. A GUI child hidden that way never receives
`WM_PAINT`, so paint-driven programs such as `V9XGDI.EXE /auto` hang until the
execution timeout. Direct mode therefore reads the target's PE optional header
before launch: when the subsystem is Windows GUI, the agent skips
`STARTF_USESHOWWINDOW` entirely and the child controls its own initial window
state, exactly as if launched through `START`. The `EXEC_COMPLETE` flag
`0x20` records that this happened. Shell mode and console-subsystem targets
keep the existing behaviour: hidden by default, shown with the show-window
flag.

File uploads are written sequentially to a request-specific `.PART` file and
CRC32-checked before commit. Replacing an existing destination first renames it
to `.V9X.BAK`; a failed final rename rolls that backup into place. Clean client
disconnect removes an incomplete part. A host or guest crash can leave a part
or backup as recoverable evidence, but never writes incoming bytes directly
over the known-good destination. Downloads stream fixed-size chunks and finish
with authoritative size and CRC32 metadata. Protocol v1 limits each file to
64 MiB.

Input injection (`INPUT_REQUEST`) applies a validated batch of synthetic mouse
and keyboard events with `SetCursorPos`, `mouse_event`, and `keybd_event`;
typed text is mapped per character through `VkKeyScan` so shifted characters
work. The whole batch is validated before any event is sent. See
`docs/protocol.md` for the wire format.

The log is plain ASCII at `C:\V9XREMOTE\AGENT.LOG` and records lifecycle and
execution start/completion events.

While it is running, the agent adds an icon to the Windows notification area.
Hovering over the icon shows the agent version, listening port, and IPv4
address. When `bind_address` is `0.0.0.0`, the displayed address is resolved
from the guest machine name after Winsock starts; if resolution is unavailable,
the tooltip keeps the configured `0.0.0.0` value. The tray worker waits for
Explorer during boot and restores the icon if Explorer is restarted.

The boot counter is flushed to `BOOT.DAT`. An accepted reboot or shutdown first
flushes its resume token to `PENDING.DAT`; the next HELLO and INFO return both
values so the host can prove the intended boot completed. Power requests are
rejected while a child execution is active.

Desktop readiness requires a visible desktop plus an Explorer `Progman` or
`Shell_TrayWnd` window. The disposable `V9XSHOT.EXE` helper uses GDI to write a bounded 24-bit
BMP under `C:\V9XREMOTE\TEMP` by default. It reports file size and CRC32 before
the host retrieves the image through the transactional download path.

The package registers the executable in the machine-wide Windows 9x
`RunServices` key. The executable should be tested interactively once before
enabling boot startup in the reference image.

Network settings are read from `C:\V9XREMOTE\AGENT.INI` at process start:

```ini
[agent]
port=9869
bind_address=0.0.0.0
allowed_client=
```

Addresses must be dotted-decimal IPv4. `0.0.0.0` listens on every IPv4
adapter. A blank `allowed_client` preserves the original accept-any behavior;
a specific address rejects every other source before HELLO. Invalid configured
addresses fail closed and are recorded in `AGENT.LOG`. Reboot or restart the
agent after editing the file. Updates preserve an existing `AGENT.INI`.
