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

To re-run the negative file-integrity check against an existing uploaded file:

```powershell
.\tests\integration\Test-LiveFileSafety.ps1 `
    -ExpectedFile .\README.md `
    -GuestPath C:\V9XREMOTE\JOBS\M3TEST\README.MD `
    -GuestDirectory C:\V9XREMOTE\JOBS\M3TEST
```
