# Claude Code notes

Read [AGENTS.md](AGENTS.md) first: it is the operating manual for driving the
Windows 9x guest (verbs, exit codes, conventions, the golden loop).

## Commands

```powershell
.\scripts\build-guest.ps1 -BuildId dev-001      # build agent + assemble build\install
.\scripts\build-guest-dos.ps1 -BuildId dev-001  # build the DOS agent -> build\dos-install
.\scripts\build-host-tests.ps1                  # native protocol test + host suites
.\scripts\make-install-media.ps1 -Validate      # build build\V9XREMOTE.ISO from build\install
.\scripts\v9xctl.ps1 <verb> ...                 # drive the guest (see AGENTS.md)
.\scripts\set-autologon.ps1 -Dismiss            # Win9x autologon; clear a logon dialog
.\scripts\capture-emulator-window.ps1 <vm> -OutFile shot.png   # host-side window grab
.\scripts\make-release.ps1                      # build\release: zip + ISO + SHA256SUMS
.\scripts\publish-github.ps1 -Message "..." -Tag  # sanitized snapshot -> public repo
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
- A DOS target lives in `src\guest-dos\` (DOS/4GW + Watt-32, needs `WATT_ROOT`).
  It reuses `include\v9xremote\` and `src\common\` and speaks the same protocol,
  but keeps only exec + file transfer; it uses the normal Watcom DOS CRT (no
  import audit). See `docs\dos-agent.md`.
- `build\install` is the canonical install package; `build\` is gitignored.
- The wire protocol is documented in `docs\protocol.md`; the PowerShell
  encoder/decoder is `scripts\V9xProtocol.ps1`, the Python one is
  `mcp\v9x_mcp.py`. Changes must keep all three in sync.
- Windows PowerShell 5.1 syntax only in scripts (no `&&`, no ternary).

## Publishing (this is a mirrored repo, treat it as special)

This folder lives inside a larger private repository, which stays primary. The
public repo is a mirror that **never receives history**: one squashed snapshot
commit per release. Publish only when explicitly asked.

```powershell
.\scripts\make-release.ps1                                # artifacts first
.\scripts\publish-github.ps1 -Message "Release 0.6.2" -Tag
```

- `publish-github.ps1` robocopy-mirrors the working tree (excluding `.git`,
  `.claude`, `build`, `__pycache__`), then a **hard scrub gate** refuses to
  publish if any mirrored text file contains the private workspace path, the
  user-profile root, or the owner's personal name. When it trips, fix the
  source file and re-run; never bypass it. This file is mirrored too, which is
  why nothing here quotes those strings, the public repo URL included.
- Snapshot message convention: `Release <version>`, nothing else. The script's
  default message (`v9x-remote-agent <version>`) does **not** match it, so
  always pass `-Message`. `-Tag` creates and pushes `v<version>`. The
  "V9x Remote Agent" that appears against each public commit is the mirror's
  author identity, not part of the message.
- A fresh mirror clone carries no committer identity and the script does not
  set one, so the snapshot would be attributed to the local git identity. Set
  it on the mirror before the first commit:
  `git -C build\github-mirror config user.name "V9x Remote Agent"` and
  `... user.email "v9x-remote-agent@users.noreply.github.com"`.
- If git rejects the mirror directory for dubious ownership (an older mirror
  created under a different local account), delete `build\github-mirror` and
  let the script re-clone it with `-Remote <public repo URL>`, which the old
  mirror's `.git\config` still holds. Prefer that over a `safe.directory`
  exception.
- The accompanying GitHub release is named `V9x Remote Agent v<version>`, its
  body is the `CHANGELOG.md` sections added since the last public release, and
  it attaches exactly the three `build\release` artifacts
  (`v9xremote-<version>.zip`, `V9XREMOTE.ISO`, `SHA256SUMS`).

## Working documents

The parent workspace's `CLAUDE.md` convention applies here: dated
`docs\decisions\`, `docs\issues\` and `docs\handoffs\` files, each with a `Status:`
line as its second line, created on first use and linked from `README.md`.
(That path is deliberately not written out: this file is published, and the
publish scrub gate below rejects it.)

The existing `docs\*.md` files (`protocol.md`, `design.md`, `host-cli.md`,
`guest-agent.md`, `dos-agent.md`, `ai-workflows.md`, `physical-machine.md`,
`quickstart-86box.md`) are stable reference material, not working documents. They stay
where they are and are exempt from the `Status:` line.

This is the one folder large enough to justify `docs\plans\<topic>.md` over a single
root `PLAN.md` if more than one effort is ever live at once.
