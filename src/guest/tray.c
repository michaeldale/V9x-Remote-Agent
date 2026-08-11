#include "agent.h"
#include "v9xremote/version.h"
#include <shellapi.h>

#define V9X_TRAY_ICON_ID 1u

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

static int v9x_tray_notify(V9xAgentState *state, DWORD message,
                           const char *address)
{
    NOTIFYICONDATAA data;
    data.cbSize = NOTIFYICONDATAA_V1_SIZE;
    data.hWnd = state->tray_window;
    data.uID = V9X_TRAY_ICON_ID;
    data.uFlags = NIF_ICON | NIF_TIP;
    data.uCallbackMessage = 0u;
    data.hIcon = state->tray_icon;
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

static DWORD WINAPI v9x_tray_worker(LPVOID parameter)
{
    V9xAgentState *state = (V9xAgentState *)parameter;
    char address[16];

    for (;;) {
        while (FindWindowA("Shell_TrayWnd", 0) == 0) Sleep(1000ul);
        v9x_tray_address(state, address);
        if (!v9x_tray_notify(state, NIM_ADD, address)) {
            Sleep(1000ul);
            continue;
        }
        do {
            Sleep(5000ul);
            v9x_tray_address(state, address);
        } while (FindWindowA("Shell_TrayWnd", 0) != 0 &&
                 v9x_tray_notify(state, NIM_MODIFY, address));
    }
}

int v9x_tray_start(V9xAgentState *state)
{
    HANDLE thread;
    state->tray_window = CreateWindowExA(0ul, "STATIC", "V9x Remote Agent",
                                         WS_OVERLAPPED, 0, 0, 0, 0,
                                         0, 0, 0, 0);
    if (state->tray_window == 0) return 0;
    state->tray_icon = LoadIconA(0, IDI_APPLICATION);
    if (state->tray_icon == 0) {
        DestroyWindow(state->tray_window);
        state->tray_window = 0;
        return 0;
    }
    thread = CreateThread(0, 16384ul, v9x_tray_worker, state, 0ul, 0);
    if (thread == 0) {
        DestroyWindow(state->tray_window);
        state->tray_window = 0;
        return 0;
    }
    CloseHandle(thread);
    return 1;
}
