#ifndef V9XREMOTE_PLATFORM_WIN_H
#define V9XREMOTE_PLATFORM_WIN_H

/* Win9x versus Windows NT (2000) differences, shared by V9XAGNT.EXE and
   SETUP.EXE. Every Win9x path stays exactly as it was; NT gets its own. */

/* 1 on the NT family (Windows 2000), 0 on Windows 95/98/ME. */
int v9x_platform_is_nt(void);

/* HKLM key whose "V9xRemoteAgent" value starts the agent: RunServices on
   Win9x (before logon). NT ignores RunServices, so there it is Run (at
   logon; pair it with autologon). */
const char *v9x_platform_run_key(void);

/* The interpreter shell-mode EXEC wraps a command in, trailing space
   included: "COMMAND.COM /C " on Win9x, "CMD.EXE /C " on NT, where
   COMMAND.COM is the 16-bit one running under NTVDM. */
const char *v9x_platform_shell_prefix(void);

/* NT refuses ExitWindowsEx until the caller enables SeShutdownPrivilege,
   which an administrator holds but has disabled. Returns 1 when enabled or
   when the platform has no such thing (Win9x), 0 on failure. */
int v9x_platform_enable_shutdown(void);

/* 1 when ExitWindowsEx(EWX_POWEROFF) can really switch the machine off. On
   NT with the "Standard PC" HAL (no ACPI, no APM) there is no soft-off, and
   that HAL turns a power-off request into a reboot; there a shutdown must
   omit EWX_POWEROFF and stop at "It is now safe to turn off your computer".
   Asks POWRPROF's GetPwrCapabilities (SystemS5) on NT; always 1 on Win9x,
   whose shutdown behaviour is unchanged. */
int v9x_platform_can_power_off(void);

/* NT Winsock sockets are kernel handles, inheritable by default, and the
   agent launches children with bInheritHandles=TRUE (for their NUL or pipe
   standard handles). A child would then hold the listening socket and keep
   port 9869 bound after the agent exits, so a hot-updated or restarted
   agent could never listen again. Clears the inherit flag on NT; does
   nothing on Win9x, where sockets are not inherited this way. */
void v9x_platform_no_inherit(HANDLE handle);

#endif
