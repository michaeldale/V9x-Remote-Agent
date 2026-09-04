v9x Remote Agent for DOS - version @VERSION@
============================================

A cut-down build of the v9x remote agent for real-mode DOS boxes (MS-DOS,
PC-DOS, FreeDOS), running in 32-bit protected mode via the DOS/4GW extender.
It speaks the identical wire protocol as the Windows 9x agent, so the same host
tooling (scripts\v9xctl.ps1 and the MCP server) drives it unchanged.

What it does
------------
  * HELLO / PING / INFO handshake and health.
  * Command execution (direct and COMMAND.COM shell modes), stdout captured.
  * File transfer: stat, list, mkdir, CRC32-verified upload and download.

What it does NOT do (dropped for DOS)
-------------------------------------
  * Live output streaming, cancel, timeout - a command runs to completion first,
    then its stdout is returned in one batch. There is no stderr capture
    (COMMAND.COM redirects stdout only).
  * Screenshot, input injection, power control, hot-update, HTTP download.
  * Concurrent connections - one client is served at a time.

Requirements
------------
  * A 386 or better (DOS/4GW is protected mode).
  * A packet driver for the network card (e.g. NE2000.COM for an NE2000 NIC).
  * ~200 KB free for DOS4GW.EXE plus the agent.

Install
-------
  1. Run INSTALL.BAT. It copies V9XAGNT.EXE, DOS4GW.EXE, AGENT.INI and
     WATTCP.CFG into C:\V9XREMOT.
  2. Edit C:\V9XREMOT\WATTCP.CFG for this machine's IP (static or DHCP).
  3. In AUTOEXEC.BAT, after the packet driver loads, add:

         SET WATTCP.CFG=C:\V9XREMOT
         C:\V9XREMOT\V9XAGNT.EXE

     The agent runs in the foreground and never returns, so it must be the last
     line of AUTOEXEC.BAT. The machine is then a dedicated agent.

Configuration (C:\V9XREMOT\AGENT.INI)
-------------------------------------
  port            TCP port to listen on (default 9869).
  allowed_client  Optional single client IP allowed to connect (blank = any).

Security
--------
Protocol v1 is unauthenticated: anyone who can reach the port has full control
of the box. Keep the NIC on an isolated network (an 86Box SLiRP/NAT is ideal);
never bridge it onto a untrusted LAN or the internet.

Logs and state live under C:\V9XREMOT (AGENT.LOG, BOOT.DAT, TEMP\).
