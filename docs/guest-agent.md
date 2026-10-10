# Guest agent

`V9XAGNT.EXE` is a runtime-free Win32 executable linked for Windows runtime 4.0.
It acquires a named mutex, increments `C:\V9XREMOTE\BOOT.DAT`, retries Winsock
startup, and listens on TCP 9869. It serves up to four concurrent client
connections, each on its own thread; a fifth is accepted and closed at once.

Execution runs in a pool of up to four concurrent jobs, each on its own worker
thread. Direct mode calls `CreateProcessA` with
the supplied application, while shell mode uses `COMMAND.COM /C` (`CMD.EXE /C` on Windows 2000). Separate
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
with authoritative size and CRC32 metadata. Each file is limited to 256 MiB
(64 MiB in agents before 0.6.4; the wire format is unchanged, only the guest's
accept ceiling).

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
the tooltip keeps the configured `0.0.0.0` value. The tray worker owns the
icon's window, waits for Explorer during boot, pumps its own message queue, and
restores the icon if Explorer is restarted. Success is logged as
`tray-icon-added`; each failure names itself with a `gle=` code
(`tray-window-failed`, `tray-createicon-failed`, `tray-loadicon-failed`,
`tray-thread-failed`, `tray-notify-failed`). The icon was broken in every
release before 0.6.2 - see
[decisions/2026-09-04-tray-icon-createthread-lpthreadid.md](decisions/2026-09-04-tray-icon-createthread-lpthreadid.md).

The icon is a small application window: black outline, navy title bar, white
body. The body flashes lime (250 ms on, 250 ms off) while the agent is busy:
any client connected, any execution running, or a frame received or sent in
the last 750 ms, so even a `ping` produces one visible blink and a long
`exec` flashes for its whole duration. Both icon variants are built by the
agent in memory as 16-colour icon resources and created with
`CreateIconFromResource`, because the agent is not allowed to import GDI32;
if that fails the agent logs `tray-createicon-failed` and uses the stock
`IDI_APPLICATION` icon without activity feedback. See
[decisions/2026-09-24-tray-activity-icon-without-gdi.md](decisions/2026-09-24-tray-activity-icon-without-gdi.md).

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

## Before anyone logs on

`RunServices` runs before the logon dialog, so on a guest that prompts at boot
the agent is already listening while that dialog sits on screen. Measured on
98 SE: `ping`, `info`, `exec`, `shell`, `stat`, `put`, `get` and `input` all
work in that state; `screenshot` returns guest error 6 (host exit 43) and
`wait-desktop` times out (host exit 44), because both need Explorer and
Explorer has not started yet.

An unattended guest that prompts therefore stalls every run at its first
`wait-desktop` while `ping` keeps succeeding. `scripts\set-autologon.ps1`
reports and fixes that, and `-Dismiss` clears a dialog that is already up. Full
evidence and the Windows 9x autologon rules are in
[decisions/2026-09-04-prelogon-reachability-and-autologon.md](decisions/2026-09-04-prelogon-reachability-and-autologon.md).

Network settings are read from `C:\V9XREMOTE\AGENT.INI` at process start:

```ini
[agent]
port=9869
bind_address=0.0.0.0
allowed_client=
allow_dos_vm=1
discovery=0
idle_timeout=300
install_id=11d8b52d-b5e6-4bf2-ae56-8d39cd39c0ca
```

`SETUP.EXE` writes all of these from its Options page (run it again from
`C:\V9XREMOTE` to change them), and `INSTALL.BAT` leaves an existing file
alone. `idle_timeout` (0.9.0+) is not on the Options page; a file without it
gets the default.

`idle_timeout` is how many seconds a connection may send nothing before the
agent closes it and logs `client-idle-timeout` (default 300, `0` never,
capped at 86400). It is checked every 5 seconds, so a close lands up to 5
seconds late. A connection that owns a running EXEC is never closed for being
idle, since the host only receives while one runs. It exists for clients that
vanish without a FIN or RST reaching the guest: before it, such a connection
waited in `recv` forever, holding one of the four connection slots and keeping
the tray icon busy.

`install_id` identifies the install, not the address: hosts use it to find
and recognise a machine after DHCP or a move gives it a new IP. SETUP.EXE
creates it, and the agent creates one on start-up if the key is missing or
malformed (logged as `install-id-created`), so installs made by INSTALL.BAT or
upgraded by hot update get one too. It never changes after that. Delete the
line to get a new one, which is worth doing on a disk image cloned from
another installed machine, since the clone carries the same ID. It is
reported in INVENTORY and in INFO (see [protocol.md](protocol.md)).

`discovery=1` starts an mDNS/DNS-SD responder that advertises the agent as
`_v9xremote._tcp` (see [protocol.md](protocol.md#discovery-mdns--dns-sd)).
The shipped `AGENT.INI` has it off, so the agent says nothing on the network
unless asked; SETUP.EXE's checkbox is ticked by default. It only helps where
the host shares a LAN with the guest (a physical machine, or a bridged/TAP
VM). Under SLiRP NAT the host never sees guest multicast. Start-up logs
`mdns-active <instance>`, a socket that cannot bind or join logs
`mdns-unavailable` and is retried every 30 s, and INVENTORY `meta discovery`
reports the state.

Addresses must be dotted-decimal IPv4. `0.0.0.0` listens on every IPv4
adapter. A blank `allowed_client` preserves the original accept-any behavior;
a specific address rejects every other source before HELLO. Invalid configured
addresses fail closed and are recorded in `AGENT.LOG`. Reboot or restart the
agent after editing the file. Updates preserve an existing `AGENT.INI`.

`allow_dos_vm=0` stops the agent from doing anything that makes Windows
create a DOS virtual machine, because some display drivers (the Velocity9x
GMA driver, for one) corrupt the screen whenever a VM starts, hidden or not.
With it set the agent withdraws the `EXEC_SHELL` capability from HELLO and
INFO, so `shell` fails on the host with exit 23 before anything is sent, and
`exec` of anything other than a Win32 GUI-subsystem executable (console EXEs,
DOS programs, batch files) is refused with `UNSUPPORTED_OPERATION`. Start-up
logs `dos-vm-disallowed`. On Windows 2000 (0.9.0+) `CMD.EXE` and console EXEs
are native Win32 programs that start no VM, so there `EXEC_SHELL` stays
advertised and only a direct `exec` of a non-PE image (a DOS or Win16
program, which NTVDM would run) is refused. Hot updates never need a VM from 0.6.4 on: the
swapper is the staged agent binary run with `-hotswap`, not a batch file.
`scripts\set-autologon.ps1 -Enable`/`-Disable` still use `shell` to move the
password list and will not work on such a guest; `-Dismiss` and reporting do.
