# Claude Code notes

Read [AGENTS.md](AGENTS.md) first: it is the operating manual for driving the
Windows 9x guest (verbs, exit codes, conventions, the golden loop).

## Commands

```powershell
.\scripts\build-guest.ps1 -BuildId dev-001      # build agent + assemble build\install
.\scripts\build-host-tests.ps1                  # native protocol test + host suites
.\scripts\make-install-media.ps1 -Validate      # build build\V9XREMOTE.ISO from build\install
.\scripts\v9xctl.ps1 <verb> ...                 # drive the guest (see AGENTS.md)
```

Host tests that need no live guest: `tests\host\Test-*.ps1` (fixture-based)
and the native `test_protocol.exe` built by `build-host-tests.ps1`.
`tests\integration\` needs a live guest.

## Facts

- Version source of truth: `include\v9xremote\version.h`
  (`V9X_AGENT_VERSION`). The build stamps the package README from it and
  fails if the top `CHANGELOG.md` entry disagrees.
- Guest code is C89 built with Open Watcom (`WATCOM` env or `C:\WATCOM`),
  runtime-free; the build audits imports, so new Win32 calls must be added to
  the allowlist in `scripts\build-guest.ps1`.
- `build\install` is the canonical install package; `build\` is gitignored.
- The wire protocol is documented in `docs\protocol.md`; the PowerShell
  encoder/decoder is `scripts\V9xProtocol.ps1`, the Python one is
  `mcp\v9x_mcp.py`. Changes must keep all three in sync.
- Windows PowerShell 5.1 syntax only in scripts (no `&&`, no ternary).
