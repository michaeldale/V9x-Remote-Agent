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

Version 0.8.0 adds SETUP.EXE, an install wizard; an install ID that
identifies this machine when its IP address changes; and optional LAN
discovery over mDNS (discovery=1 in AGENT.INI).

Version 0.9.0 also runs on Windows 2000. Install it there with SETUP.EXE
(INSTALL.BAT is Windows 98 only), as an administrator. Windows 2000 ignores
RunServices, so the agent starts from the Run key when a user logs on: set
up automatic logon so it starts unattended. Shell commands run through
CMD.EXE /C there instead of COMMAND.COM /C.

INSTALL
1. Run SETUP.EXE (AutoPlay starts it from the CD). Choose the settings,
   click Install, and let it restart Windows.
   Run C:\V9XREMOTE\SETUP.EXE later to change the settings; remove the agent
   from Add/Remove Programs.
Without a window:
1. Copy this package to a local Windows 98 directory.
2. Run INSTALL.BAT.
3. Reboot Windows.

If an older agent is already installed, INSTALL.BAT stages V9XNEW.EXE and sets
UPDATE.BAT to run during the next boot. UPDATE.BAT replaces the inactive old
binary, restores normal RunServices startup, and launches the new agent.

The package includes install, update, and removal registry files. INSTALL.BAT
copies them and the disposable V9XSHOT.EXE capture helper into C:\V9XREMOTE.
The agent writes AGENT.LOG and BOOT.DAT there. BOOT.DAT counts agent starts;
it is not proof of a machine reboot by itself.

Never request a screenshot during a fullscreen mode transition or suspected
display wedge. Use INFO, run a trace dumper directly, and retrieve trace files
first. Capture only after Explorer's desktop is ready and the display is stable.

The protocol is unauthenticated. Keep the guest behind 86Box SLiRP and protect
the forwarded port with the host firewall. Never expose port 9869 to a LAN or
the Internet.
