V9x Remote Agent @VERSION@
====================

This package provides HELLO, PING, INFO, direct execution, and COMMAND.COM
shell execution on TCP port 9869. Execution output and time are bounded.
It also supports stat, list, mkdir, and CRC32-verified file transfers up to
64 MiB per file. Uploads use .PART staging and rollback backups.
Version 0.4 adds persisted reboot resume tokens, controlled reboot/shutdown,
desktop readiness, and CRC32-verified 24-bit BMP screenshots.
Version 0.4.1 adds configurable IPv4 binding, TCP port, and a single-client
IPv4 allowlist for isolated physical-machine development networks.
Version 0.4.2 launches direct-mode GUI applications with their own window
state so paint-driven programs no longer hang hidden until timeout.

INSTALL
1. Copy this package to a local Windows 98 directory.
2. Run INSTALL.BAT.
3. Reboot Windows.

If an older agent is already installed, INSTALL.BAT stages V9XNEW.EXE and sets
UPDATE.BAT to run during the next boot. UPDATE.BAT replaces the inactive old
binary, restores normal RunServices startup, and launches the new agent.

The package includes install, update, and removal registry files. INSTALL.BAT
copies them into C:\V9XREMOTE. The agent writes AGENT.LOG and BOOT.DAT there.

The protocol is unauthenticated. Keep the guest behind 86Box SLiRP and protect
the forwarded port with the host firewall. Never expose port 9869 to a LAN or
the Internet.
