#ifndef V9X_VNCSVC_H
#define V9X_VNCSVC_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Windows NT service support for V9XVNC.EXE, all resolved at run time so the
   one binary still loads on Windows 9x (where none of this is used). On
   Windows 2000 the server runs as a SYSTEM service so it can open the
   Winlogon (secure) desktop, which a user process cannot; see
   docs\decisions\2026-10-15-vnc-remote-control.md.

   The server's own work is vnc_service_main(argc, argv) (vnc.c): on 9x and
   for a console run it is called directly; as a service it is called from
   the service thread once SERVICE_RUNNING is reported. vnc_service_stopping()
   lets the server's loop see a stop request. */

int v9x_is_nt(void);

/* -install / -uninstall (NT only): create or delete the service, as SYSTEM
   with desktop access. Return 0 on success. */
int v9x_service_install(const char *image_path);
int v9x_service_uninstall(void);

/* Start (start=1) or stop (0) the installed service through the SCM. The
   running service's stop event carries LocalSystem's default DACL, which the
   logged-on user cannot open, so the agent cannot stop it by event. Returns
   -1 when no service is installed (always on 9x), 0 on success (including
   already running / already stopped), 1 on failure. */
int v9x_service_control(int start);

/* -service: hand control to the SCM. Returns 0 when it dispatched (the
   process should exit with vnc_service_exit_code()), or non-zero when this
   is not running as a service (caller falls back to a console run). */
int v9x_service_run(void);
DWORD vnc_service_exit_code(void);
int vnc_service_stopping(void);

/* Desktop following (NT only; a no-op that returns 0 on 9x). Attaches the
   calling thread to the current input desktop when it has changed since the
   last call on that thread. `state` is a small caller-owned struct, zeroed
   once before the first call. Returns 1 when the desktop changed. */
typedef struct VncDesktop {
    HDESK desktop;
    char name[64];
    int tried;
} VncDesktop;

int vnc_follow_input_desktop(VncDesktop *state);
void vnc_desktop_release(VncDesktop *state);

#endif
