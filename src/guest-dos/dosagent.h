#ifndef V9XREMOTE_DOSAGENT_H
#define V9XREMOTE_DOSAGENT_H

/* DOS build of the v9x remote agent.

   This is the cut-down, DOS/4GW (32-bit protected mode) variant. It speaks the
   identical wire protocol as the Win9x agent so the same host tooling
   (scripts\v9xctl.ps1 and the Python MCP server) drives it unchanged, but it
   keeps only the two features that make sense on a real-mode DOS box: command
   execution and file transfer, plus the HELLO/PING/INFO handshake.

   DOS is single-tasking, so there is exactly one connection, no threads, and a
   plain blocking accept-then-serve-to-completion loop. There is no Winsock
   (Watt-32 provides the TCP stack), no CreateProcess/pipes (commands run
   synchronously with stdout captured to a temp file), and no registry (the agent
   is launched from AUTOEXEC.BAT). See docs\dos-agent.md. */

#include "v9xremote/protocol.h"

/* Capabilities this DOS build advertises in HELLO/INFO. The host gates every op
   on its capability bit, so a client that asks for a dropped feature (cancel,
   detach, screenshot, input, power, update, HTTP download) is refused cleanly.
   Only the info/ping handshake, both exec modes, and file read/write are here. */
#define V9X_DOS_CAPABILITIES (V9X_CAP_INFO | V9X_CAP_PING | \
                              V9X_CAP_EXEC_DIRECT | V9X_CAP_EXEC_SHELL | \
                              V9X_CAP_FILE_READ | V9X_CAP_FILE_WRITE)

/* Pseudo winsock-version reported in HELLO/INFO so a host can tell a DOS agent
   apart from a Win9x one (0x20 = "Watt-32", not a real Winsock number). */
#define V9X_DOS_STACK_MARKER 0x0020u

/* State directory and files. Everything must fit FAT 8.3: "V9XREMOTE" (9 chars)
   is not a valid DOS directory name, so the DOS build uses "V9XREMOT". */
#define V9X_DOS_ROOT      "C:\\V9XREMOT"
#define V9X_DOS_TEMP      "C:\\V9XREMOT\\TEMP"
#define V9X_DOS_CONFIG    "C:\\V9XREMOT\\AGENT.INI"
#define V9X_DOS_BOOT      "C:\\V9XREMOT\\BOOT.DAT"
#define V9X_DOS_LOG       "C:\\V9XREMOT\\AGENT.LOG"
#define V9X_DOS_EXEC_OUT  "C:\\V9XREMOT\\TEMP\\OUT.TMP"

/* 8.3-safe staging names, created adjacent to the upload destination so the
   final rename stays on one volume. Single upload at a time, so fixed names are
   safe (unlike the Win9x build which encodes connection+request into the name). */
#define V9X_DOS_PART_NAME "V9XUP.TMP"
#define V9X_DOS_BAK_NAME  "V9XUP.BAK"

#define V9X_DOS_FILE_PATH_MAX 230ul
#define V9X_DOS_LIST_RESPONSE_MAX 16384ul

/* In-flight upload state (mirrors the Win9x V9xWriteState). handle is a C file
   descriptor; -1 means no upload is active. */
typedef struct V9xDosWriteState {
    int handle;
    unsigned long expected_size;
    unsigned long expected_crc;
    unsigned long received;
    unsigned long running_crc;
    char final_path[260];
    char temp_path[260];
} V9xDosWriteState;

/* The whole agent state and every per-connection scratch buffer in one struct
   (single connection, so machine-global and connection state merge). Field names
   match the Win9x V9xConnection where the ported file/serve logic references
   them, to keep the diff between the two trees small. */
typedef struct V9xDos {
    int socket;                 /* Watt-32 BSD socket, -1 when no client */
    unsigned long boot_counter;
    unsigned long start_ms;     /* v9x_dos_now_ms() at agent start */
    unsigned short listen_port;
    char allowed_client[16];    /* dotted-quad, empty = allow any */
    V9xDosWriteState write_state;
    unsigned char payload[V9X_MAX_PAYLOAD];
    unsigned char header_bytes[V9X_HEADER_SIZE];
    unsigned char response[2048];
    unsigned char file_response[V9X_DOS_LIST_RESPONSE_MAX];
    unsigned char file_buffer[V9X_FILE_CHUNK_SIZE + 4ul];
} V9xDos;

/* net.c - Watt-32 transport */
int v9x_net_init(void);                 /* sock_init(); 1 = ok */
int v9x_net_listen(unsigned short port);/* returns listening socket or -1 */
int v9x_net_accept(int listener, unsigned long *peer_ip); /* client sock or -1 */
int v9x_net_recv_exact(int sock, unsigned char *target, unsigned long length);
int v9x_net_send_exact(int sock, const unsigned char *source,
                       unsigned long length);
void v9x_net_close(int sock);
unsigned long v9x_net_local_ip(void); /* configured IP, host byte order */
unsigned long v9x_net_inet_addr(const char *text); /* network order, or -1 */

/* serve.c - dispatch loop + responders */
int v9x_send_frame(V9xDos *dos, unsigned short type, unsigned long request_id,
                   const unsigned char *payload, unsigned long length);
int v9x_send_error(V9xDos *dos, unsigned long request_id,
                   unsigned long status, const char *detail);
void v9x_serve_client(V9xDos *dos);

/* exec_dos.c - synchronous command execution */
int v9x_dos_execute(V9xDos *dos, unsigned long request_id,
                    const unsigned char *payload, unsigned long length);

/* files_dos.c - file transfer */
int v9x_is_file_message(unsigned short type);
int v9x_handle_file_message(V9xDos *dos, unsigned short type,
                            unsigned long request_id,
                            const unsigned char *payload, unsigned long length);
void v9x_files_disconnect(V9xDos *dos);

/* config.c - configuration, boot counter, clock, logging */
void v9x_dos_load_config(V9xDos *dos);
unsigned long v9x_dos_increment_boot_counter(void);
unsigned long v9x_dos_now_ms(void);
void v9x_log_line(const char *event_name);
void v9x_log_event(const char *event_name, const char *detail);
void v9x_log_set_boot(unsigned long boot_counter);

#endif
