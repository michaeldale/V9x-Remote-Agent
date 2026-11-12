#include "agent.h"
#include "platform_win.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"

static int v9x_desktop_error(V9xConnection *conn, DWORD request_id,
                              DWORD status, const char *detail)
{
    unsigned long offset = 8ul;
    v9x_write_u32(conn->response, status);
    v9x_write_u32(conn->response + 4, 0ul);
    if (!v9x_append_string(conn->response, sizeof(conn->response), &offset, detail)) return 0;
    return v9x_send_frame(conn, V9X_MSG_ERROR_RESPONSE, request_id, conn->response, offset);
}

/* Cancel only the two known Windows 9x startup dialogs. Never inject a key
   into the foreground window: it may belong to an unrelated application.
   Titles are deliberately conservative (English Windows); unknown/localized
   dialogs are left alone. NT's secure logon desktop is not supported. */
static BOOL CALLBACK v9x_find_logon(HWND window, LPARAM parameter)
{
    HWND *found = (HWND *)parameter;
    char class_name[32];
    char title[80];
    HWND cancel;
    if (!IsWindowVisible(window) || !IsWindowEnabled(window)) return TRUE;
    if (GetClassNameA(window, class_name, sizeof(class_name)) == 0 ||
        lstrcmpA(class_name, "#32770") != 0) return TRUE;
    if (GetWindowTextA(window, title, sizeof(title)) == 0 ||
        (lstrcmpA(title, "Enter Network Password") != 0 &&
         lstrcmpA(title, "Enter Windows Password") != 0)) return TRUE;
    cancel = GetDlgItem(window, IDCANCEL);
    if (cancel == 0 || !IsWindowEnabled(cancel) || !IsWindowVisible(cancel))
        return TRUE;
    *found = window;
    return FALSE;
}

int v9x_handle_desktop_wait(V9xConnection *conn, unsigned long request_id,
                            const unsigned char *payload, unsigned long length)
{
    DWORD flags;
    DWORD timeout;
    DWORD started;
    DWORD elapsed;
    DWORD width, height, bpp;
    DWORD result = 0ul;
    HWND dialog = 0;
    HWND cancelled = 0;
    if (v9x_platform_is_nt())
        return v9x_desktop_error(conn, request_id, V9X_STATUS_UNSUPPORTED_OPERATION,
                              "logon cancellation is Windows 9x only");
    if (length != 8ul)
        return v9x_desktop_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              "desktop wait requires flags and timeout");
    flags = v9x_read_u32(payload);
    timeout = v9x_read_u32(payload + 4);
    /* At most one minute per request (hosts loop), like PROCESS_WAIT: the
       guest cannot see a client disconnect mid-wait, so a longer bound would
       let a dropped caller hold this connection slot or relay session. */
    if ((flags & ~V9X_DESKTOP_DISMISS_LOGON) != 0ul || timeout > 60000ul)
        return v9x_desktop_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              "invalid desktop wait flags or timeout");
    started = GetTickCount();
    for (;;) {
        if (v9x_desktop_ready()) {
            result |= V9X_DESKTOP_READY;
            break;
        }
        if ((flags & V9X_DESKTOP_DISMISS_LOGON) != 0ul && cancelled == 0) {
            dialog = 0;
            (void)EnumWindows(v9x_find_logon, (LPARAM)&dialog);
            if (dialog != 0 && PostMessageA(dialog, WM_COMMAND,
                    MAKEWPARAM(IDCANCEL, BN_CLICKED), (LPARAM)GetDlgItem(dialog, IDCANCEL))) {
                cancelled = dialog;
                v9x_event("Cancelling the Windows logon dialog at the developer's request", 0);
            }
        }
        /* A successful post is not proof. Report dismissal only once the
           original dialog is gone/hidden, and post at most once per call. */
        if (cancelled != 0 && (!IsWindow(cancelled) || !IsWindowVisible(cancelled)))
            result |= V9X_DESKTOP_LOGON_DISMISSED;
        elapsed = GetTickCount() - started;
        if (elapsed >= timeout) break;
        Sleep(timeout - elapsed < 100ul ? timeout - elapsed : 100ul);
    }
    if (cancelled != 0 && (!IsWindow(cancelled) || !IsWindowVisible(cancelled)))
        result |= V9X_DESKTOP_LOGON_DISMISSED;
    v9x_screen_info(&width, &height, &bpp);
    v9x_write_u32(conn->response, result);
    v9x_write_u32(conn->response + 4, conn->machine->boot_counter);
    v9x_write_u32(conn->response + 8, width);
    v9x_write_u32(conn->response + 12, height);
    v9x_write_u32(conn->response + 16, bpp);
    v9x_write_u32(conn->response + 20, GetTickCount() - started);
    return v9x_send_frame(conn, V9X_MSG_DESKTOP_WAIT_RESPONSE, request_id,
                          conn->response, 24ul);
}
