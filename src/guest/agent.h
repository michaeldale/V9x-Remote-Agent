#ifndef V9XREMOTE_AGENT_H
#define V9XREMOTE_AGENT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock.h>

typedef struct V9xAgentState {
    DWORD boot_counter;
    DWORD start_tick;
    volatile WORD winsock_version;
    SOCKET client_socket;
    CRITICAL_SECTION send_lock;
    volatile LONG exec_active;
    volatile LONG exec_cancel;
    DWORD exec_request_id;
    HANDLE exec_thread;
    char pending_job[64];
    WORD listen_port;
    char listen_address[16];
    char allowed_client[16];
    HWND tray_window;
    HICON tray_icon;
} V9xAgentState;

void v9x_agent_run(void);
int v9x_tray_start(V9xAgentState *state);
int v9x_serve_client(SOCKET client, V9xAgentState *state);
int v9x_send_frame(V9xAgentState *state, unsigned short type,
                   unsigned long request_id, const unsigned char *payload,
                   unsigned long length);
unsigned long v9x_execution_prepare(V9xAgentState *state,
                                    unsigned long request_id,
                                    const unsigned char *payload,
                                    unsigned long length);
int v9x_execution_resume(V9xAgentState *state);
int v9x_execution_cancel(V9xAgentState *state, unsigned long request_id);
void v9x_execution_disconnect(V9xAgentState *state);
int v9x_is_file_message(unsigned short type);
int v9x_handle_file_message(V9xAgentState *state, unsigned short type,
                            unsigned long request_id,
                            const unsigned char *payload,
                            unsigned long length);
void v9x_files_disconnect(void);
int v9x_handle_power_message(V9xAgentState *state, unsigned short type,
                             unsigned long request_id,
                             const unsigned char *payload,
                             unsigned long length);
void v9x_load_pending_job(V9xAgentState *state);
int v9x_capture_screenshot(V9xAgentState *state, unsigned long request_id,
                           const unsigned char *payload,
                           unsigned long length);
int v9x_handle_input(V9xAgentState *state, unsigned long request_id,
                     const unsigned char *payload, unsigned long length);
int v9x_desktop_ready(void);
void v9x_screen_info(DWORD *width, DWORD *height, DWORD *bits_per_pixel);
void v9x_log_line(const char *event_name);

#endif
