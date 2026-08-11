#include "agent.h"

static const char v9x_mutex_name[] = "V9xRemoteAgent.Singleton";
static const char v9x_root[] = "C:\\V9XREMOTE";
static const char v9x_boot_path[] = "C:\\V9XREMOTE\\BOOT.DAT";
static const char v9x_temp_root[] = "C:\\V9XREMOTE\\TEMP";
static const char v9x_jobs_root[] = "C:\\V9XREMOTE\\JOBS";
static const char v9x_config_path[] = "C:\\V9XREMOTE\\AGENT.INI";

static void v9x_load_network_config(V9xAgentState *state)
{
    UINT port;
    port = GetPrivateProfileIntA("agent", "port", 9869, v9x_config_path);
    if (port == 0u || port > 65535u) port = 9869u;
    state->listen_port = (WORD)port;
    (void)GetPrivateProfileStringA("agent", "bind_address", "0.0.0.0",
                                   state->listen_address,
                                   sizeof(state->listen_address),
                                   v9x_config_path);
    (void)GetPrivateProfileStringA("agent", "allowed_client", "",
                                   state->allowed_client,
                                   sizeof(state->allowed_client),
                                   v9x_config_path);
}

static DWORD v9x_increment_boot_counter(void)
{
    HANDLE file;
    DWORD value = 0ul;
    DWORD transferred = 0ul;
    (void)CreateDirectoryA(v9x_root, 0);
    (void)CreateDirectoryA(v9x_temp_root, 0);
    (void)CreateDirectoryA(v9x_jobs_root, 0);
    file = CreateFileA(v9x_boot_path, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ, 0, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        return 0ul;
    }
    if (!ReadFile(file, &value, sizeof(value), &transferred, 0) ||
        transferred != sizeof(value)) {
        value = 0ul;
    }
    ++value;
    (void)SetFilePointer(file, 0l, 0, FILE_BEGIN);
    transferred = 0ul;
    (void)WriteFile(file, &value, sizeof(value), &transferred, 0);
    (void)SetEndOfFile(file);
    (void)FlushFileBuffers(file);
    CloseHandle(file);
    return value;
}

void v9x_agent_run(void)
{
    HANDLE mutex;
    WSADATA data;
    SOCKET listener;
    SOCKET client;
    static struct sockaddr_in address;
    static struct sockaddr_in client_address;
    V9xAgentState state;
    unsigned long bind_address;
    unsigned long allowed_client;
    int restrict_client;
    int client_address_length;
    int retry;

    mutex = CreateMutexA(0, FALSE, v9x_mutex_name);
    if (mutex == 0 || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex != 0) CloseHandle(mutex);
        return;
    }

    state.boot_counter = v9x_increment_boot_counter();
    state.start_tick = GetTickCount();
    state.winsock_version = 0u;
    state.client_socket = INVALID_SOCKET;
    state.exec_active = 0l;
    state.exec_cancel = 0l;
    state.exec_request_id = 0ul;
    state.exec_thread = 0;
    state.pending_job[0] = '\0';
    state.listen_port = 9869u;
    state.listen_address[0] = '\0';
    state.allowed_client[0] = '\0';
    state.tray_window = 0;
    state.tray_icon = 0;
    v9x_load_network_config(&state);
    v9x_load_pending_job(&state);
    InitializeCriticalSection(&state.send_lock);
    v9x_log_line("agent-start");
    if (!v9x_tray_start(&state)) v9x_log_line("tray-icon-failed");

    for (;;) {
        for (retry = 0; retry < 30; ++retry) {
            if (WSAStartup(MAKEWORD(1, 1), &data) == 0) {
                state.winsock_version = data.wVersion;
                break;
            }
            Sleep(2000ul);
        }
        if (retry == 30) {
            v9x_log_line("winsock-start-failed");
            Sleep(10000ul);
            continue;
        }

        bind_address = inet_addr(state.listen_address);
        if (bind_address == INADDR_NONE) {
            v9x_log_line("bind-address-invalid");
            WSACleanup();
            Sleep(10000ul);
            continue;
        }
        restrict_client = state.allowed_client[0] != '\0';
        allowed_client = restrict_client ? inet_addr(state.allowed_client) : 0ul;
        if (restrict_client && allowed_client == INADDR_NONE) {
            v9x_log_line("allowed-client-invalid");
            WSACleanup();
            Sleep(10000ul);
            continue;
        }

        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) {
            WSACleanup();
            Sleep(5000ul);
            continue;
        }
        address.sin_family = AF_INET;
        address.sin_port = htons(state.listen_port);
        address.sin_addr.s_addr = bind_address;
        if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR ||
            listen(listener, 1) == SOCKET_ERROR) {
            closesocket(listener);
            WSACleanup();
            v9x_log_line("listen-failed");
            Sleep(5000ul);
            continue;
        }
        v9x_log_line("listening");
        for (;;) {
            client_address_length = sizeof(client_address);
            client = accept(listener, (struct sockaddr *)&client_address,
                            &client_address_length);
            if (client == INVALID_SOCKET) {
                break;
            }
            if (restrict_client &&
                client_address.sin_addr.s_addr != allowed_client) {
                v9x_log_line("client-rejected");
                closesocket(client);
                continue;
            }
            v9x_log_line("client-connected");
            state.client_socket = client;
            (void)v9x_serve_client(client, &state);
            v9x_execution_disconnect(&state);
            v9x_files_disconnect();
            state.client_socket = INVALID_SOCKET;
            closesocket(client);
            v9x_log_line("client-disconnected");
        }
        closesocket(listener);
        WSACleanup();
        Sleep(2000ul);
    }
}
