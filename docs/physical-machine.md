# Physical Windows 9x machine

Since version 0.4.1 the agent can listen on a physical machine's IPv4 adapter.
The protocol
still grants full guest control and has no authentication or encryption. Use
this only on an isolated development network, a dedicated VLAN, or a direct
cable. Do not expose TCP 9869 to the Internet or an untrusted LAN.

## Install and configure

Copy the complete `build\install` package to the physical Windows 9x machine and run
`INSTALL.BAT`. Edit `C:\V9XREMOTE\AGENT.INI` before the reboot:

```ini
[agent]
port=9869
bind_address=0.0.0.0
allowed_client=192.168.10.20
log_level=info
```

- `port` is any TCP port from 1 through 65535.
- `bind_address` is `0.0.0.0` for all IPv4 adapters or the physical machine's
  exact dotted-decimal IPv4 address.
- `allowed_client` is the one host IPv4 address permitted to connect. Leave it
  blank only when the surrounding network independently blocks every untrusted
  client.

Hostnames, CIDR ranges, and IPv6 are not accepted. An invalid bind or client
address prevents the listener from starting and writes an error to
`C:\V9XREMOTE\AGENT.LOG`. Existing configuration is preserved during agent
updates.

Use a stable address for both machines, either static configuration or a DHCP
reservation. Reboot Windows after installation or configuration changes.

## Connect from the controller

From the controller machine:

```powershell
cd <repo>
Test-NetConnection 192.168.10.98 -Port 9869
.\scripts\v9xctl.ps1 info -Host 192.168.10.98 -Port 9869
.\scripts\v9xctl.ps1 ping -Host 192.168.10.98 -Port 9869 -Json
```

`info` reports the active `Port`, `ListenAddress`, and `AllowedClient` values.
All other commands—including files, execution, screenshots, and reboot—use the
same `-Host` and `-Port` options.

The allowlist is source-IP filtering, not authentication. It does not protect
traffic from observation or an attacker able to impersonate the permitted
address. Network isolation remains the security boundary.
