#include "agent.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"
#include "v9xremote/version.h"

static unsigned long v9x_append_decimal(char *target, unsigned long offset,
                                        unsigned long value)
{
    char reversed[10];
    unsigned long count = 0ul;
    unsigned long index;
    do {
        reversed[count++] = (char)('0' + (value % 10ul));
        value /= 10ul;
    } while (value != 0ul && count < sizeof(reversed));
    for (index = 0ul; index < count; ++index) {
        target[offset + index] = reversed[count - index - 1ul];
    }
    return offset + count;
}

static unsigned long v9x_append_hex16(char *target, unsigned long offset,
                                      unsigned short value)
{
    static const char digits[] = "0123456789abcdef";
    target[offset++] = digits[(value >> 12) & 0xfu];
    target[offset++] = digits[(value >> 8) & 0xfu];
    target[offset++] = digits[(value >> 4) & 0xfu];
    target[offset++] = digits[value & 0xfu];
    return offset;
}

/* Build a one-line audit detail for the dispatch trail: "t=<hex> rid=<n>
   len=<n>". Bounded well under the log detail cap. */
static void v9x_format_cmd(char *target, const V9xFrameHeader *header)
{
    unsigned long offset = 0ul;
    target[offset++] = 't'; target[offset++] = '=';
    offset = v9x_append_hex16(target, offset, header->type);
    target[offset++] = ' ';
    target[offset++] = 'r'; target[offset++] = 'i'; target[offset++] = 'd';
    target[offset++] = '=';
    offset = v9x_append_decimal(target, offset, header->request_id);
    target[offset++] = ' ';
    target[offset++] = 'l'; target[offset++] = 'e'; target[offset++] = 'n';
    target[offset++] = '=';
    offset = v9x_append_decimal(target, offset, header->payload_length);
    target[offset] = '\0';
}

static int v9x_is_ascii_text(const unsigned char *text, unsigned long length)
{
    unsigned long index;
    for (index = 0ul; index < length; ++index) {
        if (text[index] == 0u || text[index] > 0x7fu) return 0;
    }
    return 1;
}

static int v9x_recv_exact(SOCKET socket_handle, unsigned char *target,
                          unsigned long length)
{
    unsigned long offset = 0ul;
    int received;
    while (offset < length) {
        received = recv(socket_handle, (char *)(target + offset),
                        (int)(length - offset), 0);
        if (received <= 0) return 0;
        offset += (unsigned long)received;
    }
    return 1;
}

static int v9x_send_exact(SOCKET socket_handle, const unsigned char *source,
                          unsigned long length)
{
    unsigned long offset = 0ul;
    int sent;
    while (offset < length) {
        sent = send(socket_handle, (const char *)(source + offset),
                    (int)(length - offset), 0);
        if (sent <= 0) return 0;
        offset += (unsigned long)sent;
    }
    return 1;
}

int v9x_send_frame(V9xConnection *conn, unsigned short type,
                   unsigned long request_id,
                   const unsigned char *payload, unsigned long length)
{
    V9xFrameHeader header;
    unsigned char header_bytes[V9X_HEADER_SIZE];
    int result = 1;
    header.version = V9X_PROTOCOL_VERSION;
    header.type = type;
    header.request_id = request_id;
    header.flags = 0ul;
    header.payload_length = length;
    header.reserved = 0ul;
    v9x_encode_header(header_bytes, &header);
    EnterCriticalSection(&conn->send_lock);
    if (conn->socket == INVALID_SOCKET ||
        !v9x_send_exact(conn->socket, header_bytes, V9X_HEADER_SIZE)) {
        result = 0;
    } else if (length != 0ul &&
               !v9x_send_exact(conn->socket, payload, length)) {
        result = 0;
    }
    LeaveCriticalSection(&conn->send_lock);
    if (result) InterlockedIncrement(&conn->machine->activity);
    return result;
}

static int v9x_send_error(V9xConnection *conn, unsigned long request_id,
                          unsigned long status, const char *detail)
{
    unsigned long offset = 8ul;
    v9x_write_u32(conn->response, status);
    v9x_write_u32(conn->response + 4, GetLastError());
    if (!v9x_append_string(conn->response, sizeof(conn->response), &offset,
                           detail)) {
        return 0;
    }
    return v9x_send_frame(conn, V9X_MSG_ERROR_RESPONSE, request_id,
                          conn->response, offset);
}

static int v9x_send_hello(V9xConnection *conn, unsigned long request_id)
{
    V9xAgentState *machine = conn->machine;
    unsigned long offset = 16ul;
    DWORD width;
    DWORD height;
    DWORD bits_per_pixel;
    v9x_write_u16(conn->response, V9X_PROTOCOL_VERSION);
    v9x_write_u16(conn->response + 2, 0u);
    v9x_write_u32(conn->response + 4, V9X_CAPABILITIES);
    v9x_write_u32(conn->response + 8, V9X_MAX_PAYLOAD);
    v9x_write_u32(conn->response + 12, machine->boot_counter);
    v9x_write_u16(conn->response + 16, machine->listen_port);
    v9x_write_u16(conn->response + 18, machine->winsock_version);
    offset = 20ul;
    if (!v9x_append_string(conn->response, sizeof(conn->response), &offset,
                           V9X_BUILD_ID) ||
        !v9x_append_string(conn->response, sizeof(conn->response), &offset,
                           machine->pending_job)) return 0;
    v9x_screen_info(&width, &height, &bits_per_pixel);
    conn->response[offset++] = v9x_desktop_ready() ? 1u : 0u;
    conn->response[offset++] = 0u;
    v9x_write_u16(conn->response + offset, 0u); offset += 2ul;
    v9x_write_u32(conn->response + offset, width); offset += 4ul;
    v9x_write_u32(conn->response + offset, height); offset += 4ul;
    v9x_write_u32(conn->response + offset, bits_per_pixel); offset += 4ul;
    if (!v9x_append_string(conn->response, sizeof(conn->response), &offset,
                           machine->listen_address) ||
        !v9x_append_string(conn->response, sizeof(conn->response), &offset,
                           machine->allowed_client)) return 0;
    return v9x_send_frame(conn, V9X_MSG_HELLO_RESPONSE, request_id,
                          conn->response, offset);
}

static int v9x_send_ping(V9xConnection *conn, unsigned long request_id)
{
    V9xAgentState *machine = conn->machine;
    v9x_write_u32(conn->response, GetTickCount() - machine->start_tick);
    v9x_write_u32(conn->response + 4, machine->boot_counter);
    return v9x_send_frame(conn, V9X_MSG_PING_RESPONSE, request_id,
                          conn->response, 8ul);
}

static int v9x_send_info(V9xConnection *conn, unsigned long request_id)
{
    V9xAgentState *machine = conn->machine;
    char computer[64];
    char windows_version[64];
    char system_directory[MAX_PATH];
    char windows_directory[MAX_PATH];
    char current_directory[MAX_PATH];
    DWORD computer_length = sizeof(computer);
    DWORD version;
    DWORD width;
    DWORD height;
    DWORD bits_per_pixel;
    unsigned long offset = 20ul;

    computer[0] = '\0';
    system_directory[0] = '\0';
    windows_directory[0] = '\0';
    current_directory[0] = '\0';
    (void)GetComputerNameA(computer, &computer_length);
    (void)GetSystemDirectoryA(system_directory, sizeof(system_directory));
    (void)GetWindowsDirectoryA(windows_directory, sizeof(windows_directory));
    (void)GetCurrentDirectoryA(sizeof(current_directory), current_directory);
    version = GetVersion();
    windows_version[0] = 'W'; windows_version[1] = 'i';
    windows_version[2] = 'n'; windows_version[3] = 'd';
    windows_version[4] = 'o'; windows_version[5] = 'w';
    windows_version[6] = 's'; windows_version[7] = ' ';
    offset = v9x_append_decimal(windows_version, 8ul, version & 0xfful);
    windows_version[offset++] = '.';
    offset = v9x_append_decimal(windows_version, offset,
                                (version >> 8) & 0xfful);
    windows_version[offset] = '\0';

    v9x_write_u32(conn->response, machine->boot_counter);
    v9x_write_u32(conn->response + 4, GetTickCount() - machine->start_tick);
    v9x_write_u32(conn->response + 8, V9X_CAPABILITIES);
    v9x_write_u16(conn->response + 12, machine->listen_port);
    v9x_write_u16(conn->response + 14, machine->winsock_version);
    v9x_write_u32(conn->response + 16, version);
    offset = 20ul;
    if (!v9x_append_string(conn->response, sizeof(conn->response), &offset, V9X_AGENT_VERSION) ||
        !v9x_append_string(conn->response, sizeof(conn->response), &offset, V9X_BUILD_ID) ||
        !v9x_append_string(conn->response, sizeof(conn->response), &offset, computer) ||
        !v9x_append_string(conn->response, sizeof(conn->response), &offset, windows_version) ||
        !v9x_append_string(conn->response, sizeof(conn->response), &offset, system_directory) ||
        !v9x_append_string(conn->response, sizeof(conn->response), &offset, windows_directory) ||
        !v9x_append_string(conn->response, sizeof(conn->response), &offset, current_directory) ||
        !v9x_append_string(conn->response, sizeof(conn->response), &offset,
                           machine->pending_job)) {
        return v9x_send_error(conn, request_id, V9X_STATUS_INTERNAL_ERROR,
                              "info response overflow");
    }
    v9x_screen_info(&width, &height, &bits_per_pixel);
    conn->response[offset++] = v9x_desktop_ready() ? 1u : 0u;
    conn->response[offset++] = 0u;
    v9x_write_u16(conn->response + offset, 0u); offset += 2ul;
    v9x_write_u32(conn->response + offset, width); offset += 4ul;
    v9x_write_u32(conn->response + offset, height); offset += 4ul;
    v9x_write_u32(conn->response + offset, bits_per_pixel); offset += 4ul;
    if (!v9x_append_string(conn->response, sizeof(conn->response), &offset,
                           machine->listen_address) ||
        !v9x_append_string(conn->response, sizeof(conn->response), &offset,
                           machine->allowed_client)) {
        return v9x_send_error(conn, request_id, V9X_STATUS_INTERNAL_ERROR,
                              "network info response overflow");
    }
    return v9x_send_frame(conn, V9X_MSG_INFO_RESPONSE, request_id,
                          conn->response, offset);
}

int v9x_serve_client(V9xConnection *conn)
{
    V9xFrameHeader header;
    int handshaken = 0;
    unsigned short minimum;
    unsigned short maximum;
    unsigned long exec_status;
    unsigned long target_request;
    char cmd_detail[64];
    for (;;) {
        if (!v9x_recv_exact(conn->socket, conn->header_bytes, V9X_HEADER_SIZE)) return 1;
        if (!v9x_decode_header(conn->header_bytes, &header)) return 0;
        if (header.payload_length > V9X_MAX_PAYLOAD || header.reserved != 0ul ||
            header.request_id == 0ul) return 0;
        if (header.payload_length != 0ul &&
            !v9x_recv_exact(conn->socket, conn->payload, header.payload_length)) return 0;
        InterlockedIncrement(&conn->machine->activity);
        v9x_format_cmd(cmd_detail, &header);
        v9x_log_event("cmd", cmd_detail);

        if (!handshaken) {
            if (header.type != V9X_MSG_HELLO_REQUEST || header.payload_length < 6ul) {
                (void)v9x_send_error(conn, header.request_id,
                                     V9X_STATUS_INVALID_PAYLOAD,
                                     "HELLO required");
                return 0;
            }
            minimum = v9x_read_u16(conn->payload);
            maximum = v9x_read_u16(conn->payload + 2);
            if (minimum > V9X_PROTOCOL_VERSION || maximum < V9X_PROTOCOL_VERSION) {
                (void)v9x_send_error(conn, header.request_id,
                                     V9X_STATUS_UNSUPPORTED_VERSION,
                                     "no compatible version");
                return 0;
            }
            if ((unsigned long)v9x_read_u16(conn->payload + 4) + 6ul !=
                    header.payload_length ||
                !v9x_is_ascii_text(conn->payload + 6,
                                   header.payload_length - 6ul)) {
                (void)v9x_send_error(conn, header.request_id,
                                     V9X_STATUS_INVALID_PAYLOAD,
                                     "invalid client label");
                return 0;
            }
            if (!v9x_send_hello(conn, header.request_id)) return 0;
            handshaken = 1;
        } else if (header.version != V9X_PROTOCOL_VERSION) {
            if (!v9x_send_error(conn, header.request_id,
                                V9X_STATUS_UNSUPPORTED_VERSION,
                                "unsupported frame version")) return 0;
        } else if (header.type == V9X_MSG_PING_REQUEST &&
                   header.payload_length == 0ul) {
            if (!v9x_send_ping(conn, header.request_id)) return 0;
        } else if (header.type == V9X_MSG_EXEC_REQUEST) {
            exec_status = v9x_execution_prepare(conn, header.request_id,
                                                conn->payload,
                                                header.payload_length);
            if (exec_status != V9X_STATUS_OK) {
                if (!v9x_send_error(conn, header.request_id, exec_status,
                                    exec_status == V9X_STATUS_BUSY ?
                                        "execution already active" :
                                    exec_status == V9X_STATUS_CREATE_FAILED ?
                                        "execution worker creation failed" :
                                        "invalid execution request")) return 0;
            } else {
                v9x_write_u32(conn->response, GetTickCount());
                if (!v9x_send_frame(conn, V9X_MSG_EXEC_ACCEPTED,
                                    header.request_id, conn->response, 4ul)) {
                    (void)v9x_execution_cancel(conn, header.request_id);
                    (void)v9x_execution_resume(conn);
                    return 0;
                }
                if (!v9x_execution_resume(conn)) {
                    if (!v9x_send_error(conn, header.request_id,
                                        V9X_STATUS_CREATE_FAILED,
                                        "execution thread did not start")) return 0;
                }
            }
        } else if (header.type == V9X_MSG_CANCEL_REQUEST &&
                   header.payload_length == 4ul) {
            target_request = v9x_read_u32(conn->payload);
            v9x_write_u32(conn->response, target_request);
            v9x_write_u32(conn->response + 4,
                          v9x_execution_cancel(conn, target_request) ? 1ul : 0ul);
            if (!v9x_send_frame(conn, V9X_MSG_CANCEL_RESPONSE,
                                header.request_id, conn->response, 8ul)) return 0;
        } else if (v9x_is_file_message(header.type)) {
            if (!v9x_handle_file_message(conn, header.type,
                                         header.request_id, conn->payload,
                                         header.payload_length)) return 0;
        } else if (header.type == V9X_MSG_REBOOT_REQUEST ||
                   header.type == V9X_MSG_SHUTDOWN_REQUEST) {
            if (!v9x_handle_power_message(conn, header.type,
                                          header.request_id, conn->payload,
                                          header.payload_length)) return 0;
        } else if (header.type == V9X_MSG_UPDATE_REQUEST) {
            if (!v9x_handle_update(conn, header.request_id, conn->payload,
                                   header.payload_length)) return 0;
        } else if (header.type == V9X_MSG_SCREENSHOT_REQUEST) {
            if (!v9x_capture_screenshot(conn, header.request_id,
                                        conn->payload,
                                        header.payload_length)) return 0;
        } else if (header.type == V9X_MSG_INPUT_REQUEST) {
            if (!v9x_handle_input(conn, header.request_id, conn->payload,
                                  header.payload_length)) return 0;
        } else if (header.type == V9X_MSG_DOWNLOAD_REQUEST) {
            if (!v9x_handle_download(conn, header.request_id, conn->payload,
                                     header.payload_length)) return 0;
        } else if (header.type == V9X_MSG_INFO_REQUEST &&
                   header.payload_length == 0ul) {
            if (!v9x_send_info(conn, header.request_id)) return 0;
        } else {
            if (!v9x_send_error(conn, header.request_id,
                                V9X_STATUS_UNSUPPORTED_OPERATION,
                                "unsupported operation")) return 0;
        }
    }
}
