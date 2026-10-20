# Live remote control: a separate RFB (VNC) server helper

Status: accepted; phases 1-4 complete, verified on Windows 98 SE and Windows 2000 VMs (2026-10-15)
Date: 2026-10-15

## Context

The agent can take one screenshot at a time (`SCREENSHOT_REQUEST`, written by
the disposable `V9XSHOT.EXE` helper) and inject batches of input
(`INPUT_REQUEST`). That is enough for scripted GUI checks. It is not enough to
use a machine interactively: there is no live picture, and every click is
a round trip through `v9xctl`. Inside 86Box the emulator window covers this.
On physical machines, the Velocity9x netbook first, nothing does.

The goal is VNC-style control: watch the screen continuously and drive the
keyboard and mouse live.

## Options considered

1. **Off-the-shelf VNC server** (TightVNC 1.3.x / RealVNC 3.3.x, both of which
   shipped for Windows 9x). Needs no code. But their hook DLLs and polling are
   untested against Velocity9x, and they are an opaque extra process on a
   machine whose display driver is under active development. They also have
   no place in this project's capability bits, verbs, MCP tools or install
   media.
2. **Our own RFB server as a separate guest helper.** Chosen.
3. **Stream frames over the v9x protocol.** Rejected. It would need a viewer
   written and maintained here, plus the stream format kept in sync across
   the C, PowerShell and Python codecs. RFB gets every existing viewer for
   free: TigerVNC, TightVNC, and noVNC in a browser.

A remote-control host for modern Windows was built separately (DXGI
duplication, WebRTC, secure-desktop switching). Most of its complexity does not
apply to Windows 9x, which has no session 0, no Winlogon secure desktop and no
DXGI. Two ideas carry over: reconcile modifier keys, and release every held
key and button when a viewer disconnects.

## Decision

Build `V9XVNC.EXE`, a long-lived RFB server that is separate from the agent
and listens on TCP 5900.

- **Process boundary.** It stays out of the agent for the same reason as
  `V9XSHOT.EXE` ([design.md](../design.md) section 18.3): a broken display
  driver can hang GDI, and the network agent has no GDI32 import. The agent
  starts it (`exec` detach), stops it, and stays reachable for `info`, `reset`
  and file transfer when the screen wedges.
- **Toolchain.** C89, Open Watcom, runtime-free, with its own import allowlist
  in `scripts\build-guest.ps1`, like the other guest binaries.
- **Protocol.** Standard RFB 3.3 and 3.8, so `docs\protocol.md` needs only a
  short pointer and its three codecs stay untouched. The agent advertises the
  helper with a new capability bit.
- **Security.** No authentication, consistent with the rest of the project
  (security type None). Containment stays with the sandbox network and the
  host firewall. RFB's VNC authentication (a DES challenge, 8-character
  password) may be added as an option, but it is not real security and the
  docs must say so.

## Design

**Capture.** `GetDC(0)` + `BitBlt` into a DIB section at the screen's native
depth, polled at a configurable rate (target 2-10 fps). Each poll is compared
with the previous frame in 16x16 tiles, and only dirty tiles are sent, merged
into rectangles. There is no mirror driver and no hook DLL.

**Pixel format.** The server must honour the client's `SetPixelFormat`, so it
converts from 8, 16, 24 and 32 bpp sources to any true-colour client format.
An 8 bpp source uses the system palette, sent as `SetColourMapEntries` or
expanded to true colour. This is the fiddliest part.

**Encodings.** Raw and Hextile first; neither has a dependency. Then the
`DesktopSize` pseudo-encoding: driver work changes resolution and depth in the
middle of a session, so on `WM_DISPLAYCHANGE` the server rebuilds its DIB and
tells the viewer. CopyRect and ZRLE come later; ZRLE needs an embedded inflate
library.

**Cursor.** Drawn into the frame, or sent through the Cursor pseudo-encoding.
`GetCursorInfo` exists on Windows 98 and later, not on Windows 95, so Windows 95
falls back to `GetCursor` + `GetCursorPos`.

**Input.**
- `PointerEvent` maps to absolute `mouse_event` (0-65535 scaled), with button
  state diffed against the previous event. This reuses the approach already in
  `src\guest\input.c`; there is no `SendInput` on Windows 95.
- `KeyEvent` maps X11 keysyms to virtual-key codes through a static table,
  with modifier tracking.
- When a viewer disconnects, the server releases every key and button it holds
  down.

**Clipboard (optional).** `ClientCutText` / `ServerCutText` to and from
`CF_TEXT`, Latin-1 only.

**Clients.** One active viewer at first. Shared or view-only sessions can come
later.

**Watchdog.** Every GDI call runs on a capture thread, and the network thread
times it. If capture stalls, the server stops sending updates instead of
blocking the socket, and the agent can kill and restart the helper.

## Plumbing

- `V9XVNC.EXE` in `build\install`, on the ISO, and installed by SETUP.EXE.
  Optional autostart is set in `AGENT.INI`.
- `v9xctl vnc start|stop|status` and a matching MCP tool.
- Capability bit in `include\v9xremote\protocol.h`, added to the advertised
  set only when the helper is present.
- Host fixture test: a scripted RFB handshake and framebuffer update decoded
  against a recorded or synthetic stream (`tests\host\`). Integration test
  against a live guest in `tests\integration\`.
- Version bump, CHANGELOG, `docs\guest-agent.md` and `docs\host-cli.md`.

## Phases

1. **MVP.** RFB 3.3/3.8, security None, Raw + Hextile, full-screen polling
   with tile diff, pixel-format conversion, pointer and key events, and
   release-all on disconnect. Verified on Win98SE-Native-S3, then on the
   netbook.
2. DesktopSize and mode changes, the cursor pseudo-encoding, the capture
   watchdog, and `v9xctl`/MCP verbs.
3. Clipboard, CopyRect/ZRLE, optional VNC authentication.
4. Windows 2000. Following the input desktop (`OpenInputDesktop` /
   `SetThreadDesktop` on a thread that owns no windows) to reach the
   Ctrl+Alt+Del and logon screens. This is where the modern host's lessons
   apply directly.

## Phase 1 results (2026-10-15)

Built as `src\guest\vnc.c` (one file, about 1,200 lines, a 20 KiB EXE).
Tested on Win98SE-Native-S3 (86Box, S3, 1024x768, SLiRP forward host 5970
to guest 5900) with `tests\integration\vnc_smoke.py`:

- **Frames.** Every full frame matched the agent's own screenshot to 99.95%
  or better at 32, 16 and 8 bpp. The remainder is the drawn cursor, which
  `V9XSHOT.EXE` leaves out. That held with Hextile and with Raw, over RFB
  3.3, 3.7 and 3.8.
- **Speed.** A full 1024x768 Hextile frame took about 1.2-1.9 s through
  SLiRP, against 6.8 s for Raw, with the Python decoder included in both.
- **Input.** A click on Start opened the menu, and the incremental updates
  carried only the changed tiles (6 and 23 rectangles). Typing into Notepad
  worked, including shifted symbols sent without a Shift event and Shift held
  the way a viewer sends it. Ctrl+Home and the arrow keys worked. A session
  dropped with Shift held left nothing stuck.
- **Mode change.** `tests\integration\modeset.c` switched 1024x768 to
  800x600 and back; the watching client got both DesktopSize changes and a
  correct frame afterwards.
- **Lifecycle.** `-stop` worked, the single-instance guard worked (exit 3),
  and the agent stayed responsive throughout.

Deviations from the plan:

- DesktopSize and the drawn cursor came forward from phase 2; both were small.
- The host fixture test became the live-guest client above. A recorded-stream
  fixture is still worth adding once the encoder changes again.
- There is no `v9xctl vnc` verb or MCP tool yet: start and stop go through
  `exec`.
- `v9xctl update` does not replace `V9XVNC.EXE`; SETUP.EXE and `INSTALL.BAT`
  do, stopping a running server first.

Not yet tried: a real viewer (TigerVNC, RealVNC) and the netbook. A viewer
that connects while another session is open waits in the listen backlog
until the first one ends.

## Phase 2 results (2026-10-15)

**Off by default** (the owner's request, made after phase 1). The server
exits 9 unless `[vnc] enabled=1`, and nothing on the host side can turn it
on. That is deliberate even though the agent could write `AGENT.INI`
itself: enabling screen access is the machine owner's decision, so it sits
in SETUP.EXE.

- **SETUP.EXE.** An "Enable VNC remote control" checkbox on the Options page,
  unticked by default, with `/vnc` and `/novnc` for silent installs. Ticked,
  setup writes `enabled=1`, adds `V9xRemoteVnc` to `HKLM\...\Run` (Run, not
  RunServices, on Windows 9x too, since it needs a desktop) and starts the
  server. Unticked, or on uninstall, it removes the value and stops a running
  server. It first launched the server with `WinExec`, which on Windows 9x
  waits for the child to read its message queue. V9XVNC.EXE never does, so
  setup hung for about 30 s; it now uses `CreateProcessA`.
- **`-status`.** `v9xctl vnc status|start|stop` and the MCP `v9x_vnc` tool
  are built on `get` (`AGENT.INI`) and `exec` (`V9XVNC.EXE -status|-stop`),
  so the agent's protocol has no new message and no capability bit. Exit 3
  from `-status` also counts as running: the phase 1 build did not know
  `-status`, tried to serve, and hit its single-instance guard.
- **`update`.** `v9xctl update` and MCP `v9x_update` replace V9XVNC.EXE
  beside the hot swap: stop it, upload, and restart it only if it was
  running.
- **Watchdog.** Every GDI call runs on one worker thread. The network thread
  never waits on a capture: past 5 s it logs `capture-stalled`, keeps
  handling input, sends no frames, and resumes when the capture returns.
  Capture setup and teardown wait 15 s; then the server releases every held
  key and button and exits 10 (`gdi-wedged-exit`).

Verified on Win98SE-Native-S3:

- the wizard checkbox, `/S /vnc` (33 ms) and `/S /novnc`, with the Run value
  checked by exporting it with REGEDIT;
- exit 9 and `vnc start` exit 23 while disabled;
- the `vnc` stop/start cycle;
- both update paths, while the server was running;
- the frame comparisons, a click, and the resolution change, rerun on the
  worker-thread build.

Then, to finish phase 2:

- **RichCursor and PointerPos.** The cursor shape comes from drawing it over
  black and over white: equal results are opaque, black over white is
  transparent, and anything else is an inverting pixel, shown opaque black.
  With RichCursor the frame matched the cursor-less agent screenshot exactly
  (1.0000), against 0.9997 with the cursor drawn in. The I-beam came through
  with its hotspot at 15,16, and a guest-side move arrived as PointerPos.
- **Second viewer.** While a session is open the server also watches the
  listener and answers a second connection with RFB 3.3's connection-failed
  reply and a reason. A test client that kept speaking 3.8 to that 3.3 reply
  misread the reason; real viewers drop to the server's version.
- **Watchdog, verified.** A test hook makes the worker block while
  `TEMP\VNCSTALL.DAT` exists. `tests\integration\vnc_stall.py` then showed:
  - during a capture stall, a VNC pointer event still moved the guest
    cursor, and no frame was sent;
  - `capture-stalled` was logged at 5 s, and `capture-resumed` when the file
    went;
  - a new session whose setup stalled was dropped after 15 s, and the server
    exited (`gdi-wedged-exit`).

  A real driver wedge is still untested; the hook only stands in for one.

## Phase 3 results (2026-10-15)

- **ZRLE** needed a deflate encoder, because the guest build has no C runtime
  and no zlib. `src\guest\vncz.c` uses fixed Huffman codes only, with greedy
  LZ77, one hash candidate and no references across rectangles. It ends
  every rectangle with a sync flush, as ZRLE's single stream needs. That is
  enough here because ZRLE's own palette and run-length coding has already
  removed most of the redundancy.
  - Python's zlib inflates the host test cases back exactly, as one stream.
  - Decoding the VM's screen gave pixel-identical frames to Hextile at 32,
    16 and 8 bpp.
  - A full frame of the test desktop was 20 KB, against 104 KB for Hextile.
- **CopyRect** covers only the case that is cheap to detect: the foreground
  window moving at the same size. Scrolling is not detected. On Windows 98
  with the default outline drag, the window moves on release, so a drag is
  one CopyRect. After two drags the viewer's frame matched the screenshot
  except for the caret and the clock.
- **Clipboard.** It is polled rather than chained: a clipboard viewer chain
  depends on every member pumping messages. It runs on the GDI worker, which
  now waits with `MsgWaitForMultipleObjects`, because the hidden owner
  window must answer `WM_DESTROYCLIPBOARD` when another program empties the
  clipboard. Echo is avoided by hashing.
- **Password.** `src\guest\vncdes.c` passes the FIPS known answers in the
  host test. The test client answers the challenge through that same tested
  code (`test_vnc.exe auth`) rather than a second DES. The password is
  stored in plain text in `AGENT.INI`; obfuscating it would protect nothing
  while the agent that can read the file has no authentication.

Not done: TightVNC's Tight encoding, scroll detection for CopyRect, and
clipboard formats other than text.

## Phase 4: Windows 2000 (2026-10-15)

On Windows 2000 the server works on the normal (Default) desktop straight
away. The hard part is the Winlogon **secure desktop** — the lock / "Unlock
Computer" dialog, the logon screen, a screen saver's password prompt. Only
LocalSystem can open it, so a server started from the Run key as the user
freezes on its last Default-desktop frame when the screen locks (confirmed:
capture timed out after `LockWorkStation`).

So on NT the server runs as a **service** (`vncsvc.c`):

- `-install` creates `V9xRemoteVnc` as `SERVICE_WIN32_OWN_PROCESS |
  SERVICE_INTERACTIVE_PROCESS`, LocalSystem, auto-start, and starts it;
  `-uninstall` removes it; `-service` is the SCM dispatch entry. All through
  `GetProcAddress`/`LoadLibrary`, so the one binary still loads on 9x (the
  import audit for V9XVNC gained nothing). SETUP.EXE runs `-install` /
  `-uninstall` on NT instead of writing the Run key.
- The capture thread and the input (network) thread each call
  `OpenInputDesktop` + `SetThreadDesktop` before use, switching when the
  input desktop's name changes. `SetThreadDesktop` fails on a thread that
  owns a window, which is why phase 4 first moved the clipboard (and its
  hidden owner window) off the capture thread onto its own thread. A switch
  forces a full-screen read.

Verified on the Windows 2000 SP4 VM:

- the normal desktop over VNC (frame, cursor, input);
- the user-context server freezing at the lock screen (the problem);
- the service capturing the "Unlock Computer" dialog, and typing into its
  password field — both the secure desktop, which the user-context server
  could not touch;
- `desktop-switch` logged on the lock and unlock;
- SETUP.EXE `/S /vnc` installing and starting the service, and the
  auto-start service capturing the lock screen after a clean reboot.

Not supported: sending Ctrl+Alt+Del from a viewer. There is no pre-Vista
API (`SendSAS` is Vista+), and the "Press Ctrl+Alt+Del to begin" screen thus
cannot be passed from the viewer. Autologon skips it, which these VMs use.
Windows NT 4.0 and XP are untested.

## Speed with a real viewer (2026-10-15)

RealVNC Viewer 7.15 worked against the VM on the first try, but was "very
slow". It chose ZRLE at 8 bpp. New `perf-*` timing in `VNC.LOG` showed
where the time went, over 10 s of typing into Notepad:

| | Full read per poll | Sampled capture |
|---|---|---|
| Polls | 10-12 | 98 |
| Read the screen | 7.8-9.4 s (~780 ms each) | 5.3 s (~54 ms each) |
| Diff | ~0.9 s | 0.05 s |
| Encode / send | 0.4 s / 0.02 s | 0.04 s / 0.002 s |

The guest is an emulated 200 MHz Pentium MMX with an S3 ViRGE at 16 bpp, so
reading video memory back through GDI dominates. The network did not
matter.

The fix follows x11vnc's scanline polling. Each poll reads one row per
16-row band, rotating through all 16 rows in bit-reversed order, so a
change no sample touched is still found within 16 polls. Only the tiles
that look changed are then read in full, and the diff checks only tiles
that were read.

After a dozen lines of typing, the viewer's frame still matched the guest's
screenshot except for the caret and the clock. The stall and
resolution-change tests passed again.

Along the way the test guest ran out of a Windows 98 system resource:
every `CreateProcess` failed with error 4. This came after a long run of
test processes and server restarts. A reboot cleared it. Nothing showed
V9XVNC.EXE itself leaking, but a long session on a real machine is worth
watching.

## Open questions

- Frame rate and CPU cost on period hardware. Bandwidth through the netbook's
  NDIS2 stack. Measure in phase 1 before choosing a default poll rate.
- Whether `BitBlt` from the screen DC is reliable under Velocity9x in every
  mode it supports, or needs a per-mode fallback.
- Whether the helper should run before logon (the agent's pre-logon rules in
  [2026-09-04-prelogon-reachability-and-autologon.md](2026-09-04-prelogon-reachability-and-autologon.md)).
