# Tray icon: CreateThread needs a real lpThreadId on Windows 9x

Date: 2026-09-04
Status: accepted

## Context

The notification-area icon documented in `guest-agent.md` had never appeared on
any guest, from the first release that shipped it to 0.6.1. `AGENT.LOG` on
WIN98-S3NATIVE carried `tray-icon-failed` on all ten of its recorded boots and
on every boot of every other guest checked, so the failure was total rather
than intermittent.

The log could not say more than that, because `v9x_tray_start` returned 0 for
three different reasons and `agent.c` logged one undifferentiated
`tray-icon-failed` for all of them. Worse, a failing `Shell_NotifyIcon` was not
covered at all: the tray worker retried it every second forever and discarded
the result, so that path could never appear in the log.

An instrumented build (`tray-diag`) was deployed to the VM to log
`GetLastError` at each of the three exits.

## Options

Three candidate causes, in the order they were suspected:

1. **No message pump anywhere in the agent.** Confirmed as fact by grep and by
   the build's own import allowlist: `GetMessage`, `PeekMessage` and
   `DispatchMessage` appear nowhere in the binary, and the owner window was
   created on the main thread, which then blocks in `accept()` for the life of
   the process. A systray owner window whose thread never dispatches is a dead
   window to the shell.
2. **A wrong `NOTIFYICONDATA.cbSize`.** Ruled out by reading the header:
   Open Watcom's `NOTIFYICONDATAA_V1_SIZE` is
   `FIELD_OFFSET(NOTIFYICONDATAA, szTip[64])` = 88 bytes, which is correct for
   the shell32 4.72 that ships with 98 SE.
3. **`CreateThread` rejecting a NULL `lpThreadId`.** Suspected because the two
   `CreateThread` calls that demonstrably worked (`agent.c` for connections,
   `execute.c` for the execution pool) both pass a real pointer, and only
   `tray.c` passed 0.

## Decision

Cause 3, measured directly:

```
2026-09-04 16:00:35 b11 tray-thread-failed gle=87
```

`gle=87` is `ERROR_INVALID_PARAMETER`. Windows 9x rejects a NULL `lpThreadId`;
Windows NT accepts it, which is why nothing on a modern host ever flagged the
call. The window and the icon handle were both created successfully - the
worker thread simply never existed, so `Shell_NotifyIcon` was never reached.
Hypothesis 1 was real as a latent defect but was **not** the cause of the
symptom, and hypothesis 2 was wrong.

`tray.c` now:

- passes a real `DWORD` to `CreateThread`;
- creates the owner window **on the worker thread**, the only thread that
  pumps, instead of on the main thread that blocks in `accept()`;
- drains the queue with `PeekMessageA`/`DispatchMessageA` in 100 ms slices
  inside `v9x_tray_wait`, replacing the bare `Sleep` calls;
- names each failure in the log with its `GetLastError`
  (`tray-window-failed`, `tray-loadicon-failed`, `tray-thread-failed`,
  `tray-notify-failed`) and logs `tray-icon-added` on success.

Verified on WIN98-S3NATIVE (86Box, ym430tx, 98 SE): boot 12 logged
`tray-icon-added 10.0.2.15`, the icon is present in the notification area, and
hovering it shows `Agent Version: 0.6.1 | Port: 9869 | IP: 10.0.2.15`.

## Consequences

- Any future `CreateThread` in guest code must pass a real `lpThreadId`. This
  now holds at all three call sites.
- `PeekMessageA` and `DispatchMessageA` are added to the import allowlist in
  `build-guest.ps1`, so their loss would fail the build.
- The generic `tray-icon-failed` line is kept for compatibility with old logs
  but is now always preceded by a specific reason.
- The real lesson is not the Win9x quirk but the silent failure: a code path
  that retried forever and discarded its error hid a completely broken feature
  across several releases. Failure paths in the guest log their reason.
