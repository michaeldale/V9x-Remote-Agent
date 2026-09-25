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

static void v9x_tray_tooltip(const V9xAgentState *state,
                             const char *address, char *tooltip)
{
    unsigned long offset = 0ul;
    tooltip[0] = '\0';
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
    data.uFlags = NIF_ICON | NIF_TIP;
    data.uCallbackMessage = 0u;
    data.hIcon = busy ? state->tray_icon_busy : state->tray_icon;
    v9x_tray_tooltip(state, address, data.szTip);
    return Shell_NotifyIconA(message, &data) != FALSE;
}

static void v9x_tray_address(V9xAgentState *state, char *address)
{
    char host_name[128];
    struct hostent *host;
    struct in_addr host_address;
    char *resolved;

    lstrcpynA(address, state->listen_address, 16);
    if (lstrcmpA(state->listen_address, "0.0.0.0") != 0) {
        return;
    }
    if (state->winsock_version != 0u &&
        gethostname(host_name, sizeof(host_name)) == 0) {
        host = gethostbyname(host_name);
        if (host != 0 && host->h_addrtype == AF_INET &&
            host->h_length == sizeof(host_address) &&
            host->h_addr_list != 0 && host->h_addr_list[0] != 0) {
            host_address.s_addr = *(unsigned long *)host->h_addr_list[0];
            resolved = inet_ntoa(host_address);
            if (resolved != 0) lstrcpynA(address, resolved, 16);
        }
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
            (void)DispatchMessageA(&message);
        }
        Sleep(V9X_TRAY_PUMP_SLICE);
        waited += V9X_TRAY_PUMP_SLICE;
    } while (waited < milliseconds);
}

static int v9x_tray_create_window(V9xAgentState *state)
{
    state->tray_window = CreateWindowExA(0ul, "STATIC", "V9x Remote Agent",
                                         WS_OVERLAPPED, 0, 0, 0, 0,
                                         0, 0, 0, 0);
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
    thread = CreateThread(0, 16384ul, v9x_tray_worker, state, 0ul, &thread_id);
    if (thread == 0) {
        v9x_tray_log_error("tray-thread-failed");
        return 0;
    }
    CloseHandle(thread);
    return 1;
}
