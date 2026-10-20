# Changelog

All notable changes to the V9x Remote Agent. Dates are in YYYY-MM-DD.

## 0.12.0 (2026-10-20)

### Added
- **Remote help** (docs\remote-help.md): the owner of a real Windows 9x or
  2000 machine can lend it to the developer over the Internet, and the
  developer drives it with every existing verb and MCP tool.
  - **Off by default.** The tray's new "Remote help..." item opens
    `SETUP.EXE /remote`, which explains what lending allows, takes a name,
    GitHub user and email, and sets `[remote] mode=on`. The agent then
    registers its install ID with the update server, which issues a
    machine id (`M-XXXXXX`), and checks in every 60 s (2 s while a session
    or chat is active). "Pause remote help" on the tray blocks commands
    until the agent restarts; chat keeps working.
  - **Signed relay.** A session is a byte tunnel from the developer's PC to
    the agent's own handlers, through the server over plain HTTP. Every
    chunk towards the agent is Ed25519-signed on the developer's PC by the
    new `build\tools\v9xsign.exe` and checked by the agent against
    `include\v9xremote\remote_key.h`, bound to an agent-chosen nonce and a
    sequence number, so neither the server nor anyone on the path can run
    anything; a bad chunk ends the session (`relay-bad-chunk`).
  - **Host side.** `v9x_mcp.py` takes `relay="M-XXXXXX"` on every agent tool
    (or `--relay`), and adds `v9x_relay_machines` and `v9x_chat`;
    `v9xctl.ps1` takes `-Relay M-XXXXXX` on every verb. Uploads keep 1 MiB
    of chunks in flight through the relay. Reboot and update proofs
    reconnect through a new session.
  - **Chat.** Two-way: a message from the developer pops up in the agent's
    chat window (tray: "Chat with developer..."), and the owner's replies
    reach the developer's dashboard and MCP.
  - **In use.** The agent remembers when the last command (anything but
    HELLO and PING) arrived and from where (a LAN address or `relay`). The
    tray tooltip reads "In use: last command N s ago from ..." for a minute
    afterwards; the age also goes to the server and to `C:\V9XREMOTE\STATUS.INI`,
    which the `/remote` page shows.
  - **Server.** The `v9x_update_checker` plugin (separate repository) gains
    the agent and developer endpoints, a Machines tab (online machines,
    owners, state, last command, chat) and MCP tools `remote_list_machines`,
    `remote_get_machine`, `remote_chat_send`, `remote_chat_read` and
    `remote_set_machine`, none of which can command a machine.
- **Self-update.** Tray "Check for updates..." runs `SETUP.EXE /update`: it
  asks the update server, verifies the release's Ed25519-signed SIGNED.TXT
  against `include\v9xremote\release_key.h` and the downloaded zip's size
  and SHA-256 against that, shows the release notes, and on the owner's
  yes stages the new version through the normal upgrade path (it takes over
  at the next restart). `[update] check_on_startup=1` (a tickbox on the
  `/remote` page) checks quietly after start and turns the tray item into
  "Install update x.y.z...". The crypto, inflate and zip reader are ported
  from the Velocity9x updater, with its host tests.
- `make-release.ps1` also builds `v9x-remote-agent-<version>-win32.zip`, the
  self-update package, and the release workflow publishes it;
  `scripts\sign-release.ps1` signs it afterwards on the developer's PC.

### Changed
- The DOWNLOAD verb's HTTP client moved to `src\guest\http.c`, shared with
  remote help and SETUP.EXE; a silent server now times out after 60 s
  instead of hanging the request.
- SETUP.EXE imports WSOCK32 (for `/update`).
- `publish-github.ps1` strips the update server's exact host name before its
  private-name check.

## 0.11.0 (released as part of 0.12.0)

### Added
- `V9XVNC.EXE`: live remote control over standard VNC (RFB 3.3/3.7/3.8), so
  any VNC viewer can watch the screen and drive the keyboard and mouse.
  - **Off by default.** It serves only when `AGENT.INI` has `[vnc]
    enabled=1` (exit 9 otherwise). SETUP.EXE has an "Enable VNC remote
    control" option, unticked by default; `/vnc` and `/novnc` set it for a
    silent install. Ticked, setup writes `enabled=1`, starts the server from
    `HKLM\...\Run` at every logon (on Windows 9x and 2000, since it needs a
    desktop) and starts it now. Unticked, or on uninstall, setup removes that
    and stops a running server. `INSTALL.BAT` copies the binary but never
    enables it.
  - **Separate process.** It runs apart from the agent, like `V9XSHOT.EXE`,
    because GDI capture can hang in a broken display driver; the agent never
    imports GDI32. Inside the server every GDI call runs on one worker
    thread. A capture that takes over 5 s is logged as `capture-stalled`:
    the server keeps reading input and sends no frames until it recovers
    (`capture-resumed`). If setup or teardown is stuck for 15 s, it logs
    `gdi-wedged-exit` and exits 10, releasing every held key first.
  - **Settings.** One viewer at a time, security type None unless a password
    is set (see below), `bind_address`
    and `allowed_client` honoured. Port and poll rate are in `[vnc]` (default
    5900, 100 ms).
  - **Frames.** The screen is polled into a 32 bpp DIB section and only
    changed 16x16 tiles are sent, as Raw or Hextile, in whatever 8/16/32 bpp
    true-colour format the viewer asks for; a colour-map viewer gets a fixed
    3-3-2 palette. The cursor is drawn into the frame (Windows 98 and later).
    A mode change is sent as DesktopSize, or drops a viewer that does not
    support it.
  - **Keys.** X11 keysyms map to virtual keys, with extended keys flagged and
    Shift added when a layout needs it. Everything held is released when the
    viewer disconnects.
  - **Cursor.** A viewer that takes the RichCursor pseudo-encoding gets the
    cursor shape on its own and draws it locally, so the frame no longer
    contains it and moving the mouse dirties no tiles. The shape is rendered
    over black and over white to find transparent and inverting pixels.
    PointerPos reports moves the guest makes itself. Other viewers still get
    the cursor drawn into the frame.
  - **A second viewer** gets RFB's connection-failed reply ("Another VNC
    viewer is connected to this machine.") instead of hanging in the listen
    backlog.
  - **ZRLE.** The deflate encoder is written for this project
    (`src\guest\vncz.c`: fixed Huffman, greedy LZ77, a sync flush after each
    rectangle, one zlib stream per connection), since the build has no C
    runtime or zlib. Each tile takes the smallest of raw, solid, packed
    palette, plain RLE and palette RLE. One full 1024x768 frame of the test
    desktop at 32 bpp took 20 KB, against 104 KB for Hextile and 3.1 MB for
    Raw. The server takes the first of ZRLE, Hextile and Raw in the viewer's
    order.
  - **CopyRect.** When the foreground window turns up elsewhere at the same
    size, the viewer copies the pixels it already has, and the tile diff
    then sends only what the copy missed.
  - **Clipboard both ways.** Text only, as `CF_TEXT` with Latin-1 and CRLF on
    the Windows side. Changes are found by polling every 0.5 s and compared
    by hash, so text from the viewer is not echoed back. All clipboard calls
    run on the GDI worker, so the watchdog covers an owner that hangs; its
    wait now also handles window messages, because the clipboard owner window
    must answer them.
  - **Optional VNC password** (`[vnc] password`, at most 8 characters, a
    masked field in SETUP.EXE). Viewers then use VNC authentication; DES is
    in `src\guest\vncdes.c` and checked against FIPS known answers by the new
    host test. A wrong password is refused after 2 s. `vnc status` and
    `v9x_vnc` report `PasswordSet`, never the password.
  - **Sampled capture.** Reading the whole screen back was the bottleneck:
    about 780 ms per 1024x768 frame on an emulated S3 ViRGE at 16 bpp,
    against about 80 ms to diff and next to nothing to encode, which held a
    viewer to roughly one frame a second. Each poll now reads one pixel row
    per 16-row band of tiles, rotating which row through all 16. It then
    reads in full only the tiles whose sampled row changed, those that
    changed last frame, and the area under a drawn cursor, and the diff
    checks only those tiles. A full read still happens for a full-frame
    request and after a resize. In the same typing test, polls went from
    10-12 to 98 per 10 s (about 54 ms each), so the 100 ms poll interval is
    now the limit. `perf-*` lines in `VNC.LOG` (every 60 s and at session
    end) give capture, diff, encode and send times.
  - **Windows 2000.** On NT the server installs and runs as a Windows service
    (`V9xRemoteVnc`) as LocalSystem, instead of the Run key, because only
    SYSTEM can open the Winlogon (secure) desktop. The capture and input
    threads follow the input desktop with `OpenInputDesktop` /
    `SetThreadDesktop`, so a viewer sees and can drive the lock / "Unlock
    Computer" dialog and the logon screen, not just the user desktop. The
    capture thread owns no window (the clipboard moved to its own thread,
    which owns the hidden clipboard window) so `SetThreadDesktop` can move
    it. `V9XVNC.EXE -install` / `-uninstall` manage the service (SETUP.EXE
    does this on NT); `-service` is the SCM entry point. Sending Ctrl+Alt+Del
    from a viewer is not supported (no pre-Vista API); pair the VM with
    autologon. All of this is resolved at run time and is inert on Windows 9x
    and Windows 95. Service support: `src\guest\vncsvc.c`.
  - **Command line.** `-stop` and `-status` (exit 0 running, 1 not).
  - **Test hook.** While `C:\V9XREMOTE\TEMP\VNCSTALL.DAT` exists the capture
    worker behaves as if wedged, so `tests\integration\vnc_stall.py` can
    drive the watchdog on a healthy machine.
- `v9xctl vnc status|start|stop` and the MCP `v9x_vnc` tool. They report
  whether V9XVNC.EXE is installed, enabled and running. `start` is refused
  (exit 23) on a guest where it is not enabled; the agent's protocol is
  unchanged. `v9xctl update` and the MCP `v9x_update` also replace
  `V9XVNC.EXE` when the package has it: they stop it first and restart it
  only if it was running.
- Verified on a Windows 98 SE VM:
  - against a frame-exact test client (`tests\integration\vnc_smoke.py`) at
    every depth and over all three RFB versions;
  - input, a resolution change, and stop/start;
  - SETUP.EXE's checkbox in the wizard, and `/S /vnc` and `/S /novnc`;
  - both `update` paths;
  - RichCursor (the frame then matched the cursor-less screenshot exactly)
    and PointerPos;
  - the busy reply to a second viewer;
  - the service on Windows 2000, capturing and driving the locked secure
    desktop ("Unlock Computer") that a user-context server could not;
  - a forced stall: input was still served, no frames were sent until the
    stall ended, and a stalled setup exited 10 after 15 s;
  - ZRLE decoding to frames identical to Hextile's at 32, 16 and 8 bpp;
  - CopyRect while dragging a window;
  - clipboard text with umlauts and a line break in both directions;
  - the password field in SETUP.EXE, and right and wrong passwords over RFB
    3.3, 3.7 and 3.8.
- `build-host-tests.ps1` also builds `tests\host\test_vnc.c`. It checks DES
  against the FIPS known answers, and Python's zlib must inflate the ZRLE
  deflate output back exactly, as one stream.

  Design: `docs/decisions/2026-10-15-vnc-remote-control.md`.
- Tray icon menu. Right-click the agent's notification-area icon for
  **Settings...** and **Restart agent**; double-click opens Settings.
  Settings runs the installed `C:\V9XREMOTE\SETUP.EXE /settings`, which
  opens straight on the options page (LAN discovery, DOS programs, TCP port,
  listen address, allowed client, VNC and its password) under a "Settings"
  title. Save writes `AGENT.INI` and applies VNC as before; when setup exits
  0 the agent restarts itself (new boot counter, about a second) so the
  settings apply without a reboot. Cancel changes nothing. Settings is
  greyed out when `SETUP.EXE` is not installed. Log events:
  `tray-settings-opened`, `tray-settings-closed saved|unchanged`,
  `tray-restart`, and `tray-class-failed`, `tray-settings-failed`,
  `tray-restart-failed` with a `gle=` code. Verified on Windows 98 SE and 2000.

### Changed
- `put`, `get` and `push-tree` take files up to 2 GiB (2,147,483,647 bytes),
  up from 256 MiB, on both the Windows and DOS agents (`V9X_MAX_FILE_SIZE`),
  the same ceiling `download` has had since 0.8.1. The wire format is
  unchanged: sizes and offsets were already 32-bit and every path streams
  32 KiB chunks to disk. 2 GiB rather than 4 keeps every size inside a signed
  32-bit file length (DOS `filelength`, FAT16's own limit). The MCP server now
  streams `v9x_put_file`, `v9x_get_file` and `v9x_push_tree` through the file
  instead of holding the whole file in memory, and `v9xctl put` no longer
  reads a large file whole for the LF-only check. An older agent still
  rejects anything over 256 MiB with `LIMIT_EXCEEDED` (exit 40), so update
  the guest first. Verified on a Windows 98 SE VM with a 300 MiB `put`
  (314,572,800 bytes, CRC matched); `get` round trips were checked at 16 MiB.
- Faster `v9xctl put`, `get` and `push-tree`. The host hashed files with a
  PowerShell loop at about 2.5 MiB/s, so a 300 MiB put spent two minutes
  before sending anything; large buffers now go through a compiled CRC32
  (`Add-Type`, compiled on first use, script-loop fallback): 300 MiB hashes in
  about a second. The put loop reuses one frame buffer instead of building
  two arrays per 32 KiB chunk, which had also been unrolled to `object[]` and
  copied back on every write. 16 MiB `put` on the Windows 98 SE VM went from
  0.91 to 1.07 MB/s, against 1.13 MB/s from the MCP server.
- The agent asks for 64 KiB socket buffers on its listener and sets
  `TCP_NODELAY` on each connection (Windows agent; the DOS agent is
  unchanged). Round-trip time for a small request on the VM fell from about
  4 ms to 1 ms, which helps many-request work such as `push-tree` of small
  files. Bulk throughput did not change: on 86Box it is set by the emulated
  NIC (about 1.1 MB/s on the ISA NE2000, 1.75 MB/s on PCnet-FAST at the same
  200 MHz CPU), and sending several chunks before waiting for acks gained
  nothing, so the protocol stays stop-and-wait.
- `v9xctl update` and the MCP `v9x_update` also replace
  `C:\V9XREMOTE\SETUP.EXE` when the package has it (`SetupUpdated` /
  `setup_updated`). The tray's **Settings...** runs that copy, so after a hot
  update it still showed the old version in its title. A copy that is open
  in the guest is left alone with a warning; the agent update goes ahead.

### Fixed
- VNC on Windows 2000 could not be queried, stopped or upgraded from the
  host. Three bugs, found while testing the tray menu:
  - `v9xctl vnc status` failed with "You cannot call a method on a
    null-valued expression", and `update` skipped `V9XVNC.EXE` with the same
    warning, whenever `AGENT.INI` had a `[vnc] port=` line (the Windows 2000
    VM's does). The port test's own `-match` replaced `$Matches` before the
    value was read. The MCP server parses the INI without regexes and was
    not affected.
  - `V9XVNC.EXE -status` reported the Windows 2000 service as not running,
    and `-stop` could not stop it: the service's stop event carries
    LocalSystem's default DACL, so the logged-on user's `OpenEventA` gets
    access denied. `-status` now counts access denied as running; `-stop`
    and a plain start go through the SCM when the service is installed
    (start or stop the service, 0 ok, 1 failed), so `v9xctl vnc start`
    brings back the service rather than a user-session server that cannot
    reach the logon desktop.
  - SETUP.EXE upgrading an install on Windows 2000 while the VNC service ran
    failed at "Copy V9XVNC.EXE" (setup exit 1), for the same reason. On NT
    setup now runs the package's `V9XVNC.EXE -stop` first.
  Verified on the Windows 2000 VM (`vnc status/stop/start`, `update` with a
  running service, `SETUP.EXE /S` over a running service) and again on
  Windows 98 SE.

## 0.10.0 (released as part of 0.12.0)

### Added
- `reset` verb (`RESET_REQUEST` `0x0043` / `RESET_ACCEPTED` `0x8043`,
  capability `V9X_CAP_HARD_RESET` `0x00010000`, Windows 9x only): restarts a
  machine whose Windows restart hangs, without Windows' return to real mode.
  For the Velocity9x netbook, where the resident real-mode NDIS2 ethernet
  driver stops every restart at a flashing cursor or a fatal exception 0C.
  Same proof as `reboot` (persisted token, new boot counter, reconnect). After
  `RESET_ACCEPTED` the agent sends `WM_ENDSESSION` to other programs' top-level
  windows (no `WM_QUERYENDSESSION`, as `EWX_FORCE`), flushes the registry,
  takes a level-1 lock on every fixed drive (which commits VCACHE and leaves
  the FAT clean, so ScanDisk does not run), stops writing its own log, waits
  3 s for the drive to empty its write cache, then starts a full reset (a
  power cycle) through the reset control register: `0Ah`, then `0Eh` to
  `CF9h`. There is no fallback: if the machine is still running 8 s later
  the agent releases the locks, logs `reset-failed` and keeps running. Port
  `92h` INIT and the 8042 pulse were tried and dropped: the 8042 pulse goes
  through VKD into Windows' own restart path, and the netbook went dark
  until its power button was held after a hard reset (`02h`/`06h`) followed
  by `92h`. Verified on the netbook: it comes back on its own in about 30 s
  with no ScanDisk. Ring-3 access to `CF9h` was measured on Windows 98 SE on
  a PIIX4 VM and an ICH7 netbook with `tests\integration\portprobe.c`; the
  flush with `resetprb.c` and `flushprb.c`.

### Fixed
- The install ID no longer changes on every start when `AGENT.INI` has LF-only
  line ends. The Windows 9x profile writer appends a key to an LF-only
  section as `<CR>install_id=...<CR>`, which `GetPrivateProfileString` never
  reads back, so each start generated and appended another ID (a netbook whose
  `AGENT.INI` had been uploaded with `put` from an LF file held about 50).
  The agent and SETUP.EXE now repair `AGENT.INI` before reading it: CRLF line
  ends, blank-line runs collapsed, and only the first `install_id` kept (the
  one Windows returns), with the original saved as `AGENT.BAK` and
  `config-repaired` logged. A clean file is left alone. Reproduced and
  verified on a Windows 98 SE VM.
- `v9xctl put` and the MCP `v9x_put_file` tool warn (stderr and a `Warning` /
  `warning` result field) when an `.INI`, `.BAT`, `.CMD`, `.REG`, `.CFG` or
  `.INF` file is sent with LF-only line ends. The transfer stays byte-exact.

## 0.9.1 (2026-10-09)

### Changed
- `v9xctl update` also upgrades agents too old to hot-update (before 0.6.0).
  It used to judge that by capability `0x0200`, but 0.5.x advertises that bit
  too, so against 0.5.x it uploaded both binaries and then failed on
  `UNSUPPORTED_OPERATION` (exit 41). It now reads INFO `AgentVersion` first
  and, below 0.6.0, drives the staged route those agents support, with every
  guest path fixed and 8.3:
  copy the package to `C:\V9XPKG`, run its `INSTALL.BAT` there, check that
  `V9XNEW.EXE` is staged, reboot so `UPDATE.BAT` swaps it in from
  `RunServices`, confirm the agent now reports the package's version and
  nothing is left staged, then remove `C:\V9XPKG`. A session that improvised
  this by hand put the package in a long-named folder, and the boot-time step
  stopped at a prompt until someone pressed a key. The result carries
  `Mode = Staged`; failures exit 41 (staging) or 42 (did not apply, see
  `C:\V9XREMOTE\UPDATE.LOG`). It creates DOS VMs (`INSTALL.BAT`,
  `UPDATE.BAT`), which agents before 0.6.0 cannot avoid. Verified by staging
  0.5.2 to 0.9.1 on a Windows 98 SE guest. Host-side only; the agent is
  unchanged from 0.9.0 apart from its version.
- The MCP `v9x_update` tool checks the agent version the same way and refuses
  an agent before 0.6.0 before uploading anything, pointing at
  `v9xctl update`.

## 0.9.0 (2026-10-09)

### Changed
- Idle connections are closed: a client that sends nothing for
  `AGENT.INI [agent] idle_timeout` seconds (default 300, `0` never) is
  dropped and logged as `client-idle-timeout`, except while it owns a running
  EXEC. Before, a client that vanished without a FIN or RST reaching the guest
  held one of the four connection slots, and kept the tray icon busy, until
  the agent restarted. Applies to Win9x and NT; uses `select()` only, so the
  import table is unchanged.

### Added
- Windows 2000 support for the Win32 agent and `SETUP.EXE`. Windows 98
  behaviour is unchanged: every difference is chosen at run time from
  `GetVersion()` (`src/guest/platform.c`), and the import table is the same.
  - Autostart: NT ignores `RunServices`, so on NT `SETUP.EXE` and the
    hot-update swapper register `HKLM\...\CurrentVersion\Run` instead. The
    agent then starts at logon, so pair it with autologon. Over an existing
    install, `SETUP.EXE` stages the update by pointing `Run` at
    `V9XNEW.EXE -hotswap` rather than `UPDATE.BAT`. `SETUP.EXE /uninstall`
    clears both keys on NT.
  - Reboot and shutdown enable `SeShutdownPrivilege` first, which NT requires
    before `ExitWindowsEx` (also for SETUP.EXE's "Restart Windows now"). If
    it cannot be enabled the request fails with `ACCESS_DENIED` instead of
    being accepted and then doing nothing. The token calls are resolved with
    `GetProcAddress`, so nothing new is imported.
  - Shutdown on NT drops `EWX_POWEROFF` when the machine has no soft-off
    (`GetPwrCapabilities` reports no `SystemS5`, as with the "Standard PC"
    HAL and APM off), because that HAL turns a power-off request into a
    reboot. Such a machine halts at "It is now safe to turn off your
    computer" and the agent logs `shutdown-accepted-halt`. Win9x still
    always powers off.
  - Shell-mode EXEC runs `CMD.EXE /C` on NT, where `COMMAND.COM` is the
    16-bit interpreter under NTVDM. The wire format is unchanged.
  - Hot update: NT lets a running EXE be renamed, so on NT the swapper also
    waits for the old agent's singleton mutex to disappear before swapping.
  - Sockets are not inheritable on NT. NT Winsock sockets are kernel handles
    that children inherit by default, so the hot-update swapper (and any
    EXEC child) held the listening socket and the relaunched agent looped on
    `listen-failed` forever. Every agent socket now has its inherit flag
    cleared (`SetHandleInformation`, resolved at run time), and on NT the
    swapper relaunches the agent with no inheritance, which also recovers an
    agent built without the fix. The swap log says `run-set` on NT.
  - `INSTALL.BAT` and `UNINSTALL.BAT` stop on NT and point at `SETUP.EXE`.
  - INVENTORY reads the NT locations: `os` from `Windows NT\CurrentVersion`
    (`version_number` like `5.0.2195`, plus `product_type`), `bios` from
    `HARDWARE\DESCRIPTION\System` (plus `video_version`), and `display`
    `bpp` and `refresh` from `EnumDisplaySettingsA`. Devices are the present
    instances under `SYSTEM\CurrentControlSet\Enum`, with presence and
    problem codes from `CFGMGR32` (`meta devices=live`); an instance with no
    `Class` value is classified through its `ClassGUID`. `CFGMGR32` and
    `EnumDisplaySettingsA` are resolved at run time. The 9x records are
    unchanged, apart from `REG_MULTI_SZ` values now being read (joined with
    ` / `) where before they were skipped.
  - `allow_dos_vm=0` on NT refuses only a direct `exec` of a DOS or Win16
    program (anything that is not a PE image), since `CMD.EXE` and console
    EXEs start no VM there; `EXEC_SHELL` stays advertised. `SETUP.EXE` labels
    the option "Allow DOS and 16-bit Windows programs (NTVDM)" on NT.

## 0.8.1 (2026-10-07)

### Changed
- A guest `download` may fetch up to 2 GiB (`V9X_MAX_DOWNLOAD_SIZE`). It
  streams straight to the guest disk, so it no longer shares the 256 MiB
  `put`/`get` ceiling; a 256 MiB+ game archive failed with Win32 223
  (`ERROR_FILE_TOO_LARGE`) on 0.8.0. The wire format is unchanged.

### Added
- Relative mouse moves: INPUT opcode 7 (`s32 dx, s32 dy`), capability bit
  `V9X_CAP_INPUT_RELATIVE` (`0x8000`), `moverel DX,DY` in `v9xctl input`
  and `relative=true` on the MCP `v9x_move` tool. Games that read motion
  deltas and recentre the cursor (UT99's own menu cursor) never see an
  absolute `move`. Sent as `mouse_event` motion in steps of at most 4 pixels,
  below Windows' default acceleration threshold of 6.

## 0.8.0 (2026-10-01)

### Added
- `SETUP.EXE`, a graphical install wizard (Welcome, Options, Install,
  Finish), launched by `AUTORUN.INF` from the install CD. The Options page
  sets LAN discovery, DOS programs (`allow_dos_vm`), TCP port, listen address
  and allowed client, and shows the install ID. Over an existing install it
  keeps the settings and ID and stages the update for the next boot, exactly
  like `INSTALL.BAT`; run from `C:\V9XREMOTE` it only changes settings. It
  registers in Add/Remove Programs (`SETUP.EXE /uninstall`), and `/S` runs
  a silent install. `INSTALL.BAT` stays as the no-GUI path.
- Install ID: an RFC 4122 v4 UUID in `AGENT.INI [agent] install_id`,
  generated by SETUP.EXE or on the agent's first start, so a machine can be
  recognised after its IP changes. Win9x and DOS agents both have it. It is
  reported in INVENTORY `meta install_id`, and in INFO when the host asks
  for it: capability `INSTALL_ID` (0x4000) means INFO accepts a u32 flags
  payload, and flag 1 appends the ID after `allowed_client`. Older hosts
  send no flags and see the old reply, so nothing breaks either way.
- LAN discovery (Win9x agent): with `discovery=1` the agent runs an
  mDNS/DNS-SD responder (RFC 6762/6763, Winsock 1.1 multicast, no Bonjour)
  advertising `_v9xremote._tcp.local` with SRV, TXT (`id`, `ver`, `build`,
  `dos`, `os`, `name`) and A records, re-announcing when its address
  changes and sending a goodbye before a hot update or reboot. INVENTORY
  `meta discovery` says `off`, `on` or `unavailable`. Off by default in
  `AGENT.INI`; SETUP.EXE ticks it.
- `v9xctl discover` lists agents that answer, and a global `-Id <install id>`
  finds the target by discovery instead of `-Host/-Port`, and finds it again
  if it comes back from a reboot on a new address. `-QueryAddress` sends the
  query unicast to one address. The MCP server gains `v9x_discover` and
  `--id`, with the loopback guard applied to the address discovery returns.

## 0.7.1 (2026-09-26)

### Fixed
- `INVENTORY` lists only devices that are loaded now. It walks the
  Configuration Manager's live tree (`HKEY_DYN_DATA\Config Manager\Enum`)
  and follows each node's `HardWareKey` into `HKLM\Enum`, instead of reading
  every instance under `Services\Class`, which also keeps devices the machine
  had in the past. On a Velocity9x netbook 0.7.0 reported three display
  adapters, five disk drives and a dozen USB controllers, and named the
  stale "Standard PCI Graphics Adapter" as the display adapter; the `display`
  record now names the first working live Display node (the GMA 950).
  `device` records gain `id` (the hardware key, e.g. `PCI\VEN_8086&...`) and
  `problem` (the Configuration Manager problem code, when nonzero). A final
  `meta` record now always says `devices=live`, or `devices=registry` when
  there is no live tree and the old walk is used.

## 0.7.0 (2026-09-26)

### Added
- `INVENTORY` message (0x0070 -> 0x8070) and capability bit `INVENTORY`
  (0x2000): a hardware and software inventory read with calls that are safe
  on every 9x release. The reply is a list of typed records (`os`, `cpu`,
  `memory`, `drive`, `display`, `net`, `bios`, `device`, `meta`), each a set
  of string key/value fields, so fields can be added later without another
  protocol change. It covers the Windows product/build/sub-version, DirectX
  and IE versions, CPUID vendor/brand/family/model/stepping/features and a
  measured clock speed, physical and page-file memory, per-drive type, label,
  file system and size, the display adapter and resolution, host IP
  addresses, BIOS strings and the PnP device list (DriverDesc of the Display,
  Monitor, MEDIA, Net, disk controller, CD-ROM, modem, port, USB and input
  classes). Only local fixed disks are probed, with critical-error dialogs
  suppressed, so an empty floppy or CD drive or a dead network share never
  stalls or prompts. CPUs without CPUID (386/486) report `vendor=unknown`.
  The reply fits the 16 KiB response buffer; if it would not, the last
  record is `meta truncated=1`.
- `v9xctl inventory` prints it (grouped text, or `-Json`), and the MCP
  server has a `v9x_inventory` tool. The DOS agent does not advertise it.

## 0.6.4 (2026-09-25)

### Changed
- The per-file transfer limit is 256 MiB, up from 64 MiB, for `put`, `get`,
  `push-tree` and `download` on both the Windows and DOS agents
  (`V9X_MAX_FILE_SIZE`). The wire format is unchanged: sizes were already
  32-bit and every path streams 32 KiB chunks to disk, so nothing is buffered
  in guest memory. The PowerShell and MCP clients enforce the same ceiling.
  An older agent still rejects anything over 64 MiB with `LIMIT_EXCEEDED`
  (exit 40), so update the guest first.
- Hot update no longer runs a batch file. The agent used to write
  `HOTSWAP.BAT` and run it through `COMMAND.COM`, which creates a DOS virtual
  machine; on the Velocity9x netbook that corrupts the display every time.
  The staged `V9XNEW.EXE` is now launched with `-hotswap` and does the swap
  itself: wait for the lock, rename and copy, re-assert the RunServices
  entry with the registry API, relaunch. `HOTSWAP.LOG` keeps the same step
  names. The agent now imports ADVAPI32 for the registry write. The update
  *from* an older agent still goes through that agent's batch once.

### Added
- `allow_dos_vm=0` in `AGENT.INI` for guests whose display driver cannot
  survive a DOS VM. The agent then withdraws the `EXEC_SHELL` capability (so
  `shell` fails on the host with exit 23 without touching the guest) and
  refuses `exec` of anything but Win32 GUI-subsystem executables with
  `UNSUPPORTED_OPERATION`. Logged as `dos-vm-disallowed` at start. Default 1.

## 0.6.3 (2026-09-24)

### Added
- The notification-area icon now shows activity. Its body flashes lime while
  any client is connected or any execution is running, and a short hold keeps
  it flashing after the last frame so even a bare `ping` blinks once. The
  agent counts claimed connection slots and every frame it receives or sends;
  the tray worker samples those counters every 100 ms and swaps the icon at
  250 ms intervals. Nothing about the protocol changes.
- The icon itself is now drawn by the agent as a 16-colour icon resource in
  memory (`CreateIconFromResource`) instead of borrowing `IDI_APPLICATION`:
  the agent may not import GDI32, so this is the only way to have two colour
  variants of one picture. If icon creation fails the agent logs
  `tray-createicon-failed` and falls back to the stock icon without flashing.
- `scripts\v9xctl.ps1` explains itself. Running it with no verb, an unknown
  verb, or `help` prints the verb list, the connection options (`-Host`,
  `-Port`, `-ConnectTimeoutSeconds`) and worked examples instead of an
  interactive prompt for `Action`; a verb missing its required parameter
  prints that verb's usage line and an example. Comment-based help backs
  `Get-Help .\scripts\v9xctl.ps1 -Examples`, verbs tab-complete, and a
  connection failure now names the target it tried and how to override it.

## 0.6.2 (2026-09-04)

### Fixed
- The notification-area icon, which had never appeared on any guest in any
  release that shipped it. `v9x_tray_start` passed a NULL `lpThreadId` to
  `CreateThread`, which Windows 9x rejects with `ERROR_INVALID_PARAMETER`
  (Windows NT accepts it, so no modern host ever flagged the call). The window
  and icon were created fine; the worker thread never existed, so
  `Shell_NotifyIcon` was never reached. Measured as `tray-thread-failed gle=87`
  on 98 SE. The owner window now also belongs to the worker thread and that
  thread pumps its own message queue, instead of the window being created on
  the main thread that blocks in `accept()` forever.
- Tray failures are no longer silent. `v9x_tray_start` returned 0 for three
  different reasons behind one `tray-icon-failed` line, and a failing
  `Shell_NotifyIcon` was retried every second with its result discarded, so a
  wholly broken feature could not be diagnosed from the log. Each failure now
  names itself with its `GetLastError` (`tray-window-failed`,
  `tray-loadicon-failed`, `tray-thread-failed`, `tray-notify-failed`) and
  success logs `tray-icon-added`.

### Added
- `scripts\set-autologon.ps1`: report, enable or disable Windows 9x autologon
  on a guest, and clear a logon dialog that is already up (`-Dismiss`). Purely
  host-side, composed from existing verbs, no protocol change.
- `scripts\capture-emulator-window.ps1`: capture the emulator's own window from
  the host. `screenshot` needs Explorer, so it returns exit 43 for precisely
  the states worth seeing - a logon dialog, a BIOS prompt, a shutdown hang.
  This reads the window instead and needs nothing from the guest.
- Documented what the agent can and cannot do before anyone logs on.
  `RunServices` does start it before the logon dialog and everything except
  `screenshot` and `wait-desktop` works there, so a guest that prompts at boot
  stalls automation at its first `wait-desktop` while `ping` keeps succeeding.
  `AGENTS.md` now says to call `info` before concluding a guest is hung.

## 0.6.1 (2026-08-21)

### Fixed
- Windows 95 support. The agent imported `KERNEL32:InterlockedCompareExchange`,
  which Windows 95 does not export (it arrived in Windows 98 and NT4), so the
  loader refused to start `V9XAGNT.EXE` ("linked to missing export"). Every
  compare-exchange was a 0/1 flag acquire or a plain read, so they are now the
  `v9x_flag_acquire`/`v9x_flag_read` macros built on `InterlockedExchange`
  (exported since Windows 95) and an aligned volatile read. Verified on a real
  Windows 95 486; behaviour on 98 SE is unchanged.
- `packaging/` text files are CRLF on disk again and a `.gitattributes` pins
  them (`packaging/** text eol=crlf`, plus `*.BAT`/`*.REG`/`*.INI`/`*.CFG`).
  They had been LF-only in the working tree since 16 Aug, so anything copied
  straight from `packaging/` (bypassing `build-guest.ps1`, which normalises)
  produced batch files that Win9x `COMMAND.COM` misparses with "Bad command or
  file name" on every line.

## 0.6.0 (2026-08-18)

### Added
- Concurrent execution pool. The agent now runs up to four `EXEC_REQUEST`
  jobs at once instead of one; `V9X_STATUS_BUSY` is returned only when every
  slot is in use. `EXEC_STDOUT`/`EXEC_STDERR`/`EXEC_COMPLETE` are already keyed
  by request id, so streams interleave, and `CANCEL_REQUEST` is scoped to the
  owning connection and request id.
- Multiple simultaneous client connections. The listener serves each accepted
  socket on its own thread with its own send lock and buffers, up to four
  concurrent connections (excess are rejected). Reboot, shutdown, and
  screenshot remain machine-wide and are refused while any execution is active.
  Request ids are unique only per connection.
- Upgrade without reboot. `UPDATE_REQUEST` (`0x0042`) verifies the size and
  CRC32 of already-uploaded `V9XNEW.EXE`/`V9XSNEW.EXE`, writes `HOTSWAP.BAT`,
  launches it detached, replies `UPDATE_ACCEPTED`, and exits so the batch can
  swap the now-unlocked binaries and relaunch the agent. The RunServices entry
  is never repointed, so an interrupted swap still recovers on the next boot.
  Wires the previously reserved `V9X_CAP_DRIVER_UPDATE` capability bit.
- HTTP download. `DOWNLOAD_REQUEST` (`0x0027`) fetches an `http://` URL straight
  to a guest path over a hand-rolled HTTP/1.0 client (no new DLL dependency),
  streaming `DOWNLOAD_PROGRESS` and committing through the same transactional
  `.PART`/backup path as uploads. HTTPS is rejected with a clear error. New
  `V9X_CAP_HTTP_DOWNLOAD` capability bit.
- Structured audit log. `AGENT.LOG` lines now carry a local timestamp, the boot
  counter, and per-event detail (executed command, transferred path and CRC,
  download URL and HTTP status, and a full per-command dispatch trail). The log
  rotates to `AGENT.OLD` past 256 KB.

## 0.5.3 (2026-08-16)

### Fixed
- `INSTALL.BAT` no longer aborts on real Windows 98 SE hardware. It guarded its
  target directory with a bare `IF NOT EXIST C:\V9XREMOTE`, but Win98 SE
  `COMMAND.COM` evaluates `IF EXIST <directory>` as false for a directory that
  exists. The guard therefore fired on every run, `MD` failed with "Unable to
  create directory", and the install exited with "ERROR: V9x Remote Agent files
  could not be copied" without installing or staging anything. Both guards now
  test `C:\V9XREMOTE\NUL`. Found installing onto a Gateway Solo 2150.
- `INSTALL.BAT` applies its registry file before clearing read-only attributes,
  and clears them one named file at a time instead of sweeping
  `C:\V9XREMOTE\*.*`. The wildcard sweep also walked the `TEMP` and `JOBS`
  subdirectories and could fail with "General failure reading drive C", which
  aborted the batch *after* the new binaries were staged but *before*
  `UPDATE.REG` was applied - leaving a staged update that would never install
  and a machine that silently kept running the old agent.

## 0.5.2 (2026-08-13)

### Fixed
- Screenshot capture and all GDI screen probing now run in the disposable
  `V9XSHOT.EXE` helper. A display-driver GPF or blocked GDI call can no longer
  directly crash the network agent; the agent terminates a helper that has not
  completed within 7.5 seconds and returns a normal screenshot error.
- Capture is rejected unless Explorer's desktop is ready, and the helper
  refuses a mode whose dimensions change during a short stability sample.
- `V9XAGNT.EXE` no longer imports GDI32. HELLO and INFO use USER screen metrics
  and report bits-per-pixel as unknown (`0`); successful screenshot responses
  still report the source colour depth measured by the helper.
- The Win98 install and staged-update paths deploy the helper with the matching
  agent build.
- `INSTALL.BAT` validates the complete package before modifying the guest,
  checks every required copy, and removes both staged binaries if either copy
  fails. `UPDATE.BAT` installs the helper before activating the matching agent.

### Changed
- Recovery guidance now forbids screenshots during fullscreen mode changes or
  suspected display wedges. Use INFO, direct trace-dump execution, and file
  download first, and capture only after desktop readiness is re-established.
- `BootCounter` is documented as an agent-start counter, not independent reboot
  proof; reconnect, the persisted resume token, and desktop readiness are
  required before post-reboot GUI work.

## 0.5.1 (2026-08-12)

Fixes a live execution wedge: `shell -Command "start SETUP.EXE"` spawned a
detached child that kept the Windows 9x `COMMAND.COM` wrapper (and the
inherited pipe write handles) alive, so the finished request held the guest's
single execution slot for its full timeout and other connections were refused
for 60 to 120 seconds.

### Added
- Detached execution: `v9xctl.ps1 exec`/`shell -Detach` (MCP: `detach: true`
  on `v9x_exec`/`v9x_shell`) launches the child with all standard handles on
  `NUL` and completes immediately without capturing output or waiting. Use it
  instead of `START` for installers and other programs that outlive the
  request. Request option bit `0x0001`, completion flag `0x40`, capability
  bit `V9X_CAP_EXEC_DETACH` (0x800).

### Fixed
- Capture pipes are now created non-inheritable and the child receives
  inheritable duplicates of only the write ends (via `DuplicateHandle`), so
  agent-side read handles can no longer leak into a child's descendants.
- Shell mode no longer burns the whole timeout when the command finished but
  its DOS-VM wrapper lingers because of `START`-spawned or Win16
  descendants: once every pipe write handle has closed and the process
  handle is still unsignaled after a 2 s grace, the agent discards the
  wrapper and completes the request successfully with new completion flag
  `0x80` (`Orphaned`). Windows 9x has no job objects, so waiting on the
  descendant tree is impossible by design.

## 0.5.0 (2026-08-11)

First public release, focused on packaging the project for other people and
for AI coding agents, plus one new guest capability.

### Added
- A Windows notification-area icon now shows that the guest agent is running;
  its hover tooltip reports the agent version, listening port, and IPv4 address.
- Input injection: a new `INPUT_REQUEST` protocol operation and guest handler
  (`src/guest/input.c`) inject batched mouse and keyboard events (move, click,
  drag, wheel, key/hotkey, typed ASCII, delay), applied atomically after
  whole-batch validation. Exposed as `v9xctl.ps1 input -Sequence "..."` and
  MCP tools `v9x_click`, `v9x_move`, `v9x_type`, `v9x_key`, `v9x_scroll`, and
  `v9x_input`. This lets an agent drive dialogs and installers that only the
  GUI exposes; capability bit `V9X_CAP_INPUT_INJECT` (0x400) advertises it.
- MCP server (`mcp/v9x_mcp.py`): standard-library-only Python 3.9+, speaks
  the V9XR protocol natively (no PowerShell needed, so macOS/Linux hosts
  work), and exposes 20 tools including screenshots returned as PNG images
  the model can see. Offline test suite in `mcp/test_v9x_mcp.py`.
- Install CD builder (`scripts/make-install-media.ps1`): writes a plain
  ISO9660 image of the guest package with no external tools, so a fresh
  Windows 9x guest can be set up by mounting `V9XREMOTE.ISO` and running
  `INSTALL.BAT`. `-Validate` mounts the ISO on the host and verifies every
  file hash.
- Release tooling: `scripts/make-release.ps1` (zip + ISO + SHA256SUMS) and
  GitHub Actions workflows for CI and tagged releases with prebuilt
  binaries.
- Public documentation: rewritten `README.md`, `docs/quickstart-86box.md`
  (zero-to-working 86Box guide), `docs/ai-workflows.md`, `AGENTS.md`
  (operating manual for AI agents), `SECURITY.md`, and `CLAUDE.md`.

### Changed
- License changed from all-rights-reserved to BSD 2-Clause.
- `INSTALL.BAT` clears the read-only attribute after copying, so installs
  from CD media no longer leave read-only files that would break staged
  updates.
- The package version is stamped from `include/v9xremote/version.h` at build
  time, and the build fails if the changelog disagrees.
- The design plan moved to `docs/design.md` with environment-specific paths
  generalized.

## 0.4.2 (2026-08-10)

### Fixed
- Direct execution of GUI applications no longer hangs paint-driven programs.
  The agent previously always set `STARTF_USESHOWWINDOW` with `SW_HIDE` by
  default, which Windows substitutes into the child's first `ShowWindow`
  call; a hidden GUI window never receives `WM_PAINT`, so programs such as
  `V9XGDI.EXE /auto` idled until the execution timeout. Direct mode now reads
  the target's PE optional header and, for Windows GUI subsystem executables,
  leaves the initial window state to the application, matching launch via
  `START`. Shell mode and console-subsystem targets keep the previous
  hidden-by-default behaviour.
- `v9xctl.ps1 exec` accepts an empty or omitted `-Arguments` value, so
  programs that take no arguments can be launched in direct mode.

### Added
- New `EXEC_COMPLETE` flag `0x20` (`V9X_EXEC_FLAG_GUI_WINDOW`) reports when
  the GUI-window rule was applied; `v9xctl.ps1` surfaces it as `GuiWindow`.
- `CHANGELOG.md` (this file).

### Changed
- The guest package is now written to `build\install`, which always holds the
  most recently built version. The old `build\package` default and the
  accumulated per-milestone package folders were removed, and the docs now
  point at `build\install`.

## 0.4.1 (2026-08-09)

### Added
- Physical-machine support: configurable IPv4 listen address and TCP port,
  plus a single-client IPv4 allowlist for isolated development networks, all
  read from `C:\V9XREMOTE\AGENT.INI`. `HELLO` and `INFO` report the configured
  values. See `docs/physical-machine.md`.

## 0.4 (2026-08-09)

### Added
- Controlled reboot and shutdown with persisted resume tokens
  (`PENDING.DAT`) and boot-counter proof, so the host can verify that the
  intended boot completed.
- Desktop readiness reporting for post-boot sequencing.
- CRC32-verified 24-bit BMP screenshot capture and transfer.
- `run-driver-cycle.ps1` host orchestration for the Velocity9x
  build/install/reboot/test loop.

## 0.3 (2026-08-09)

### Added
- Initial public feature set consolidated from the M1-M3 development builds:
  framed TCP protocol with HELLO/PING/INFO, direct and `COMMAND.COM /C` shell
  execution with bounded piped output, timeouts and cancellation, and
  CRC32-verified file stat/list/mkdir/upload/download with `.PART` staging
  and rollback backups.
- Windows 98 packaging with install, staged-update, and uninstall scripts
  registered through `RunServices`.
