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

`--id <install id>` finds the guest by mDNS discovery instead of
`--host/--port` (agent 0.8.0+ with `discovery=1`, on a LAN the host shares
with the guest). The server looks the ID up again whenever a connection fails
and while waiting out a reboot or update, so it follows a guest onto a new
DHCP address. The loopback guard applies to whatever address discovery
returns, so a LAN guest still needs `--allow-remote`.
`--query-address ip[:port]` sends the query unicast, and `--discover-seconds`
(default 3) sets how long to listen.

Remote help (agent 0.12.0+, [../docs/remote-help.md](../docs/remote-help.md)):
every tool that talks to an agent takes `relay: "M-XXXXXX"` and then drives
that lent machine through the update server; `--relay M-XXXXXX` makes one
machine the default, and `--relay-server` (or `V9X_RELAY_SERVER`) picks the
server (default `https://michaeldale.com.au`). `v9x_relay_machines` lists the
lent machines and `v9x_chat` talks to their owners. Commands are signed by
`build\tools\v9xsign.exe` with `%USERPROFILE%\.v9x\relay-signing.key`; the
operator key comes from `%USERPROFILE%\.v9x\operator.key` or
`V9X_OPERATOR_KEY`. `v9x_relay.py` beside the server is the transport.

## Tools

| Tool | Purpose |
|---|---|
| `v9x_discover` | List agents on the LAN (instance, address, port, version, install ID) by mDNS; opens no agent connection |
| `v9x_ping` | Liveness, uptime, boot counter |
| `v9x_info` | Full agent status (version, screen mode, desktop readiness, install ID...) |
| `v9x_inventory` | Hardware/software inventory records (agent 0.7.0+) |
| `v9x_exec` | Run a Win32 EXE with captured stdout/stderr and a timeout |
| `v9x_shell` | Run a `COMMAND.COM /C` command (DIR, batch files, VER...) |
| `v9x_put_file` | Upload a local file, CRC32-verified |
| `v9x_get_file` | Download a guest file, CRC32-verified |
| `v9x_push_tree` | Recursively upload a directory |
| `v9x_stat` | Existence/size/attributes of a guest path |
| `v9x_list_dir` | Directory listing |
| `v9x_mkdir` | Idempotent directory creation |
| `v9x_read_text` | A guest text file inline: last `tail` lines, lines containing `match`, bounded (agent 0.13.0+) |
| `v9x_hash` | Size, CRC32, SHA-256 of a guest file; `local_path` compares with a host file (0.13.0+) |
| `v9x_delete` | Delete a file, or a directory (`recursive`, `force`); refuses roots, WINDOWS, SYSTEM, the agent folder (0.13.0+) |
| `v9x_ps` | Guest processes; marks the agent and its exec jobs (0.13.0+, Windows 9x/2000) |
| `v9x_kill` | Terminate a process by pid or unique name, e.g. a stuck exec job (0.13.0+) |
| `v9x_reg_query` | Subkeys and values of one registry key, read-only (0.13.0+) |
| `v9x_wait_file` | Wait until a guest file exists (`gone`, `stable_ms`) instead of sleeping; any agent |
| `v9x_windows` | Visible top-level windows; `wait_for` a title to appear (`gone`: close) (0.13.0+) |

No tool needs a sleep around it: `v9x_ping` takes `wait_seconds` (keeps
retrying while the guest boots), `v9x_exec`/`v9x_shell` take
`wait_for_file` / `wait_for_window` (+ `wait_seconds`), `v9x_screenshot` and
the input tools take `window` (capture just that window), and every
connection that is dropped before HELLO is retried twice.
| `v9x_screenshot` | Capture the guest screen, returned as a PNG image block the model can see |
| `v9x_click` | Click a mouse button, optionally moving to (x,y) first; `double` for double-click |
| `v9x_move` | Move the cursor to absolute screen coordinates |
| `v9x_type` | Type ASCII text into the focused window |
| `v9x_key` | Press a key or hotkey combo, e.g. `CTRL+ESC`, `ALT+F4` |
| `v9x_scroll` | Scroll the mouse wheel |
| `v9x_input` | Raw ordered action batch for drags and precise sequences |
| `v9x_reboot_with_proof` | Reboot and verify reconnect + changed agent-start counter + echoed resume token |
| `v9x_reset_with_proof` | Windows 9x guests whose restart hangs: flush, hard reset through CF9h, same proof as reboot (agent 0.10.0+) |
| `v9x_shutdown` | Clean shutdown |
| `v9x_wait_desktop` | Block until the Windows desktop is ready |
| `v9x_vnc` | Status, start or stop the guest's VNC server for a person to use (only where SETUP.EXE enabled it) |
| `v9x_relay_machines` | Remote-help machines lent by their owners: id, owner, OS, online, paused, busy |
| `v9x_chat` | Send a chat message to a remote-help machine's owner, or read the conversation |

Every tool opens a fresh connection, so a crashed VM never wedges the server;
errors come back as tool results with `isError` set.

The screenshot tool converts the agent's BMP (24-bit, or from agent 0.13.0
8-bit/RLE8) to PNG in-process (also standard library only) and returns it as
an MCP image content block, so the model can actually look at the Windows 98
screen. Pass `save_path` to keep a copy on disk. Pass `region`
(`[x, y, width, height]`), `scale` (0.01-1) and `gray` to keep the image
small: image cost grows with pixels. 0.13.0+ agents shape and losslessly pack
the image before sending it (much faster over remote help); for older ones
the server shapes it after the download. The metadata then includes
`region_x`/`region_y`/`region_width`/`region_height`, so
`screen_x = region_x + image_x * region_width / width`.

Every input tool (`v9x_click`, `v9x_move`, `v9x_type`, `v9x_key`,
`v9x_scroll`, `v9x_input`) also takes `screenshot: true`: after the input it
waits `settle_ms` (default 500, max 30000) for the guest to repaint and returns
the PNG in the same call, on the same connection (`save_path` works there too).
One GUI step (act, look) is then one tool call.

Never request a screenshot during a fullscreen mode transition or suspected
display wedge. Use info, direct trace execution, and file retrieval first;
capture only after the desktop/display is confirmed stable.

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
