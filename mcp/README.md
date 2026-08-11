# V9x MCP server

`v9x_mcp.py` exposes the Windows 9x guest as MCP tools over stdio. Standard
library only, Python 3.9+, works on Windows, macOS, and Linux (unlike the
PowerShell CLI, which needs Windows). It speaks the V9XR wire protocol
directly; PowerShell is not involved.

## Setup

Claude Code:

```bash
claude mcp add v9x -- python /path/to/repo/mcp/v9x_mcp.py
```

Any other MCP client (generic stdio configuration):

```json
{
  "mcpServers": {
    "v9x": {
      "command": "python",
      "args": ["/path/to/repo/mcp/v9x_mcp.py"]
    }
  }
}
```

Options: `--host` (default `127.0.0.1`), `--port` (default `9869`),
`--timeout` (socket timeout seconds). A non-loopback `--host` is refused
unless you also pass `--allow-remote`, because the guest protocol is
unauthenticated (see [../SECURITY.md](../SECURITY.md)).

## Tools

| Tool | Purpose |
|---|---|
| `v9x_ping` | Liveness, uptime, boot counter |
| `v9x_info` | Full agent status (version, screen mode, desktop readiness...) |
| `v9x_exec` | Run a Win32 EXE with captured stdout/stderr and a timeout |
| `v9x_shell` | Run a `COMMAND.COM /C` command (DIR, batch files, VER...) |
| `v9x_put_file` | Upload a local file, CRC32-verified |
| `v9x_get_file` | Download a guest file, CRC32-verified |
| `v9x_push_tree` | Recursively upload a directory |
| `v9x_stat` | Existence/size/attributes of a guest path |
| `v9x_list_dir` | Directory listing |
| `v9x_mkdir` | Idempotent directory creation |
| `v9x_screenshot` | Capture the guest screen, returned as a PNG image block the model can see |
| `v9x_click` | Click a mouse button, optionally moving to (x,y) first; `double` for double-click |
| `v9x_move` | Move the cursor to absolute screen coordinates |
| `v9x_type` | Type ASCII text into the focused window |
| `v9x_key` | Press a key or hotkey combo, e.g. `CTRL+ESC`, `ALT+F4` |
| `v9x_scroll` | Scroll the mouse wheel |
| `v9x_input` | Raw ordered action batch for drags and precise sequences |
| `v9x_reboot_with_proof` | Reboot and verify via boot counter + echoed resume token |
| `v9x_shutdown` | Clean shutdown |
| `v9x_wait_desktop` | Block until the Windows desktop is ready |

Every tool opens a fresh connection, so a crashed VM never wedges the server;
errors come back as tool results with `isError` set.

The screenshot tool converts the agent's 24-bit BMP to PNG in-process (also
standard library only) and returns it as an MCP image content block, so the
model can actually look at the Windows 98 screen. Pass `save_path` to keep a
copy on disk.

## Typical agent loop

1. `v9x_ping`, then `v9x_wait_desktop` after any boot
2. `v9x_mkdir` a job directory under `C:\V9XREMOTE\JOBS\`
3. `v9x_put_file` / `v9x_push_tree` the freshly built binaries
4. `v9x_exec` with an explicit `timeout_ms`, read the JSON result
5. `v9x_get_file` any result files; `v9x_screenshot` to look at the screen
6. `v9x_reboot_with_proof` + `v9x_wait_desktop` when the change requires it

Conventions and failure-handling rules are in [../AGENTS.md](../AGENTS.md).

## Tests

```bash
python -m unittest discover -s mcp -p "test_*.py" -v
```

The tests are offline: frame and payload codecs, the BMP to PNG converter,
and the JSON-RPC handshake, none of which need a running guest.
