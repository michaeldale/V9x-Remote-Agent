#include "agent.h"
#include "installid_win.h"
#include "platform_win.h"

static const char v9x_mutex_name[] = V9X_AGENT_MUTEX_NAME;
static const char v9x_root[] = "C:\\V9XREMOTE";
static const char v9x_boot_path[] = "C:\\V9XREMOTE\\BOOT.DAT";
static const char v9x_temp_root[] = "C:\\V9XREMOTE\\TEMP";
static const char v9x_jobs_root[] = "C:\\V9XREMOTE\\JOBS";
static const char v9x_config_path[] = "C:\\V9XREMOTE\\AGENT.INI";

/* One static context per concurrent connection. Static (not heap) so a slot is
   never freed while an execution worker that captured it might still send into
   it; `in_use` is both the allocator and the lifecycle marker. */
static V9xConnection v9x_connections[V9X_MAX_CONNECTIONS];

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
    state->allow_dos_vm =
        GetPrivateProfileIntA("agent", "allow_dos_vm", 1, v9x_config_path) != 0;
    state->discovery =
        GetPrivateProfileIntA("agent", "discovery", 0, v9x_config_path) != 0;
    {
        UINT seconds = GetPrivateProfileIntA("agent", "idle_timeout", 300,
                                             v9x_config_path);
        if (seconds > 86400u) seconds = 86400u;
        state->idle_timeout_ms = (DWORD)seconds * 1000ul;
    }
    switch (v9x_install_id_ensure(v9x_config_path, state->install_id)) {
    case 1: v9x_log_event("install-id-created", state->install_id); break;
    case -1: v9x_log_event("install-id-unsaved", state->install_id); break;
    default: break;
    }
}

/* The address the agent is reachable on: the bind address when one is set,
   otherwise the first address the host name resolves to. 0 when Winsock is
   not up or nothing resolves. */
int v9x_primary_address(const V9xAgentState *state, unsigned long *address)
{
    char host_name[128];
    struct hostent *host;
    if (lstrcmpA(state->listen_address, "0.0.0.0") != 0) {
        *address = inet_addr(state->listen_address);
        return *address != INADDR_NONE;
    }
    if (state->winsock_version == 0u ||
        gethostname(host_name, sizeof(host_name)) != 0) {
        return 0;
    }
    host = gethostbyname(host_name);
    if (host == 0 || host->h_addrtype != AF_INET ||
        host->h_length != sizeof(*address) || host->h_addr_list == 0 ||
        host->h_addr_list[0] == 0) {
        return 0;
    }
    *address = *(unsigned long *)host->h_addr_list[0];
    return 1;
}

/* Advertised capabilities. EXEC_SHELL is withdrawn when DOS VMs are
   disallowed so a client fails before sending anything rather than after.
   On NT the shell is CMD.EXE, a native Win32 program, so it stays. */
unsigned long v9x_capabilities(const V9xAgentState *machine)
{
    unsigned long caps = V9X_CAPABILITIES;
    if (!machine->allow_dos_vm && !v9x_platform_is_nt()) caps &= ~V9X_CAP_EXEC_SHELL;
    return caps;
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

/* Claim a free connection slot for an accepted socket. Returns the slot or 0 if
   every slot is in use (the caller then rejects the connection). */
static V9xConnection *v9x_claim_connection(V9xAgentState *state, SOCKET client)
{
    int index;
    for (index = 0; index < V9X_MAX_CONNECTIONS; ++index) {
        V9xConnection *conn = &v9x_connections[index];
        if (v9x_flag_acquire(&conn->in_use)) {
            conn->index = index;
            conn->socket = client;
            conn->machine = state;
            conn->thread = 0;
            conn->pending_exec = 0;
            {
                unsigned long i;
                for (i = 0ul; i < sizeof(conn->write_state); ++i) {
                    ((unsigned char *)&conn->write_state)[i] = 0u;
                }
            }
            InitializeCriticalSection(&conn->send_lock);
            InterlockedIncrement(&state->active_connections);
            return conn;
        }
    }
    return 0;
}

/* One thread per connection: serve until the client disconnects, then tear down
   this connection's execution slots and upload state before releasing the slot.
   `in_use` is cleared LAST so the slot cannot be recycled while a worker that
   owns it is still running. */
static DWORD WINAPI v9x_connection_thread(LPVOID parameter)
{
    V9xConnection *conn = (V9xConnection *)parameter;
    (void)v9x_serve_client(conn);
    v9x_execution_disconnect(conn);
    v9x_files_disconnect(conn);
    closesocket(conn->socket);
    conn->socket = INVALID_SOCKET;
    DeleteCriticalSection(&conn->send_lock);
    v9x_log_line("client-disconnected");
    InterlockedDecrement(&conn->machine->active_connections);
    InterlockedExchange(&conn->in_use, 0l);
    return 0ul;
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
    V9xConnection *conn;
    HANDLE thread;
    DWORD thread_id;
    unsigned long bind_address;
    unsigned long allowed_client;
    int restrict_client;
    int client_address_length;
    int retry;
    int slot;

    /* Singleton guard, tolerant of a hot-update handover. When a hot update
       swaps the binary, the new agent is launched while the outgoing process is
       still tearing down, so its mutex object can briefly linger. Retry for a
       few seconds before conceding, otherwise the relaunched agent would see
       ERROR_ALREADY_EXISTS against a dying predecessor and exit silently (before
       it can even log), leaving the guest with no agent. A genuine second
       instance simply waits out the retries and then exits. */
    {
        int mutex_try;
        mutex = 0;
        for (mutex_try = 0; mutex_try < 10; ++mutex_try) {
            mutex = CreateMutexA(0, FALSE, v9x_mutex_name);
            if (mutex != 0 && GetLastError() != ERROR_ALREADY_EXISTS) break;
            if (mutex != 0) { CloseHandle(mutex); mutex = 0; }
            Sleep(500ul);
        }
    }
    if (mutex == 0 || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex != 0) CloseHandle(mutex);
        return;
    }

    state.boot_counter = v9x_increment_boot_counter();
    v9x_log_set_boot(state.boot_counter);
    state.start_tick = GetTickCount();
    state.winsock_version = 0u;
    for (slot = 0; slot < V9X_EXEC_POOL_SIZE; ++slot) {
        state.exec_pool[slot].active = 0l;
        state.exec_pool[slot].cancel = 0l;
        state.exec_pool[slot].owner = 0;
        state.exec_pool[slot].thread = 0;
        state.exec_pool[slot].request_id = 0ul;
    }
    state.pending_job[0] = '\0';
    state.listen_port = 9869u;
    state.listen_address[0] = '\0';
    state.allowed_client[0] = '\0';
    state.allow_dos_vm = 1;
    state.install_id[0] = '\0';
    state.discovery = 0;
    state.discovery_status = V9X_DISCOVERY_OFF;
    state.idle_timeout_ms = 300000ul;
    state.active_connections = 0l;
    state.activity = 0l;
    state.tray_window = 0;
    state.tray_icon = 0;
    state.tray_icon_busy = 0;
    for (slot = 0; slot < V9X_MAX_CONNECTIONS; ++slot) {
        v9x_connections[slot].in_use = 0l;
        v9x_connections[slot].socket = INVALID_SOCKET;
    }
    v9x_load_network_config(&state);
    v9x_load_pending_job(&state);
    InitializeCriticalSection(&state.screenshot_lock);
    v9x_log_line("agent-start");
    if (!state.allow_dos_vm) v9x_log_line("dos-vm-disallowed");
    v9x_hotswap_cleanup();
    if (!v9x_tray_start(&state)) v9x_log_line("tray-icon-failed");
    if (state.discovery && !v9x_mdns_start(&state)) v9x_log_line("mdns-thread-failed");

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
        v9x_platform_no_inherit((HANDLE)listener);
        address.sin_family = AF_INET;
        address.sin_port = htons(state.listen_port);
        address.sin_addr.s_addr = bind_address;
        if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR ||
            listen(listener, V9X_MAX_CONNECTIONS) == SOCKET_ERROR) {
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
            v9x_platform_no_inherit((HANDLE)client);
            if (restrict_client &&
                client_address.sin_addr.s_addr != allowed_client) {
                v9x_log_line("client-rejected");
                closesocket(client);
                continue;
            }
            conn = v9x_claim_connection(&state, client);
            if (conn == 0) {
                v9x_log_line("client-overflow");
                closesocket(client);
                continue;
            }
            v9x_log_line("client-connected");
            thread = CreateThread(0, 0ul, v9x_connection_thread, conn, 0ul,
                                  &thread_id);
            if (thread == 0) {
                v9x_log_line("client-thread-failed");
                closesocket(conn->socket);
                conn->socket = INVALID_SOCKET;
                DeleteCriticalSection(&conn->send_lock);
                InterlockedDecrement(&state.active_connections);
                InterlockedExchange(&conn->in_use, 0l);
                continue;
            }
            conn->thread = thread;
            CloseHandle(thread);
        }
        closesocket(listener);
        WSACleanup();
        Sleep(2000ul);
    }
}
