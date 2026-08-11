# Security

## Threat model

Protocol v1 is **unauthenticated and unencrypted by design**. Anyone who can
open a TCP connection to the agent port (default 9869) has full control of
the guest: arbitrary execution, file read/write, reboot and shutdown. This is
a deliberate trade-off for a disposable development sandbox on a 1998-era
guest OS, documented in [docs/design.md](docs/design.md).

Consequences:

- Run the guest behind NAT (86Box SLiRP) and restrict the forwarded host port
  to localhost with the host firewall.
- Never bridge the guest NIC, never port-forward 9869 from a router, never
  expose it to a LAN you do not fully trust.
- On physical machines, use an isolated network segment and the agent's
  `bind_address`/`allowed_client` settings
  ([docs/physical-machine.md](docs/physical-machine.md)); these reduce
  accidents, they are not authentication.
- Treat every guest as disposable. Do not store credentials or private data
  inside it.

Reports that the guest can be controlled through its own port are therefore
not vulnerabilities; that is the product.

## What is in scope

Host-side issues are in scope and welcome:

- Memory-unsafe parsing or injection in `scripts\v9xctl.ps1` /
  `scripts\V9xProtocol.ps1`
- Vulnerabilities in the MCP server (`mcp\v9x_mcp.py`), e.g. path traversal
  from tool arguments or unsafe handling of guest-supplied data
- The guest agent trusting host-supplied data in a way that corrupts the
  agent itself (e.g. protocol parsing bugs exploitable before the attacker
  already has full control)
- Build or release pipeline integrity issues

## Reporting

Use GitHub private vulnerability reporting (Security > Report a
vulnerability) on the repository. Please include reproduction steps. There is
no bounty program; fixes are best-effort but taken seriously.
