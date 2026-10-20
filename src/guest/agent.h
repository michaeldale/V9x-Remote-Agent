#ifndef V9XREMOTE_AGENT_H
#define V9XREMOTE_AGENT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock.h>
#include "v9xremote/protocol.h"
#include "v9xremote/installid.h"

#define V9X_EXEC_POOL_SIZE 4
#define V9X_DISCOVERY_OFF 0l
#define V9X_DISCOVERY_ACTIVE 1l
#define V9X_DISCOVERY_UNAVAILABLE 2l
#define V9X_MAX_CONNECTIONS 4
#define V9X_LIST_RESPONSE_MAX 16384ul
#define V9X_CHAT_LOG_SIZE 8192ul
#define V9X_CHAT_QUEUE 4
#define V9X_CHAT_LINE 512
#define V9X_REMOTE_OFF 0l
#define V9X_REMOTE_CONNECTING 1l
#define V9X_REMOTE_ONLINE 2l
#define V9X_REMOTE_UNREACHABLE 3l
#define V9X_REMOTE_STOPPED 4l
/* Singleton mutex; the hot-update swapper also waits on it (see update.c). */
#define V9X_AGENT_MUTEX_NAME "V9xRemoteAgent.Singleton"

/* Windows 95's KERNEL32 does not export InterlockedCompareExchange (it arrived
   in Windows 98 and NT4), so the agent must never import it. Every
   compare-exchange the agent needs is either a 0/1 flag acquire or a plain
   read of the current value: an acquire is InterlockedExchange (exported since
   Windows 95) - writing 1 when the flag is already 1 changes nothing and the
   old value still tells us who won - and a read of an aligned volatile LONG is
   atomic on x86 without any Interlocked call at all. */
#define v9x_flag_acquire(flag) (InterlockedExchange((flag), 1l) == 0l)
#define v9x_flag_read(flag) (*(flag))

struct V9xAgentState;
struct V9xConnection;

/* One slot in the concurrent execution pool (shared machine-wide). Each slot
   carries its own worker thread, cancel flag, and the full job payload, so
   several EXEC_REQUESTs can run at once. `owner` is the connection that
   receives this slot's STDOUT/STDERR/COMPLETE frames; it also scopes
   cancellation, since a request id is unique only per connection and must never
   be matched against a different owner. */
typedef struct V9xExecSlot {
    volatile LONG active;
    volatile LONG cancel;
    struct V9xConnection *owner;
    HANDLE thread;
    DWORD request_id;
    DWORD timeout_ms;
    DWORD stdout_limit;
    DWORD stderr_limit;
    BYTE mode;
    BYTE show_window;
    unsigned short options;
    char application[260];
    char command[2048];
    char directory[260];
} V9xExecSlot;

/* In-flight upload state, one per connection (was a files.c module static). */
typedef struct V9xWriteState {
    HANDLE handle;
    DWORD expected_size;
    DWORD expected_crc;
    DWORD received;
    DWORD running_crc;
    char final_path[260];
    char temp_path[260];
} V9xWriteState;

/* Shared, machine-global state: one instance for the whole agent. Holds
   configuration, the boot counter, the tray, and the execution pool. It never
   holds a client socket - that is per-connection. */
typedef struct V9xAgentState {
    DWORD boot_counter;
    DWORD start_tick;
    volatile WORD winsock_version;
    V9xExecSlot exec_pool[V9X_EXEC_POOL_SIZE];
    CRITICAL_SECTION screenshot_lock;
    char pending_job[64];
    WORD listen_port;
    char listen_address[16];
    char allowed_client[16];
    /* AGENT.INI [agent] allow_dos_vm (default 1). When 0 the agent refuses
       anything that would make Windows create a DOS virtual machine: shell
       requests (COMMAND.COM), and direct execution of console-subsystem or
       DOS programs. Some display drivers corrupt the screen whenever a VM
       starts, even a hidden one. */
    int allow_dos_vm;
    /* AGENT.INI [agent] install_id: generated on first start when missing,
       so every install carries one whichever way it was installed. */
    char install_id[V9X_INSTALL_ID_SIZE];
    /* AGENT.INI [agent] discovery (default 0). When 1 an mDNS/DNS-SD
       responder advertises _v9xremote._tcp on the LAN. `discovery_status`
       is V9X_DISCOVERY_OFF, _ACTIVE or _UNAVAILABLE (bind or join failed). */
    int discovery;
    volatile LONG discovery_status;
    /* AGENT.INI [agent] idle_timeout in seconds (default 300, 0 = never).
       A connection that sends nothing for this long is closed, unless it
       owns a running EXEC (the host then only receives). Without it a
       client that vanished without a FIN or RST pinned a connection slot
       and kept the tray icon busy forever. */
    DWORD idle_timeout_ms;
    /* Activity picture for the tray icon. `active_connections` counts claimed
       connection slots; `activity` is bumped for every frame received or sent,
       and only ever compared for change, so wrap-around is harmless. */
    volatile LONG active_connections;
    volatile LONG activity;
    HWND tray_window;
    HICON tray_icon;
    HICON tray_icon_busy;
    /* Remote help (remote.c, docsemote-help.md). `remote_on` is AGENT.INI
       [remote] mode=on; `remote_paused` is the tray's Pause, deliberately
       never saved so a restart is back to On; `remote_status` is one of
       V9X_REMOTE_*; `remote_session` is 1 while a relay session is open.
       Setting `remote_wake` makes the worker check in now. */
    int remote_on;
    volatile LONG remote_paused;
    volatile LONG remote_status;
    volatile LONG remote_session;
    HANDLE remote_wake;
    char remote_machine[16];
    /* The last request other than HELLO and PING, from any client or the
       relay: what the tray, the check-in and SETUP's /remote page show so
       an owner can tell the machine is in use. */
    volatile LONG last_command_set;
    DWORD last_command_tick;
    unsigned short last_command_type;
    char last_command_source[16];
    /* Chat with the developer. `chat_log` is the conversation the tray's
       chat window shows; `chat_incoming` is set when the worker appended a
       message from the developer; `chat_outgoing` holds the owner's lines
       until the next check-in delivers them. All under `chat_lock`. */
    CRITICAL_SECTION chat_lock;
    char chat_log[V9X_CHAT_LOG_SIZE];
    unsigned long chat_log_length;
    volatile LONG chat_incoming;
    char chat_outgoing[V9X_CHAT_QUEUE][V9X_CHAT_LINE];
    int chat_outgoing_count;
    /* AGENT.INI [update] check_on_startup, and the newer version a quiet
       check found (empty when none). */
    int update_check_on_startup;
    char update_available[24];
} V9xAgentState;

/* Per-connection context: one static slot per concurrent client. Owns the
   socket, its own send lock, and every receive/response/file scratch buffer, so
   connections never share mutable state. `in_use` is both the slot allocator
   and the lifecycle marker - it is cleared only after this connection's exec
   workers have all exited, so no worker can send into a recycled slot. */
typedef struct V9xConnection {
    volatile LONG in_use;
    int index;
    SOCKET socket;
    CRITICAL_SECTION send_lock;
    struct V9xAgentState *machine;
    HANDLE thread;
    V9xExecSlot *pending_exec;
    /* 1 for the loopback connection a remote-help relay session runs on. */
    int is_relay;
    char peer[16];
    V9xWriteState write_state;
    unsigned char payload[V9X_MAX_PAYLOAD];
    unsigned char header_bytes[V9X_HEADER_SIZE];
    unsigned char response[2048];
    unsigned char file_response[V9X_LIST_RESPONSE_MAX];
    unsigned char file_buffer[V9X_FILE_CHUNK_SIZE + 4ul];
    unsigned char screen_response[320];
    unsigned char input_response[16];
    unsigned char power_response[96];
} V9xConnection;

void v9x_agent_run(void);
unsigned long v9x_capabilities(const V9xAgentState *machine);
int v9x_hotswap_run(void);
void v9x_hotswap_cleanup(void);
int v9x_tray_start(V9xAgentState *state);
int v9x_spawn(const char *command, HANDLE *process_out, DWORD *native_error);
int v9x_agent_relaunch(DWORD *native_error);
int v9x_mdns_start(V9xAgentState *state);
void v9x_mdns_goodbye(void);
int v9x_start_connection(V9xAgentState *state, SOCKET client, int is_relay);
void v9x_remote_load_config(V9xAgentState *state);
int v9x_remote_start(V9xAgentState *state);
int v9x_update_check_start(V9xAgentState *state);
void v9x_note_command(V9xConnection *conn, unsigned short type);
long v9x_last_command_age(V9xAgentState *state);
void v9x_chat_send(V9xAgentState *state, const char *text);
void v9x_chat_append(V9xAgentState *state, const char *who, const char *text);
void v9x_tray_chat_service(V9xAgentState *state);
int v9x_primary_address(const V9xAgentState *state, unsigned long *address);
int v9x_serve_client(V9xConnection *conn);
int v9x_send_frame(V9xConnection *conn, unsigned short type,
                   unsigned long request_id, const unsigned char *payload,
                   unsigned long length);
unsigned long v9x_execution_prepare(V9xConnection *conn,
                                    unsigned long request_id,
                                    const unsigned char *payload,
                                    unsigned long length);
int v9x_execution_resume(V9xConnection *conn);
int v9x_execution_cancel(V9xConnection *conn, unsigned long request_id);
void v9x_execution_disconnect(V9xConnection *conn);
int v9x_execution_any_active(V9xAgentState *machine);
int v9x_execution_owned(const V9xConnection *conn);
int v9x_is_file_message(unsigned short type);
int v9x_handle_file_message(V9xConnection *conn, unsigned short type,
                            unsigned long request_id,
                            const unsigned char *payload,
                            unsigned long length);
void v9x_files_disconnect(V9xConnection *conn);
unsigned long v9x_promote_temp(const char *final_path, const char *temp_path,
                               DWORD *native_error);
int v9x_handle_power_message(V9xConnection *conn, unsigned short type,
                             unsigned long request_id,
                             const unsigned char *payload,
                             unsigned long length);
void v9x_load_pending_job(V9xAgentState *state);
void v9x_reset_machine(void);
int v9x_capture_screenshot(V9xConnection *conn, unsigned long request_id,
                           const unsigned char *payload,
                           unsigned long length);
int v9x_handle_input(V9xConnection *conn, unsigned long request_id,
                     const unsigned char *payload, unsigned long length);
int v9x_handle_download(V9xConnection *conn, unsigned long request_id,
                        const unsigned char *payload, unsigned long length);
int v9x_handle_update(V9xConnection *conn, unsigned long request_id,
                      const unsigned char *payload, unsigned long length);
int v9x_handle_inventory(V9xConnection *conn, unsigned long request_id);
int v9x_desktop_ready(void);
void v9x_screen_info(DWORD *width, DWORD *height, DWORD *bits_per_pixel);
void v9x_log_line(const char *event_name);
void v9x_log_event(const char *event_name, const char *detail);
void v9x_log_set_boot(DWORD boot_counter);
void v9x_log_freeze(int frozen);

#endif
