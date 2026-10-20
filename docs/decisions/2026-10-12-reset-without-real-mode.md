# `reset`: a flushed hardware reset instead of Windows' restart

Status: accepted (verified on a PIIX4 VM and the ICH7 netbook)
Date: 2026-10-12

## Context

On the Velocity9x netbook (HP Mini 110, Intel 945GSE/ICH7, Windows 98 SE) no
Windows restart completes, whether from `reboot` (`ExitWindowsEx(EWX_REBOOT |
EWX_FORCE)`) or the Start menu. It stops at a flashing `_` or, once, a fatal
exception 0C. It hangs on the stock `vga.drv` too, and it completes once
`net start` is removed from `AUTOEXEC.BAT`: the cause is the real-mode NDIS2
ethernet stack (`protman$`, `ndishlp$`, the AR8132 driver). It stays resident
under Windows and is still live when Windows drops back to real mode to
restart. Ethernet is the machine's only network, so the driver stays, and
every hung restart costs a walk to the machine.

The goal was a verb that does as much of a clean shutdown as ring 3 can,
then resets the machine before Windows ever returns to real mode.

## Measurements

Three small GUI-subsystem probes (no DOS VM), in `tests\integration\`:

**`portprobe.c`: can a Win32 program reach the reset ports?** It reads CPL,
IOPL and the System VM's TSS I/O permission bitmap (found with `SGDT`/`STR`;
the Win9x system arena is readable from ring 3) and times byte `IN`s. It
never writes a port.

| | PIIX4 VM (86Box, 430TX) | ICH7 netbook |
|---|---|---|
| CPL / IOPL | 3 / 0 | 3 / 0 |
| 60h, 61h, 64h | trapped | trapped |
| 80h, 92h | direct | direct |
| CF8h-CFBh | direct | direct |
| CFCh | trapped | direct |
| `IN` CF9h | 00h | 00h, ~2100 cycles (64h: ~4500, trapped) |

So `OUT` to CF9h (reset control) and 92h (port A) from a Win32 thread goes
straight to the hardware, and no VxD is needed. 64h (the 8042) goes through
VKD.

**`flushprb.c`: what commits VCACHE?** Int 21h 710Dh (reset drive, flush
buffers and cache) is not executed through either `VWIN32_DIOC_DOS_IOCTL` or
`VWIN32_DIOC_DOS_DRIVEINFO` (carry stays set, AX=0001h). Lock Logical Volume
(440Dh/084Ah) goes through `VWIN32_DIOC_DOS_IOCTL`: level 0 is refused with
files open (AX=0005h), level 1 is granted in 18 ms and unlocks cleanly.

**`resetprb.c`: what survives a reset?** Each run writes a new 64 KiB file
and an HKLM value, then resets from the probe itself.

| Run | File | Registry value | ScanDisk at boot |
|---|---|---|---|
| CF9h, no flush | lost, with its new directory | lost | ran |
| `RegFlushKey` + level-1 lock, then CF9h | kept | kept | did not run |
| port 92h INIT, no flush | (not checked) | | ran |
| 8042 FEh, no flush | kept | | did not run |
| level-1 lock held, then 8042 FEh | machine hung, no reset | | |

The unflushed run also lost the rename at the end of a `put` that finished a
second or two earlier (only the `.PART` file was left), so without a flush,
staging files and then resetting is not safe.

The 8042 results show that a trapped FEh is not a plain pulse: VKD hands it
to Windows' own restart path, which flushes (hence no ScanDisk) and waits for
a held volume lock forever. That path may also be the real-mode return that
hangs the netbook.

## Decision

`RESET_REQUEST` (`0x0043`), capability `0x00010000`, Windows 9x only (NT
withholds the bit and answers `UNSUPPORTED_OPERATION`: ring-3 port I/O faults
there). After the same validation, token persistence and acceptance as
`reboot`:

1. `WM_ENDSESSION(TRUE)` to the top-level windows of every other process,
   `SendMessageTimeout(SMTO_ABORTIFHUNG)`, 2 s per window and 8 s in all.
   No `WM_QUERYENDSESSION`, matching `EWX_FORCE`: nothing can veto or prompt.
   WinOldAp (`tty`) windows are skipped.
2. `RegFlushKey` on HKLM, HKU and HKCU.
3. Log `reset-flush`, then freeze the agent's own log so no agent thread
   (a client polling for the reset, say) writes to disk again.
4. A level-1 lock (writes by others still allowed) on every fixed drive,
   then 3 s with the disk idle so the drive can empty its write cache.
5. CF9h full reset: `OUT CF9h, 0Ah` then `0Eh` (ICH7 RST_CNT / PIIX4 RC:
   FULL_RST and SYS_RST, then RST_CPU 0 to 1). The chipset drops
   SLP_S3#/S4#/S5# for a few seconds: a real power cycle.
6. Still alive 8 s later: release the locks, unfreeze the log, log
   `reset-failed` and keep serving. No fallback.

A VxD, or a `System_Exit` hook that resets after Windows' own flush, was not
needed once ring 3 could reach CF9h.

### How it got there (netbook runs)

1. Hard reset `02h`/`06h`, then port 92h INIT, then (locks released) the
   8042 pulse: the flush worked (log lines, WININIT rename, no ScanDisk), but
   the netbook went dark and stayed off until its power button was held.
   No `reset-failed` was logged, so it died inside that block: either `06h`
   wedged the board or it was ignored and the 92h INIT did. (The 8042 path
   would have produced Windows' real-mode hang instead, a flashing cursor.)
2. Full reset `0Ah`/`0Eh` alone: back on its own in 30 s, no ScanDisk. But
   the log lines written just before the locks were blanks on disk: the
   file's new length had been written, its data had not. The full reset
   powers the drive off, and the lock had only handed the data to the
   drive's write cache. Windows 98 SE had the same kind of trouble powering
   IDE drives off before their cache was written.
3. Full reset with the log frozen and 3 s of idle after the locks: back in
   32 s, the reset lines intact, no ScanDisk.

The 8042 pulse and port 92h are not used: the first takes Windows' restart
path, which is what hangs this machine, and the second (or the hard reset)
wedged it.

## The dirty flag

No raw FAT write is needed: granting the level-1 lock leaves the volume
marked clean, and ScanDisk did not run after any flushed reset. Should a
reset ever happen without the lock (a drive the lock fails on), ScanDisk runs
at the next boot. With `MSDOS.SYS` `AutoScan=1` (the netbook's setting) it ran
to completion on its own on the VM; if it finds errors it may stop for input.
`AutoScan=2` fixes without asking.

## WININIT.INI

Verified on the VM: a `WININIT.INI` put seconds before `reset` was processed
at the next boot (renamed, `WININIT.BAK` left). Its paths must be 8.3, as for
any restart: an entry naming `C:\V9XREMOTE\...` was consumed and ignored,
the same entry as `C:\V9XREM~1\...` worked.

## Verification

- PIIX4 VM: `v9xctl reset` twice, boots 25 to 26 and 26 to 27, proof by token
  and counter, no ScanDisk; `AGENT.LOG` kept `reset-notified 20 184ms` and
  `reset-flush C`, the last line written before the locks.
- Windows 2000 VM: capability absent, `v9xctl reset` exits 23 without
  sending, a raw `RESET_REQUEST` returns `UNSUPPORTED_OPERATION`.
- Netbook (agent 0.10.0 test builds): attempt 3 above, boots 172 to 173, proof
  in 32 s with no hand reset; `SCANDISK.LOG` unchanged; the WININIT rename
  staged before attempt 1 applied. Attempt 2 (boots 170 to 171) also came
  back on its own.
