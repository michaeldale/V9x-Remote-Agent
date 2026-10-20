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
with authoritative size and CRC32 metadata. Each file is limited to 2 GiB
(2,147,483,647 bytes; 256 MiB in agents before 0.11.0 and 64 MiB before 0.6.4;
the wire format is unchanged, only the guest's accept ceiling).

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

The icon's owner window has its own class (`V9xRemoteTray`) so it receives
the icon's mouse messages. Right-click shows a menu with **Settings...** and
**Restart agent**; double-click is Settings. Settings starts
`C:\V9XREMOTE\SETUP.EXE /settings` (greyed out when that file is missing),
and a second click while it is open only brings it to the front. The tray
thread watches that process: exit code 0 means setup saved, and the agent
then restarts, because it reads `AGENT.INI` only at start. Restart removes
the icon (`NIM_DELETE`, so no ghost icon is left), sends the mDNS goodbye,
starts `V9XAGNT.EXE -service` the same way the hot update relaunches, and
exits; the new agent's singleton guard waits out the old mutex. Anything the
old agent was running is cut off. Logged as `tray-settings-opened`,
`tray-settings-closed saved|unchanged` and `tray-restart`.

Since 0.12.0 the menu also has **Remote help...** (`SETUP.EXE /remote`, saved
the same way and restarting the agent), **Check for updates...**
(`SETUP.EXE /update`, which never restarts the agent itself; it reads
"Install update x.y.z..." once a quiet startup check found one), and, while
remote help is on, **Pause remote help** / **Resume remote help** and **Chat
with developer...**. A message from the developer opens the chat window by
itself, with a beep. The tooltip reads "In use: last command N s ago from
<address or relay>" for a minute after any command other than HELLO and
PING, and with remote help on it switches to a shorter form ending in
"Remote help: on|paused|in session|offline". Remote help itself, its
`[remote]` and `[update]` settings, `C:\V9XREMOTE\STATUS.INI` and the log
events (`remote-registered`, `relay-session-open`, `relay-bad-chunk`,
`relay-session-closed <reason>`, ...) are in [remote-help.md](remote-help.md).

The boot counter is flushed to `BOOT.DAT`. An accepted reboot or shutdown first
flushes its resume token to `PENDING.DAT`; the next HELLO and INFO return both
values so the host can prove the intended boot completed. Power requests are
rejected while a child execution is active.

### `reset` instead of `reboot`

Use `reboot` wherever Windows restarts cleanly: it is Windows' own shutdown.
Use `reset` (agent 0.10.0+, Windows 9x only) on a machine whose restart hangs
after Windows has shut down, typically because a real-mode driver loaded from
`CONFIG.SYS` or `AUTOEXEC.BAT` (an NDIS2 network stack, for one) is still
resident when Windows returns to real mode. The symptom is a flashing cursor
or a fatal exception 0C where the BIOS screen should be, from the Start menu
as well as from `reboot`.

`reset` never leaves protected mode. After accepting it the agent sends
`WM_ENDSESSION` to other programs' windows (no `WM_QUERYENDSESSION`, so
nothing can refuse or ask to save, as with `reboot`'s `EWX_FORCE`), flushes
the registry, takes a level-1 lock on each fixed drive (which writes out the
disk cache and leaves the volume marked clean, so ScanDisk does not run),
stops writing its own log, gives the drive 3 s to empty its write cache, and
power-cycles the machine through the full reset of port CF9h. Files written before the request, including a `WININIT.INI`, are
on disk at the next boot; `WININIT.INI` paths must be 8.3 as always. It is
not a full shutdown: RunServices processes and VxDs get no notice, and
programs that save only after `WM_QUERYENDSESSION` save nothing. There is no
fallback: if CF9h does not reset the machine the agent logs `reset-failed` and
stays up, and the host's proof times out (exit 42). Measurements and reasoning:
[decisions/2026-10-12-reset-without-real-mode.md](decisions/2026-10-12-reset-without-real-mode.md).

Desktop readiness requires a visible desktop plus an Explorer `Progman` or
`Shell_TrayWnd` window. The disposable `V9XSHOT.EXE` helper uses GDI to write a bounded 24-bit
BMP under `C:\V9XREMOTE\TEMP` by default. It reports file size and CRC32 before
the host retrieves the image through the transactional download path.

The package registers the executable in the machine-wide Windows 9x
`RunServices` key. The executable should be tested interactively once before
enabling boot startup in the reference image.

### Live remote control (`V9XVNC.EXE`)

`V9XVNC.EXE` is a VNC (RFB) server that ships beside the agent for a person
to watch and drive the machine with any VNC viewer. It is **off by default**:
it serves only when `AGENT.INI` has `[vnc] enabled=1`.

**Turning it on and off.** The machine's owner turns it on in SETUP.EXE with
"Enable VNC remote control", or with `SETUP.EXE /S /vnc` for a silent
install. Setup writes `enabled=1`, sets the server to start automatically,
and starts it now. Unticking the option, `/novnc`, or uninstalling reverses
all of that. `INSTALL.BAT` copies the binary but never enables it. How it
autostarts depends on the system:

- **Windows 9x:** from `HKLM\...\Run` (value `V9xRemoteVnc`) at logon, as the
  logged-on user.
- **Windows 2000:** as a Windows service (`V9xRemoteVnc`), running as
  LocalSystem so it can reach the Winlogon desktop (below). `V9XVNC.EXE
  -install` and `-uninstall` create and remove it; `-install` also starts it.

**Windows 2000: the logon and lock screens.** The service follows the input
desktop: when Windows switches to the secure Winlogon desktop (the lock /
"Unlock Computer" dialog, a screen saver's password prompt), the capture and
input threads move there with it, so a viewer sees and can drive it. This
needs LocalSystem; a server running as the user (the Run key, or a hand
start) cannot open that desktop and freezes on the last user-desktop frame
until it returns. Sending the Ctrl+Alt+Del secure attention sequence from a
viewer is **not** supported (there is no pre-Vista API for it); pair the VM
with autologon, which skips the "Press Ctrl+Alt+Del to begin" screen. The
capture thread owns no window (the clipboard runs on its own thread) so
`SetThreadDesktop` can move it. Windows 9x has one desktop and does none of
this.

**Process and watchdog.** It is a separate process for the same reason as
`V9XSHOT.EXE`: it calls GDI, which a broken display driver can hang. Inside
it, every GDI call runs on one worker thread. A capture that is more than
5 s late is logged as `capture-stalled`; the server keeps taking input and
sends no frames until the capture returns (`capture-resumed`). If setting up
or tearing down the capture is stuck for 15 s, the server releases every key
and button it holds, logs `gdi-wedged-exit` and exits 10.

From the host:

```powershell
.\scripts\v9xctl.ps1 vnc status -Json   # Installed, Enabled, Running, VncPort
.\scripts\v9xctl.ps1 vnc start          # exit 23 where it is not enabled
.\scripts\v9xctl.ps1 vnc stop
```

**Connecting.** Point any VNC viewer at port 5900 of the guest; under 86Box
SLiRP, add a forward to guest port 5900.

- **Protocol:** RFB 3.3, 3.7 and 3.8.
- **Security:** type None, or VNC authentication (type 2) when a password is
  set. A wrong password is refused after a 2 s delay.
- **Encodings:** ZRLE, Hextile and Raw, taking the first of these in the
  viewer's preference order. CopyRect is used for a moved foreground window.
  DesktopSize, RichCursor and PointerPos pseudo-encodings are supported too.
- **Bandwidth:** one full frame of the test desktop at 1024x768 and 32 bpp
  took about 20 KB with ZRLE, 104 KB with Hextile and 3.1 MB with Raw.
- **Clipboard:** text both ways. Viewer text goes to `CF_TEXT` as Latin-1
  with CRLF; Windows text is checked every 0.5 s and sent when it changes.
  What is on the clipboard at connect time is not sent, and text that came
  from the viewer is not echoed back.
- **Viewers:** one at a time. A second viewer is refused with "Another VNC
  viewer is connected to this machine."
- **Cursor:** a viewer that takes RichCursor draws the cursor itself. Others
  get it drawn into the frame (Windows 98 and later in both cases;
  `GetCursorInfo`), and Windows 95 shows none.
- **Configuration:** the `[vnc]` section of `AGENT.INI`:
  - `enabled`;
  - `port`, default 5900; `-port <n>` overrides it;
  - `poll_ms`, default 100;
  - `password`, blank or at most 8 characters, set with SETUP.EXE's "VNC
    password" field.

  It honours `[agent] bind_address` and `allowed_client`.
- **Log:** `C:\V9XREMOTE\VNC.LOG`, truncated past 64 KiB.
- **Updates:** `v9xctl update` replaces the binary along with the agent.

| Exit code | Meaning |
|---:|---|
| 0 | stopped by `-stop`; `-stop`: request delivered; `-status`: running |
| 1 | `-stop` or `-status`: no server is running |
| 2 | invalid port |
| 3 | already running |
| 4-8 | stop event, memory or thread, Winsock, `allowed_client`, socket, listen failure (see `VNC.LOG`) |
| 9 | disabled (`[vnc] enabled` is not 1) |
| 10 | GDI wedged during capture setup or teardown |

`-install` and `-uninstall` (Windows 2000) create and remove the service and
exit 0 on success. They need administrator rights.

The password is weak protection. VNC authentication is DES with at most 8
characters, nothing on the wire is encrypted, and the password sits in plain
text in `AGENT.INI`, which anyone who can reach the unauthenticated agent can
read. It keeps a casual viewer off the screen; it is not a reason to expose
port 5900 beyond the sandbox network.

For testing, while `C:\V9XREMOTE\TEMP\VNCSTALL.DAT` exists the capture worker
acts as if the display driver had wedged it (see
`tests\integration\vnc_stall.py`). Never leave that file behind on a machine
someone uses.

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

The file must have CRLF line ends. Windows 98 reads an LF-only INI, but its
profile writer appends a new key to an LF-only section as
`<CR>key=value<CR>`, which the reader never finds again. Before 0.10.0 an
LF-only `AGENT.INI` without an `install_id` therefore collected a new one on
every start. Since 0.10.0 the agent (and SETUP.EXE) repairs the file before
reading it: every line end becomes CRLF, blank-line runs collapse, and only
the first `install_id` line is kept, which is the one Windows returns. The
original is saved as `C:\V9XREMOTE\AGENT.BAK` and the agent logs
`config-repaired` (`config-repair-failed` when the file could not be read,
backed up or rewritten, or is over 32 KB). A file that is already clean is not
touched. See
[issues/2026-10-12-install-id-changes-every-boot.md](issues/2026-10-12-install-id-changes-every-boot.md)
for cleaning up a machine still on an older agent.

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
