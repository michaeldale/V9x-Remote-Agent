# Install wizard, mDNS discovery and the install ID
Status: accepted

Date: 2026-10-01

## Context

Three requests arrived together: an install screen where features can be
switched on and off, a way for a host to find agents on a LAN without knowing
their addresses, and a stable identity so a machine can be followed when DHCP
or a move changes its IP. Until 0.8.0 the only installer was `INSTALL.BAT`,
settings meant hand-editing `AGENT.INI`, and the host only ever reached a
guest at an address typed on the command line.

## Decisions

**Standard mDNS/DNS-SD, not a custom beacon.** Winsock 1.1 on every 9x
release can join a multicast group (`IP_ADD_MEMBERSHIP` = 5 in the 1.1
numbering), so the agent can speak RFC 6762/6763 itself, with no Bonjour on the
guest. Standard tools (`dns-sd -B _v9xremote._tcp`, `avahi-browse`) see it
too. The host side needs nothing beyond the .NET or Python standard library:
it sends one legacy one-shot query from an ephemeral port, and the agent
answers by unicast. The host never binds 5353, which Windows' own mDNS service
usually holds.

**No probing.** RFC 6762 wants a responder to probe its names before claiming
them. Both names here carry the first 8 hex characters of the install ID
(`<COMPUTERNAME>-<id8>`, `v9x-<id8>.local`), so two installs cannot collide
unless their IDs share 32 random bits. The probe/rename machinery is not
worth its code in a runtime-free C agent.

**Off by default, ticked in setup.** The protocol is unauthenticated, and
advertising it announces a remote-execution endpoint to the whole segment. So
the shipped `AGENT.INI` and `INSTALL.BAT` installs stay silent, and SETUP.EXE
shows the option ticked, where the person installing sees it and can untick
it. Discovery only helps on a LAN the host shares with the guest. Under 86Box
SLiRP, multicast never reaches the host, which is why `tests/integration/mdnsq.c`
exists: it queries the responder from inside the guest.

**The install ID lives in AGENT.INI and never in HELLO.** Both host parsers
reject trailing HELLO and INFO data, so appending a field would break every
older host against a newer agent. The ID goes into INVENTORY `meta` (schema
free), into the mDNS TXT record, and into INFO only when the host asks for it
with a flags payload. The host asks only when the agent advertises
`V9X_CAP_INSTALL_ID`, because older agents require an empty INFO payload.
Keeping the ID in the INI means it survives agent upgrades and reinstalls, and
a person can see it and reset it. The agent creates one on start-up when it is
missing, so no install path is left without one.

**The ID is generated without a random API.** `CoCreateGuid`/`UuidCreate`
would pull OLE32 or RPCRT4 into an agent whose imports are audited. Instead a
32-bit MurmurHash3-finaliser pool is fed with:
- the time
- tick and performance counters sampled around `Sleep(0)`
- process and thread IDs, and a stack address
- free memory
- the C: volume serial and the computer name

The DOS agent's pool uses the BIOS tick, `clock()` and BOOT.DAT. RDTSC is
avoided because it faults on a 386/486. The output only has to be unique, not
secret.

**SETUP.EXE is a plain window, not a dialog resource.** The build has no
resource compiler step (the same constraint as the tray icon), so the wizard
creates its controls in code and switches pages by showing and hiding groups.
It may import GDI32, but only to use the stock dialog font. The long-lived
agent still may not import GDI32. Over an existing install, setup follows
`INSTALL.BAT`'s update path exactly: it stages `V9XNEW.EXE`/`V9XSNEW.EXE` and
points RunServices at `UPDATE.BAT`. Setup never replaces a running agent in
place.

## Verified

On WIN98-S3NATIVE (86Box, 98 SE, agent 0.6.2 installed, boot 17):
- The wizard rendered correctly and pre-filled the existing settings. It
  refused port 70000.
- The first run exposed that `WritePrivateProfileStringA(NULL, NULL, NULL,
  ini)`, the cache flush, returns FALSE on Windows 98 even though it works.
  Setup stopped at that step with nothing staged. The flush result is now
  ignored.
- The second run staged the update, kept the install ID, and restarted from
  the Finish page.
- Boot 18 came up as 0.8.0 with RunServices restored by UPDATE.BAT, the
  Add/Remove Programs entry present, INFO and INVENTORY reporting the same
  install ID, and `meta discovery=on`.
- `MDNSQ` inside the guest got correct legacy-unicast answers to both a
  unicast query and one sent to 224.0.0.251. The host codec decoded all four
  records.
- `SETUP.EXE /S` from `C:\V9XREMOTE` rewrote the settings only and exited 0.
