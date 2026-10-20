# Windows 98 integration checklist

1. Configure an 86Box SLiRP forward to guest TCP 9869 and protect it with the
   host firewall.
2. Copy `build\install` to the guest and run `INSTALL.BAT`.
3. Reboot and run `v9xctl.ps1 ping` and `info -Json` on the host.
4. Disconnect and repeat both commands to prove the accept loop survives.
5. Reboot and confirm the reported boot counter increases.
6. Repeat for twenty reboot/reconnect cycles before beginning Milestone 2.

## Verified milestones

- 2026-08-09: Windows 98SE reported `4.10.2222`; HELLO/PING/INFO passed.
- 2026-08-09: direct and `COMMAND.COM` execution plus pipe capture passed.
- 2026-08-09: create, upload, replacement, stat, list, download, CRC32, and
  recursive tree push passed with agent build `codex-20260809-m3`.

The `reset` verb was designed from three GUI-subsystem probes, built with the
normal Watcom CRT (commands in each file's header): `portprobe.c` (read-only:
which ports the System VM traps), `flushprb.c` (which VWIN32 calls flush a
volume) and `resetprb.c` (resets the machine; disposable VMs only;
`full` mode is the full reset the agent uses). Results are
in `docs/decisions/2026-10-12-reset-without-real-mode.md`.

`V9XVNC.EXE` is checked with `vnc_smoke.py`, an RFB client that needs no
viewer. It decodes Raw and Hextile frames at 8, 16 or 32 bpp and writes a PNG.
It can compare the frame with a `v9xctl screenshot` BMP and send pointer and
key events, and `--watch` reports DesktopSize changes. `modeset.c` switches
the resolution for a few seconds to drive that path (commands in each file's
header). Run against Windows 98 SE on 2026-10-15, it showed:

- a 99.97% or better pixel match with the agent's screenshot at every depth
  (the remainder is the cursor, which the screenshot omits);
- RFB 3.3, 3.7 and 3.8;
- clicks, typing, Ctrl and arrow keys, and release on disconnect;
- 1024x768 to 800x600 and back.

Further `vnc_smoke.py` options:

- `--encoding zrle` decodes ZRLE through Python's zlib.
- `--copyrect` offers CopyRect.
- `--clip-send TEXT` sends viewer clipboard text, and `--clip-expect TEXT`
  with `--watch` waits for the server's clipboard text.
- `--password PW` answers VNC authentication through
  `build\host-tests\test_vnc.exe auth`, which uses the server's tested DES;
  run `build-host-tests.ps1` first.

`vnc_smoke.py --rich-cursor --cursor-out cur.png` also checks the RichCursor
and PointerPos pseudo-encodings. `vnc_stall.py <agent port> <vnc port>`
drives the watchdog with the `TEMP\VNCSTALL.DAT` hook. It needs VNC enabled
and running, plus `shell`, and it leaves the server stopped.

To re-run the negative file-integrity check against an existing uploaded file:

```powershell
.\tests\integration\Test-LiveFileSafety.ps1 `
    -ExpectedFile .\README.md `
    -GuestPath C:\V9XREMOTE\JOBS\M3TEST\README.MD `
    -GuestDirectory C:\V9XREMOTE\JOBS\M3TEST
```
