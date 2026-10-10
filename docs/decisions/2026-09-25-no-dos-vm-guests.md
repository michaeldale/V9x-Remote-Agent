# Guests that cannot start a DOS VM: allow_dos_vm and the native hot swap

Date: 2026-09-25
Status: accepted

## Context

The Velocity9x netbook (Intel GMA 950, the project's own display driver)
corrupts its screen whenever Windows 98 creates a DOS virtual machine. It
does not matter whether the VM is a visible DOS box or a hidden one: a
`shell` request (`COMMAND.COM /C`), a console-subsystem Win32 program
(Windows 9x hosts those in a VM through CONAGENT), a DOS `.EXE` or `.COM`,
and a batch file all trigger it.

The agent was creating VMs in two places without the operator asking for
one:

- every `shell` verb, by design, and any `exec` of a console program;
- every hot update, because the swap was a `HOTSWAP.BAT` run through
  `COMMAND.COM`.

The second one is the worse offender: it is invisible in the request, and it
happened on each of the three updates pushed to the netbook on 24 and 25
September.

## Decision

1. **Hot update is native.** `UPDATE_REQUEST` launches the already-staged
   `V9XNEW.EXE` with `-hotswap`. `entry.c` routes that switch to
   `v9x_hotswap_run`, which does exactly what the batch did: wait for the
   running agent to release `V9XAGNT.EXE` (rename as the probe, the same
   trick the batch used because `COPY /Y` lies about locked files), copy the
   staged binaries in, re-assert the RunServices value, relaunch the agent,
   and write the same step names to `HOTSWAP.LOG`. The registry write needs
   ADVAPI32, now an allowed import. The one thing the swapper cannot do is
   delete itself, so the relaunched agent removes `V9XNEW.EXE` at start.
   Updating *from* an agent older than 0.6.4 still runs that agent's batch
   one last time.

2. **`allow_dos_vm=0` is a per-guest AGENT.INI switch**, default on. With it
   off the agent withdraws `EXEC_SHELL` from the advertised capabilities, so
   `v9xctl` and the MCP server refuse `shell` on the host side without
   sending a frame, and `v9x_execution_prepare` rejects any direct exec whose
   target is not a Win32 GUI-subsystem PE (it already parsed the PE header to
   decide window handling) with `UNSUPPORTED_OPERATION` and a message that
   says why. It is a switch rather than the default because every other
   guest is fine with VMs and `shell` is the convenient path for built-ins.

## Consequences

- No protocol change. Capability bits and status codes already existed; the
  only visible difference is which are used.
- On a `allow_dos_vm=0` guest there is no way to delete a guest file or run
  a batch through the agent. A native delete verb would close most of that
  gap and is the obvious next step if it is needed.
- `scripts\set-autologon.ps1 -Enable`/`-Disable` move the password list
  with `shell` and do not work on such a guest. `-Dismiss` uses `input` and
  does.
- The Velocity9x driver's VM problem itself is untouched; this only keeps
  the agent from triggering it.
