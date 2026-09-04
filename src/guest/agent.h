#ifndef V9XREMOTE_AGENT_H
#define V9XREMOTE_AGENT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock.h>
#include "v9xremote/protocol.h"

#define V9X_EXEC_POOL_SIZE 4
#define V9X_MAX_CONNECTIONS 4
#define V9X_LIST_RESPONSE_MAX 16384ul

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
    HWND tray_window;
    HICON tray_icon;
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
int v9x_tray_start(V9xAgentState *state);
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
int v9x_capture_screenshot(V9xConnection *conn, unsigned long request_id,
                           const unsigned char *payload,
                           unsigned long length);
int v9x_handle_input(V9xConnection *conn, unsigned long request_id,
                     const unsigned char *payload, unsigned long length);
int v9x_handle_download(V9xConnection *conn, unsigned long request_id,
                        const unsigned char *payload, unsigned long length);
int v9x_handle_update(V9xConnection *conn, unsigned long request_id,
                      const unsigned char *payload, unsigned long length);
int v9x_desktop_ready(void);
void v9x_screen_info(DWORD *width, DWORD *height, DWORD *bits_per_pixel);
void v9x_log_line(const char *event_name);
void v9x_log_event(const char *event_name, const char *detail);
void v9x_log_set_boot(DWORD boot_counter);

#endif
