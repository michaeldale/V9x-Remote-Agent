# Quickstart: 86Box + Windows 98 + the agent

This walks from nothing to an AI-controllable Windows 98 guest. Budget an hour
if you have never installed Windows 98 before; most of it is watching the
installer.

## What you need

- **86Box 6.0 or later** with its ROM set: https://86box.net (the 86Box docs
  cover ROM installation).
- **Your own Windows 98 SE installation media and license.** This project
  contains no Microsoft software and cannot provide any. 86Box's own guides
  cover installing an OS into a VM.
- **This project**: a release download (contains `V9XREMOTE.ISO` prebuilt) or
  a checkout of the repo (build the ISO with
  `.\scripts\make-install-media.ps1`; see the README Build section).

## 1. Create the VM

Any Windows 98 capable 86Box machine works. This hardware profile is the one
the project is developed and tested against:

| Setting | Value |
|---|---|
| Machine | Socket 7 (Intel i430TX), e.g. `YM430TX` |
| CPU | Pentium MMX 200 |
| RAM | 128 MB |
| Video | S3 ViRGE/DX (`virge_dx_pci`) |
| Network | NE2000 Plug and Play (`ne2kpnp`), mode SLiRP |
| Disk | IDE hard disk image, 500 MB to 2 GB |
| CD-ROM | Any IDE CD-ROM drive (needed for the install ISO) |

Set the network card in the 86Box UI: Settings > Network > NE2000 Plug and
Play, mode SLiRP. SLiRP gives the guest NAT internet access and, crucially,
lets the host forward a TCP port into the guest.

## 2. Install Windows 98

Install Windows 98 SE from your own media, per the 86Box documentation. After
setup completes, let Windows detect the NE2000 adapter; TCP/IP with DHCP is
the Windows default and is what SLiRP expects. Verify inside the guest with
`Start > Run > winipcfg`: the adapter should show an address like
`10.0.2.15`.

## 3. Forward the agent port

Close 86Box, open the VM profile's `86box.cfg` in a text editor, and add:

```ini
[SLiRP Port Forwarding #1]
0_protocol = tcp
0_external = 9869
0_internal = 9869
```

(The `[Network]` section with `net_01_card = ne2kpnp` and
`net_01_net_type = slirp` will already be there from step 1.)

**Firewall note:** an SLiRP forward listens on every host address, not just
localhost, so anything on your LAN could reach the guest. Restrict inbound
TCP 9869 in the host firewall to localhost, or at minimum to your own
machine. The protocol is deliberately unauthenticated; treat the port like a
root shell to the guest, because that is what it is.

## 4. Install the agent from the ISO

1. Start the VM. In 86Box: Media > CD-ROM > Image, and pick `V9XREMOTE.ISO`.
2. In the guest: `Start > Run > D:\INSTALL.BAT` (adjust the drive letter if
   needed).
3. Reboot Windows when the script says so. The agent starts with Windows via
   `RunServices` and writes its log to `C:\V9XREMOTE\AGENT.LOG`.

## 5. Verify from the host

From the repo or release directory on the host:

```powershell
.\scripts\v9xctl.ps1 ping
.\scripts\v9xctl.ps1 shell -Command "VER" -Json
.\scripts\v9xctl.ps1 screenshot -Destination .\desktop.bmp -Json
```

`ping` returning `{"Success":true,...}` with an uptime and boot counter means
everything works. Now read [ai-workflows.md](ai-workflows.md) to put an AI
agent in the loop, or [host-cli.md](host-cli.md) for the full command
reference.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `ping` says connection refused | Port forward missing, or the VM is off / still booting | Check the `[SLiRP Port Forwarding #1]` stanza; wait for the desktop; agent starts a few seconds after boot |
| `ping` times out with no refusal | Host firewall is intercepting, or another service owns 9869 | Check firewall rules and `netstat -ano | findstr 9869` |
| Refused even at the Win98 desktop | Agent not installed or not in RunServices | In the guest, check `C:\V9XREMOTE\AGENT.LOG` exists and re-run `INSTALL.BAT`, then reboot |
| Guest has no IP in winipcfg | Wrong NIC type or SLiRP not selected | Use NE2000 Plug and Play + SLiRP; let Windows redetect hardware |
| Files on `D:` not visible | ISO not attached | 86Box Media > CD-ROM > Image, select the ISO, then re-open `D:` |
| Agent worked, then refused after a while | The agent serves one client at a time; a stuck client blocks reconnects briefly | Wait a few seconds; each v9xctl invocation opens a fresh connection |
