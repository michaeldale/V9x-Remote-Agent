#include "agent.h"
#include "v9xremote/version.h"
#include <shellapi.h>

#define V9X_TRAY_ICON_ID 1u
#define V9X_TRAY_PUMP_SLICE 100ul
/* Half-period of the green flash while the agent is busy. */
#define V9X_TRAY_FLASH_MS 250ul
/* Keep flashing this long after the last frame, so a request that completes
   in a few milliseconds (ping, stat) still produces a visible blink. */
#define V9X_TRAY_HOLD_MS 750ul
/* Idle refresh: re-resolve the address and re-assert the icon. */
#define V9X_TRAY_REFRESH_MS 5000ul

/* Mouse events on the icon arrive as this message on the owner window. */
#define V9X_TRAY_CALLBACK (WM_USER + 1u)
#define V9X_TRAY_CMD_SETTINGS 1
#define V9X_TRAY_CMD_RESTART 2
#define V9X_TRAY_CMD_REMOTE 3
#define V9X_TRAY_CMD_PAUSE 4
#define V9X_TRAY_CMD_CHAT 5
#define V9X_TRAY_CMD_UPDATE 6

/* The chat window's controls. */
#define V9X_CHAT_ID_LOG 101
#define V9X_CHAT_ID_INPUT 102

static const char v9x_tray_class[] = "V9xRemoteTray";
static const char v9x_chat_class[] = "V9xRemoteChat";
static const char v9x_tray_setup_exe[] = "C:\\V9XREMOTE\\SETUP.EXE";
static const char v9x_tray_setup_command[] = "C:\\V9XREMOTE\\SETUP.EXE /settings";
static const char v9x_tray_remote_command[] = "C:\\V9XREMOTE\\SETUP.EXE /remote";
static const char v9x_tray_update_command[] = "C:\\V9XREMOTE\\SETUP.EXE /update";

static V9xAgentState *v9x_tray_state;
static HWND v9x_chat_window;
static HWND v9x_chat_log;
static HWND v9x_chat_input;

/* Owned by the tray thread. The window procedure only records which menu
   command was picked; the steady-state loop acts on it, so starting SETUP.EXE
   and restarting happen in one place. */
static volatile int v9x_tray_request;
static HANDLE v9x_tray_setup_process;

/* The icon is a 16x16, 16-colour icon resource (BITMAPINFOHEADER, palette,
   4-bpp XOR image, 1-bpp AND mask) assembled in memory and handed to
   CreateIconFromResource. It is drawn by hand because the agent is forbidden
   from importing GDI32 (see build-guest.ps1), so CreateCompatibleBitmap and
   friends are not available; and CreateIcon's colour bitmaps are
   device-dependent, whereas an RT_ICON DIB is not. */
#define V9X_ICON_SIZE 16ul
#define V9X_ICON_XOR_STRIDE 8ul   /* 16 pixels x 4 bpp */
#define V9X_ICON_AND_STRIDE 4ul   /* 16 pixels x 1 bpp, padded to 4 bytes */
#define V9X_ICON_BYTES (40ul + 16ul * 4ul + \
                        V9X_ICON_SIZE * V9X_ICON_XOR_STRIDE + \
                        V9X_ICON_SIZE * V9X_ICON_AND_STRIDE)

/* Standard 16-colour palette indices used by the drawing. */
#define V9X_PAL_BLACK 0u
#define V9X_PAL_NAVY 4u
#define V9X_PAL_LIME 10u
#define V9X_PAL_WHITE 15u

static unsigned long v9x_tray_append(char *target, unsigned long offset,
                                     const char *text)
{
    while (*text != '\0' && offset < 63ul) {
        target[offset++] = *text++;
    }
    target[offset] = '\0';
    return offset;
}

static unsigned long v9x_tray_append_decimal(char *target,
                                             unsigned long offset,
                                             unsigned long value)
{
    char digits[10];
    unsigned long count = 0ul;
    do {
        digits[count++] = (char)('0' + (value % 10ul));
        value /= 10ul;
    } while (value != 0ul && count < sizeof(digits));
    while (count != 0ul && offset < 63ul) {
        target[offset++] = digits[--count];
    }
    target[offset] = '\0';
    return offset;
}

/* The tooltip holds 63 characters. While a command arrived in the last
   minute it says so, so the owner can tell someone is using the machine;
   with remote help on it uses a shorter form that also names its state. */
static void v9x_tray_tooltip(V9xAgentState *state,
                             const char *address, char *tooltip)
{
    unsigned long offset = 0ul;
    long age = v9x_last_command_age(state);
    tooltip[0] = '\0';
    if (age >= 0l && age < 60l) {
        offset = v9x_tray_append(tooltip, offset, "In use: last command ");
        offset = v9x_tray_append_decimal(tooltip, offset, (unsigned long)age);
        offset = v9x_tray_append(tooltip, offset, " s ago from ");
        (void)v9x_tray_append(tooltip, offset, state->last_command_source);
        return;
    }
    if (state->remote_on) {
        offset = v9x_tray_append(tooltip, offset, "Agent ");
        offset = v9x_tray_append(tooltip, offset, V9X_AGENT_VERSION);
        offset = v9x_tray_append(tooltip, offset, " | IP ");
        offset = v9x_tray_append(tooltip, offset, address);
        offset = v9x_tray_append(tooltip, offset, " | Remote help: ");
        (void)v9x_tray_append(tooltip, offset,
                              v9x_flag_read(&state->remote_paused) ? "paused" :
                              v9x_flag_read(&state->remote_session) ? "in session" :
                              v9x_flag_read(&state->remote_status) == V9X_REMOTE_ONLINE ? "on" :
                              "offline");
        return;
    }
    offset = v9x_tray_append(tooltip, offset, "Agent Version: ");
    offset = v9x_tray_append(tooltip, offset, V9X_AGENT_VERSION);
    offset = v9x_tray_append(tooltip, offset, " | Port: ");
    offset = v9x_tray_append_decimal(tooltip, offset, state->listen_port);
    offset = v9x_tray_append(tooltip, offset, " | IP: ");
    (void)v9x_tray_append(tooltip, offset, address);
}

/* Every tray failure used to be silent: v9x_tray_start returned 0 for three
   different reasons and the caller logged one undifferentiated
   "tray-icon-failed", while a failing Shell_NotifyIcon was retried forever
   with its result discarded. The icon was broken from the first release and
   the log could not say why, so each failure now names itself and carries
   GetLastError. Call this immediately after the failing call: any intervening
   API resets the thread's last-error value. */
static void v9x_tray_log_error(const char *event)
{
    DWORD error = GetLastError();
    char detail[80];
    unsigned long at = 0ul;
    detail[at++] = 'g';
    detail[at++] = 'l';
    detail[at++] = 'e';
    detail[at++] = '=';
    (void)v9x_tray_append_decimal(detail, at, (unsigned long)error);
    v9x_log_event(event, detail);
}

static unsigned long v9x_icon_put32(unsigned char *target, unsigned long at,
                                    unsigned long value)
{
    target[at++] = (unsigned char)(value & 0xFFul);
    target[at++] = (unsigned char)((value >> 8) & 0xFFul);
    target[at++] = (unsigned char)((value >> 16) & 0xFFul);
    target[at++] = (unsigned char)((value >> 24) & 0xFFul);
    return at;
}

static unsigned long v9x_icon_put16(unsigned char *target, unsigned long at,
                                    unsigned long value)
{
    target[at++] = (unsigned char)(value & 0xFFul);
    target[at++] = (unsigned char)((value >> 8) & 0xFFul);
    return at;
}

/* Palette index of pixel (x, y) in top-down coordinates, or 16 for
   transparent. The picture is the classic application window: a black
   outline, a navy title bar with a white button, and a body that is white
   when idle and lime while the agent is busy. */
static unsigned int v9x_icon_pixel(unsigned long x, unsigned long y,
                                   unsigned int body)
{
    if (x == 0ul || x == 15ul || y == 0ul || y == 15ul) return 16u;
    if (x == 1ul || x == 14ul || y == 1ul || y == 14ul) return V9X_PAL_BLACK;
    if (y <= 4ul) {
        return (y == 2ul && x == 13ul) ? V9X_PAL_WHITE : V9X_PAL_NAVY;
    }
    return body;
}

static unsigned long v9x_icon_build(unsigned char *out, unsigned int body)
{
    static const unsigned char palette[16][3] = {
        /* R, G, B */
        {0u, 0u, 0u}, {128u, 0u, 0u}, {0u, 128u, 0u}, {128u, 128u, 0u},
        {0u, 0u, 128u}, {128u, 0u, 128u}, {0u, 128u, 128u},
        {192u, 192u, 192u}, {128u, 128u, 128u}, {255u, 0u, 0u},
        {0u, 255u, 0u}, {255u, 255u, 0u}, {0u, 0u, 255u},
        {255u, 0u, 255u}, {0u, 255u, 255u}, {255u, 255u, 255u}
    };
    unsigned long at = 0ul;
    unsigned long row;
    unsigned long x;
    unsigned long y;
    unsigned int index;

    /* BITMAPINFOHEADER; biHeight covers the XOR image and the AND mask. */
    at = v9x_icon_put32(out, at, 40ul);
    at = v9x_icon_put32(out, at, V9X_ICON_SIZE);
    at = v9x_icon_put32(out, at, V9X_ICON_SIZE * 2ul);
    at = v9x_icon_put16(out, at, 1ul);
    at = v9x_icon_put16(out, at, 4ul);
    at = v9x_icon_put32(out, at, 0ul);
    at = v9x_icon_put32(out, at, V9X_ICON_SIZE * V9X_ICON_XOR_STRIDE +
                                 V9X_ICON_SIZE * V9X_ICON_AND_STRIDE);
    at = v9x_icon_put32(out, at, 0ul);
    at = v9x_icon_put32(out, at, 0ul);
    at = v9x_icon_put32(out, at, 0ul);
    at = v9x_icon_put32(out, at, 0ul);

    /* RGBQUAD palette: blue, green, red, reserved. */
    for (index = 0u; index < 16u; ++index) {
        out[at++] = palette[index][2];
        out[at++] = palette[index][1];
        out[at++] = palette[index][0];
        out[at++] = 0u;
    }

    /* XOR image, bottom-up, two pixels per byte, first pixel in the high
       nibble. Transparent pixels are black so the mask alone decides. */
    for (row = 0ul; row < V9X_ICON_SIZE; ++row) {
        y = V9X_ICON_SIZE - 1ul - row;
        for (x = 0ul; x < V9X_ICON_SIZE; x += 2ul) {
            unsigned int left = v9x_icon_pixel(x, y, body);
            unsigned int right = v9x_icon_pixel(x + 1ul, y, body);
            if (left > 15u) left = V9X_PAL_BLACK;
            if (right > 15u) right = V9X_PAL_BLACK;
            out[at++] = (unsigned char)((left << 4) | right);
        }
    }

    /* AND mask, bottom-up, one bit per pixel, MSB first, 1 = transparent. */
    for (row = 0ul; row < V9X_ICON_SIZE; ++row) {
        unsigned long bits = 0ul;
        y = V9X_ICON_SIZE - 1ul - row;
        for (x = 0ul; x < V9X_ICON_SIZE; ++x) {
            bits <<= 1;
            if (v9x_icon_pixel(x, y, body) > 15u) bits |= 1ul;
        }
        out[at++] = (unsigned char)((bits >> 8) & 0xFFul);
        out[at++] = (unsigned char)(bits & 0xFFul);
        out[at++] = 0u;
        out[at++] = 0u;
    }
    return at;
}

static HICON v9x_icon_create(unsigned int body)
{
    static unsigned char resource[V9X_ICON_BYTES];
    unsigned long length = v9x_icon_build(resource, body);
    return CreateIconFromResource(resource, (DWORD)length, TRUE, 0x00030000ul);
}

static int v9x_tray_notify(V9xAgentState *state, DWORD message,
                           const char *address, int busy)
{
    NOTIFYICONDATAA data;
    data.cbSize = NOTIFYICONDATAA_V1_SIZE;
    data.hWnd = state->tray_window;
    data.uID = V9X_TRAY_ICON_ID;
    data.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    data.uCallbackMessage = V9X_TRAY_CALLBACK;
    data.hIcon = busy ? state->tray_icon_busy : state->tray_icon;
    v9x_tray_tooltip(state, address, data.szTip);
    return Shell_NotifyIconA(message, &data) != FALSE;
}

static void v9x_tray_address(V9xAgentState *state, char *address)
{
    unsigned long value;
    struct in_addr host_address;
    char *resolved;

    lstrcpynA(address, state->listen_address, 16);
    if (v9x_primary_address(state, &value)) {
        host_address.s_addr = value;
        resolved = inet_ntoa(host_address);
        if (resolved != 0) lstrcpynA(address, resolved, 16);
    }
}

/* Sleep in slices, draining the message queue between them. The icon's owner
   window must belong to a thread that dispatches messages, otherwise the
   shell is talking to a window that never answers. */
static void v9x_tray_wait(unsigned long milliseconds)
{
    MSG message;
    unsigned long waited = 0ul;
    do {
        while (PeekMessageA(&message, 0, 0u, 0u, PM_REMOVE)) {
            /* Enter sends, Tab moves between the chat window's controls. */
            if (v9x_chat_window != 0 && IsDialogMessageA(v9x_chat_window, &message)) {
                continue;
            }
            (void)TranslateMessage(&message);
            (void)DispatchMessageA(&message);
        }
        Sleep(V9X_TRAY_PUMP_SLICE);
        waited += V9X_TRAY_PUMP_SLICE;
    } while (waited < milliseconds);
}

/* The right-click menu. SetForegroundWindow before TrackPopupMenu and the
   WM_NULL after it are the documented workaround for a tray menu that will
   not close when the user clicks elsewhere (Q135788). */
static void v9x_tray_menu(HWND window)
{
    HMENU menu = CreatePopupMenu();
    POINT at;
    UINT settings_flags = MF_STRING;
    char update_label[48];
    int chosen;
    if (menu == 0) return;
    if (GetFileAttributesA(v9x_tray_setup_exe) == 0xfffffffful) {
        settings_flags |= MF_GRAYED;
    }
    (void)AppendMenuA(menu, settings_flags, V9X_TRAY_CMD_SETTINGS, "&Settings...");
    (void)AppendMenuA(menu, settings_flags, V9X_TRAY_CMD_REMOTE, "Remote &help...");
    if (v9x_tray_state != 0 && v9x_tray_state->remote_on) {
        (void)AppendMenuA(menu, MF_STRING, V9X_TRAY_CMD_PAUSE,
                          v9x_flag_read(&v9x_tray_state->remote_paused) ?
                              "Res&ume remote help" : "&Pause remote help");
        (void)AppendMenuA(menu, MF_STRING, V9X_TRAY_CMD_CHAT, "&Chat with developer...");
    }
    update_label[0] = '\0';
    if (v9x_tray_state != 0 && v9x_tray_state->update_available[0] != '\0') {
        (void)v9x_tray_append(update_label, 0ul, "Install &update ");
        (void)v9x_tray_append(update_label, (unsigned long)lstrlenA(update_label),
                              v9x_tray_state->update_available);
        (void)v9x_tray_append(update_label, (unsigned long)lstrlenA(update_label), "...");
    } else {
        (void)v9x_tray_append(update_label, 0ul, "Check for &updates...");
    }
    (void)AppendMenuA(menu, settings_flags, V9X_TRAY_CMD_UPDATE, update_label);
    (void)AppendMenuA(menu, MF_SEPARATOR, 0u, 0);
    (void)AppendMenuA(menu, MF_STRING, V9X_TRAY_CMD_RESTART, "&Restart agent");
    (void)SetMenuDefaultItem(menu, V9X_TRAY_CMD_SETTINGS, FALSE);
    (void)GetCursorPos(&at);
    (void)SetForegroundWindow(window);
    chosen = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                 at.x, at.y, 0, window, 0);
    (void)PostMessageA(window, WM_NULL, 0, 0);
    (void)DestroyMenu(menu);
    if (chosen != 0) v9x_tray_request = chosen;
}

static LRESULT CALLBACK v9x_tray_proc(HWND window, UINT message, WPARAM wparam,
                                      LPARAM lparam)
{
    if (message == V9X_TRAY_CALLBACK) {
        if (lparam == WM_RBUTTONUP) {
            v9x_tray_menu(window);
        } else if (lparam == WM_LBUTTONDBLCLK) {
            v9x_tray_request = V9X_TRAY_CMD_SETTINGS;
        }
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

/* ---- chat window ---------------------------------------------------- */

static void v9x_chat_refresh(V9xAgentState *state)
{
    static char copy[V9X_CHAT_LOG_SIZE];
    int length;
    if (v9x_chat_log == 0) return;
    EnterCriticalSection(&state->chat_lock);
    lstrcpynA(copy, state->chat_log, (int)sizeof(copy));
    LeaveCriticalSection(&state->chat_lock);
    (void)SetWindowTextA(v9x_chat_log, copy);
    length = lstrlenA(copy);
    (void)SendMessageA(v9x_chat_log, EM_SETSEL, (WPARAM)length, (LPARAM)length);
    (void)SendMessageA(v9x_chat_log, EM_SCROLLCARET, 0, 0);
}

static void v9x_chat_send_input(void)
{
    char text[V9X_CHAT_LINE];
    if (v9x_chat_input == 0 || v9x_tray_state == 0) return;
    text[0] = '\0';
    (void)GetWindowTextA(v9x_chat_input, text, sizeof(text));
    if (text[0] == '\0') return;
    v9x_chat_send(v9x_tray_state, text);
    (void)SetWindowTextA(v9x_chat_input, "");
    v9x_chat_refresh(v9x_tray_state);
}

static LRESULT CALLBACK v9x_chat_proc(HWND window, UINT message, WPARAM wparam,
                                      LPARAM lparam)
{
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK) {
            v9x_chat_send_input();
            return 0;
        }
        if (LOWORD(wparam) == IDCANCEL) {
            (void)DestroyWindow(window);
            return 0;
        }
        break;
    case WM_SIZE:
        if (v9x_chat_log != 0) {
            int width = (int)LOWORD(lparam);
            int height = (int)HIWORD(lparam);
            (void)MoveWindow(v9x_chat_log, 6, 6, width - 12, height - 44, TRUE);
            (void)MoveWindow(v9x_chat_input, 6, height - 32, width - 92, 24, TRUE);
            (void)MoveWindow(GetDlgItem(window, IDOK), width - 80, height - 33, 74, 26, TRUE);
        }
        return 0;
    case WM_CLOSE:
        (void)DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        v9x_chat_window = 0;
        v9x_chat_log = 0;
        v9x_chat_input = 0;
        return 0;
    default:
        break;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

static void v9x_chat_open(V9xAgentState *state)
{
    static int registered = 0;
    HINSTANCE instance = GetModuleHandleA(0);
    WNDCLASSA klass;
    if (v9x_chat_window != 0) {
        (void)ShowWindow(v9x_chat_window, SW_SHOWNORMAL);
        (void)SetForegroundWindow(v9x_chat_window);
        v9x_chat_refresh(state);
        return;
    }
    if (!registered) {
        klass.style = 0u;
        klass.lpfnWndProc = v9x_chat_proc;
        klass.cbClsExtra = 0;
        klass.cbWndExtra = 0;
        klass.hInstance = instance;
        klass.hIcon = state->tray_icon;
        klass.hCursor = LoadCursorA(0, IDC_ARROW);
        klass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        klass.lpszMenuName = 0;
        klass.lpszClassName = v9x_chat_class;
        if (RegisterClassA(&klass) == 0) {
            v9x_tray_log_error("chat-class-failed");
            return;
        }
        registered = 1;
    }
    v9x_chat_window = CreateWindowExA(0ul, v9x_chat_class,
                                      "V9x Remote Agent - chat with the developer",
                                      WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                      CW_USEDEFAULT, CW_USEDEFAULT, 440, 320,
                                      0, 0, instance, 0);
    if (v9x_chat_window == 0) {
        v9x_tray_log_error("chat-window-failed");
        return;
    }
    v9x_chat_log = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                   ES_READONLY | ES_AUTOVSCROLL,
                                   6, 6, 410, 240, v9x_chat_window,
                                   (HMENU)V9X_CHAT_ID_LOG, instance, 0);
    v9x_chat_input = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     6, 252, 330, 24, v9x_chat_window,
                                     (HMENU)V9X_CHAT_ID_INPUT, instance, 0);
    (void)SendMessageA(v9x_chat_input, EM_LIMITTEXT, (WPARAM)(V9X_CHAT_LINE - 1), 0);
    (void)CreateWindowExA(0ul, "BUTTON", "Send",
                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                          342, 251, 74, 26, v9x_chat_window, (HMENU)IDOK, instance, 0);
    v9x_chat_refresh(state);
    (void)SetFocus(v9x_chat_input);
    (void)SetForegroundWindow(v9x_chat_window);
}

/* Called every pump slice: a message from the developer opens (or
   refreshes) the chat window and beeps. */
void v9x_tray_chat_service(V9xAgentState *state)
{
    if (InterlockedExchange(&state->chat_incoming, 0l) != 0l) {
        (void)MessageBeep(MB_ICONASTERISK);
        v9x_chat_open(state);
    }
}

static int v9x_tray_create_window(V9xAgentState *state)
{
    WNDCLASSA klass;
    klass.style = 0u;
    klass.lpfnWndProc = v9x_tray_proc;
    klass.cbClsExtra = 0;
    klass.cbWndExtra = 0;
    klass.hInstance = GetModuleHandleA(0);
    klass.hIcon = 0;
    klass.hCursor = 0;
    klass.hbrBackground = 0;
    klass.lpszMenuName = 0;
    klass.lpszClassName = v9x_tray_class;
    if (RegisterClassA(&klass) == 0) {
        v9x_tray_log_error("tray-class-failed");
        return 0;
    }
    state->tray_window = CreateWindowExA(0ul, v9x_tray_class, "V9x Remote Agent",
                                         WS_OVERLAPPED, 0, 0, 0, 0,
                                         0, 0, klass.hInstance, 0);
    if (state->tray_window == 0) {
        v9x_tray_log_error("tray-window-failed");
        return 0;
    }
    state->tray_icon = v9x_icon_create(V9X_PAL_WHITE);
    if (state->tray_icon != 0) {
        state->tray_icon_busy = v9x_icon_create(V9X_PAL_LIME);
    }
    if (state->tray_icon == 0 || state->tray_icon_busy == 0) {
        /* Fall back to the stock application icon, without activity
           feedback, rather than lose the tray icon altogether. */
        v9x_tray_log_error("tray-createicon-failed");
        if (state->tray_icon != 0) DestroyIcon(state->tray_icon);
        state->tray_icon = LoadIconA(0, IDI_APPLICATION);
        state->tray_icon_busy = state->tray_icon;
    }
    if (state->tray_icon == 0) {
        v9x_tray_log_error("tray-loadicon-failed");
        DestroyWindow(state->tray_window);
        state->tray_window = 0;
        return 0;
    }
    return 1;
}

/* One sample of the activity picture, taken every pump slice. The agent is
   busy while any client is connected or any execution is running; a frame
   in either direction also arms a short hold so that the briefest request
   still blinks. Returns 1 while the icon should be flashing. */
static int v9x_tray_sample_busy(V9xAgentState *state, LONG *last_activity,
                                unsigned long *hold_ms)
{
    LONG activity = v9x_flag_read(&state->activity);
    if (activity != *last_activity) {
        *last_activity = activity;
        *hold_ms = V9X_TRAY_HOLD_MS;
    } else if (*hold_ms > V9X_TRAY_PUMP_SLICE) {
        *hold_ms -= V9X_TRAY_PUMP_SLICE;
    } else {
        *hold_ms = 0ul;
    }
    if (*hold_ms != 0ul) return 1;
    if (v9x_flag_read(&state->active_connections) > 0l) return 1;
    return v9x_execution_any_active(state);
}

/* Restart: drop the icon (it would otherwise linger as a ghost until the
   mouse passes over it), say goodbye on mDNS, start a fresh agent and exit.
   This is how settings saved from the tray take effect: the agent reads
   AGENT.INI only at start. */
static void v9x_tray_restart(V9xAgentState *state)
{
    NOTIFYICONDATAA data;
    DWORD native_error = 0ul;
    v9x_log_event("tray-restart", "relaunching");
    data.cbSize = NOTIFYICONDATAA_V1_SIZE;
    data.hWnd = state->tray_window;
    data.uID = V9X_TRAY_ICON_ID;
    data.uFlags = 0u;
    (void)Shell_NotifyIconA(NIM_DELETE, &data);
    v9x_mdns_goodbye();
    if (!v9x_agent_relaunch(&native_error)) {
        /* Keep serving; the steady-state loop re-adds the icon. */
        SetLastError(native_error);
        v9x_tray_log_error("tray-restart-failed");
        return;
    }
    ExitProcess(0ul);
}

/* Act on a menu command and watch the SETUP.EXE the tray started. Setup in
   /settings mode exits 0 only after it saved, which restarts the agent. */
static void v9x_tray_service(V9xAgentState *state)
{
    int request = v9x_tray_request;
    DWORD code;
    DWORD native_error = 0ul;
    v9x_tray_request = 0;
    if (v9x_tray_setup_process != 0 &&
        WaitForSingleObject(v9x_tray_setup_process, 0ul) == WAIT_OBJECT_0) {
        code = 1ul;
        (void)GetExitCodeProcess(v9x_tray_setup_process, &code);
        CloseHandle(v9x_tray_setup_process);
        v9x_tray_setup_process = 0;
        v9x_log_event("tray-settings-closed", code == 0ul ? "saved" : "unchanged");
        if (code == 0ul) request = V9X_TRAY_CMD_RESTART;
    }
    if (request == V9X_TRAY_CMD_SETTINGS || request == V9X_TRAY_CMD_REMOTE ||
        request == V9X_TRAY_CMD_UPDATE) {
        const char *command =
            request == V9X_TRAY_CMD_REMOTE ? v9x_tray_remote_command :
            request == V9X_TRAY_CMD_UPDATE ? v9x_tray_update_command :
                                             v9x_tray_setup_command;
        if (v9x_tray_setup_process != 0) {
            HWND open = FindWindowA("V9xRemoteSetup", 0);
            if (open != 0) (void)SetForegroundWindow(open);
        } else if (!v9x_spawn(command, &v9x_tray_setup_process, &native_error)) {
            v9x_tray_setup_process = 0;
            SetLastError(native_error);
            v9x_tray_log_error("tray-settings-failed");
        } else {
            /* "SETUP.EXE /settings" and friends, without the folder. */
            v9x_log_event("tray-settings-opened", command + 13);
        }
    } else if (request == V9X_TRAY_CMD_PAUSE) {
        LONG paused = !v9x_flag_read(&state->remote_paused);
        InterlockedExchange(&state->remote_paused, paused);
        v9x_log_line(paused ? "remote-paused" : "remote-resumed");
        if (state->remote_wake != 0) (void)SetEvent(state->remote_wake);
    } else if (request == V9X_TRAY_CMD_CHAT) {
        v9x_chat_open(state);
    } else if (request == V9X_TRAY_CMD_RESTART) {
        v9x_tray_restart(state);
    }
}

static DWORD WINAPI v9x_tray_worker(LPVOID parameter)
{
    V9xAgentState *state = (V9xAgentState *)parameter;
    char address[16];
    int reported = 0;
    LONG last_activity;
    unsigned long hold_ms;
    unsigned long since_refresh;
    unsigned long flash_elapsed;
    int phase;
    int was_busy;
    int shown_busy;
    int want_busy;

    /* The owner window is created here, not by v9x_tray_start: this is the
       only thread in the agent that pumps messages, and a window belongs to
       the thread that created it. The main thread spends its life blocked in
       accept(). */
    if (!v9x_tray_create_window(state)) return 0ul;

    for (;;) {
        while (FindWindowA("Shell_TrayWnd", 0) == 0) v9x_tray_wait(1000ul);
        v9x_tray_address(state, address);
        if (!v9x_tray_notify(state, NIM_ADD, address, 0)) {
            if (!reported) {
                v9x_tray_log_error("tray-notify-failed");
                reported = 1;
            }
            v9x_tray_wait(1000ul);
            continue;
        }
        v9x_log_event("tray-icon-added", address);
        reported = 0;

        /* Steady state: sample activity every slice, swap the icon between
           the white and lime bodies at V9X_TRAY_FLASH_MS while busy, and
           re-assert the icon with a fresh address every V9X_TRAY_REFRESH_MS
           when idle. Any failed NIM_MODIFY or a vanished taskbar (Explorer
           restarted) drops back to NIM_ADD. */
        last_activity = v9x_flag_read(&state->activity);
        hold_ms = 0ul;
        since_refresh = 0ul;
        flash_elapsed = 0ul;
        phase = 0;
        was_busy = 0;
        shown_busy = 0;
        for (;;) {
            v9x_tray_wait(V9X_TRAY_PUMP_SLICE);
            since_refresh += V9X_TRAY_PUMP_SLICE;
            v9x_tray_service(state);
            v9x_tray_chat_service(state);
            if (v9x_tray_sample_busy(state, &last_activity, &hold_ms)) {
                if (!was_busy) {
                    /* Go lime on the first busy sample rather than waiting
                       out a half-period. */
                    phase = 1;
                    flash_elapsed = 0ul;
                } else {
                    flash_elapsed += V9X_TRAY_PUMP_SLICE;
                    if (flash_elapsed >= V9X_TRAY_FLASH_MS) {
                        flash_elapsed = 0ul;
                        phase = !phase;
                    }
                }
                was_busy = 1;
                want_busy = phase;
            } else {
                was_busy = 0;
                phase = 0;
                flash_elapsed = 0ul;
                want_busy = 0;
            }
            if (want_busy == shown_busy && since_refresh < V9X_TRAY_REFRESH_MS) {
                continue;
            }
            if (since_refresh >= V9X_TRAY_REFRESH_MS) {
                since_refresh = 0ul;
                v9x_tray_address(state, address);
            }
            if (FindWindowA("Shell_TrayWnd", 0) == 0 ||
                !v9x_tray_notify(state, NIM_MODIFY, address, want_busy)) {
                break;
            }
            shown_busy = want_busy;
        }
    }
}

int v9x_tray_start(V9xAgentState *state)
{
    HANDLE thread;
    /* lpThreadId must be a real pointer. Windows 9x rejects NULL here with
       ERROR_INVALID_PARAMETER, which is why the tray icon never once appeared
       on 98 SE: measured as "tray-thread-failed gle=87" on WIN98-S3NATIVE,
       4 Sep 2026. Windows NT accepts NULL, so this was invisible on a modern
       host. The agent's other two CreateThread calls always passed a
       pointer. */
    DWORD thread_id;
    v9x_tray_state = state;
    thread = CreateThread(0, 16384ul, v9x_tray_worker, state, 0ul, &thread_id);
    if (thread == 0) {
        v9x_tray_log_error("tray-thread-failed");
        return 0;
    }
    CloseHandle(thread);
    return 1;
}
