#include "agent.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"
#include "v9xremote/version.h"

static unsigned char v9x_payload[V9X_MAX_PAYLOAD];
static unsigned char v9x_header_bytes[V9X_HEADER_SIZE];
static unsigned char v9x_response[2048];

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

int v9x_send_frame(V9xAgentState *state, unsigned short type,
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
    EnterCriticalSection(&state->send_lock);
    if (state->client_socket == INVALID_SOCKET ||
        !v9x_send_exact(state->client_socket, header_bytes, V9X_HEADER_SIZE)) {
        result = 0;
    } else if (length != 0ul &&
               !v9x_send_exact(state->client_socket, payload, length)) {
        result = 0;
    }
    LeaveCriticalSection(&state->send_lock);
    return result;
}

static int v9x_send_error(V9xAgentState *state, unsigned long request_id,
                          unsigned long status, const char *detail)
{
    unsigned long offset = 8ul;
    v9x_write_u32(v9x_response, status);
    v9x_write_u32(v9x_response + 4, GetLastError());
    if (!v9x_append_string(v9x_response, sizeof(v9x_response), &offset, detail)) {
        return 0;
    }
    return v9x_send_frame(state, V9X_MSG_ERROR_RESPONSE, request_id,
                          v9x_response, offset);
}

static int v9x_send_hello(V9xAgentState *state, unsigned long request_id)
{
    unsigned long offset = 16ul;
    DWORD width;
    DWORD height;
    DWORD bits_per_pixel;
    v9x_write_u16(v9x_response, V9X_PROTOCOL_VERSION);
    v9x_write_u16(v9x_response + 2, 0u);
    v9x_write_u32(v9x_response + 4, V9X_CAPABILITIES);
    v9x_write_u32(v9x_response + 8, V9X_MAX_PAYLOAD);
    v9x_write_u32(v9x_response + 12, state->boot_counter);
    v9x_write_u16(v9x_response + 16, state->listen_port);
    v9x_write_u16(v9x_response + 18, state->winsock_version);
    offset = 20ul;
    if (!v9x_append_string(v9x_response, sizeof(v9x_response), &offset,
                           V9X_BUILD_ID) ||
        !v9x_append_string(v9x_response, sizeof(v9x_response), &offset,
                           state->pending_job)) return 0;
    v9x_screen_info(&width, &height, &bits_per_pixel);
    v9x_response[offset++] = v9x_desktop_ready() ? 1u : 0u;
    v9x_response[offset++] = 0u;
    v9x_write_u16(v9x_response + offset, 0u); offset += 2ul;
    v9x_write_u32(v9x_response + offset, width); offset += 4ul;
    v9x_write_u32(v9x_response + offset, height); offset += 4ul;
    v9x_write_u32(v9x_response + offset, bits_per_pixel); offset += 4ul;
    if (!v9x_append_string(v9x_response, sizeof(v9x_response), &offset,
                           state->listen_address) ||
        !v9x_append_string(v9x_response, sizeof(v9x_response), &offset,
                           state->allowed_client)) return 0;
    return v9x_send_frame(state, V9X_MSG_HELLO_RESPONSE, request_id,
                          v9x_response, offset);
}

static int v9x_send_ping(V9xAgentState *state, unsigned long request_id)
{
    v9x_write_u32(v9x_response, GetTickCount() - state->start_tick);
    v9x_write_u32(v9x_response + 4, state->boot_counter);
    return v9x_send_frame(state, V9X_MSG_PING_RESPONSE, request_id,
                          v9x_response, 8ul);
}

static int v9x_send_info(V9xAgentState *state, unsigned long request_id)
{
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

    v9x_write_u32(v9x_response, state->boot_counter);
    v9x_write_u32(v9x_response + 4, GetTickCount() - state->start_tick);
    v9x_write_u32(v9x_response + 8, V9X_CAPABILITIES);
    v9x_write_u16(v9x_response + 12, state->listen_port);
    v9x_write_u16(v9x_response + 14, state->winsock_version);
    v9x_write_u32(v9x_response + 16, version);
    offset = 20ul;
    if (!v9x_append_string(v9x_response, sizeof(v9x_response), &offset, V9X_AGENT_VERSION) ||
        !v9x_append_string(v9x_response, sizeof(v9x_response), &offset, V9X_BUILD_ID) ||
        !v9x_append_string(v9x_response, sizeof(v9x_response), &offset, computer) ||
        !v9x_append_string(v9x_response, sizeof(v9x_response), &offset, windows_version) ||
        !v9x_append_string(v9x_response, sizeof(v9x_response), &offset, system_directory) ||
        !v9x_append_string(v9x_response, sizeof(v9x_response), &offset, windows_directory) ||
        !v9x_append_string(v9x_response, sizeof(v9x_response), &offset, current_directory) ||
        !v9x_append_string(v9x_response, sizeof(v9x_response), &offset,
                           state->pending_job)) {
        return v9x_send_error(state, request_id, V9X_STATUS_INTERNAL_ERROR,
                              "info response overflow");
    }
    v9x_screen_info(&width, &height, &bits_per_pixel);
    v9x_response[offset++] = v9x_desktop_ready() ? 1u : 0u;
    v9x_response[offset++] = 0u;
    v9x_write_u16(v9x_response + offset, 0u); offset += 2ul;
    v9x_write_u32(v9x_response + offset, width); offset += 4ul;
    v9x_write_u32(v9x_response + offset, height); offset += 4ul;
    v9x_write_u32(v9x_response + offset, bits_per_pixel); offset += 4ul;
    if (!v9x_append_string(v9x_response, sizeof(v9x_response), &offset,
                           state->listen_address) ||
        !v9x_append_string(v9x_response, sizeof(v9x_response), &offset,
                           state->allowed_client)) {
        return v9x_send_error(state, request_id, V9X_STATUS_INTERNAL_ERROR,
                              "network info response overflow");
    }
    return v9x_send_frame(state, V9X_MSG_INFO_RESPONSE, request_id,
                          v9x_response, offset);
}

int v9x_serve_client(SOCKET client, V9xAgentState *state)
{
    V9xFrameHeader header;
    int handshaken = 0;
    unsigned short minimum;
    unsigned short maximum;
    unsigned long exec_status;
    unsigned long target_request;
    for (;;) {
        if (!v9x_recv_exact(client, v9x_header_bytes, V9X_HEADER_SIZE)) return 1;
        if (!v9x_decode_header(v9x_header_bytes, &header)) return 0;
        if (header.payload_length > V9X_MAX_PAYLOAD || header.reserved != 0ul ||
            header.request_id == 0ul) return 0;
        if (header.payload_length != 0ul &&
            !v9x_recv_exact(client, v9x_payload, header.payload_length)) return 0;

        if (!handshaken) {
            if (header.type != V9X_MSG_HELLO_REQUEST || header.payload_length < 6ul) {
                (void)v9x_send_error(state, header.request_id,
                                     V9X_STATUS_INVALID_PAYLOAD,
                                     "HELLO required");
                return 0;
            }
            minimum = v9x_read_u16(v9x_payload);
            maximum = v9x_read_u16(v9x_payload + 2);
            if (minimum > V9X_PROTOCOL_VERSION || maximum < V9X_PROTOCOL_VERSION) {
                (void)v9x_send_error(state, header.request_id,
                                     V9X_STATUS_UNSUPPORTED_VERSION,
                                     "no compatible version");
                return 0;
            }
            if ((unsigned long)v9x_read_u16(v9x_payload + 4) + 6ul !=
                    header.payload_length ||
                !v9x_is_ascii_text(v9x_payload + 6,
                                   header.payload_length - 6ul)) {
                (void)v9x_send_error(state, header.request_id,
                                     V9X_STATUS_INVALID_PAYLOAD,
                                     "invalid client label");
                return 0;
            }
            if (!v9x_send_hello(state, header.request_id)) return 0;
            handshaken = 1;
        } else if (header.version != V9X_PROTOCOL_VERSION) {
            if (!v9x_send_error(state, header.request_id,
                                V9X_STATUS_UNSUPPORTED_VERSION,
                                "unsupported frame version")) return 0;
        } else if (header.type == V9X_MSG_PING_REQUEST &&
                   header.payload_length == 0ul) {
            if (!v9x_send_ping(state, header.request_id)) return 0;
        } else if (header.type == V9X_MSG_EXEC_REQUEST) {
            exec_status = v9x_execution_prepare(state, header.request_id,
                                                v9x_payload,
                                                header.payload_length);
            if (exec_status != V9X_STATUS_OK) {
                if (!v9x_send_error(state, header.request_id, exec_status,
                                    exec_status == V9X_STATUS_BUSY ?
                                        "execution already active" :
                                    exec_status == V9X_STATUS_CREATE_FAILED ?
                                        "execution worker creation failed" :
                                        "invalid execution request")) return 0;
            } else {
                v9x_write_u32(v9x_response, GetTickCount());
                if (!v9x_send_frame(state, V9X_MSG_EXEC_ACCEPTED,
                                    header.request_id, v9x_response, 4ul)) {
                    (void)v9x_execution_cancel(state, header.request_id);
                    (void)v9x_execution_resume(state);
                    return 0;
                }
                if (!v9x_execution_resume(state)) {
                    if (!v9x_send_error(state, header.request_id,
                                        V9X_STATUS_CREATE_FAILED,
                                        "execution thread did not start")) return 0;
                }
            }
        } else if (header.type == V9X_MSG_CANCEL_REQUEST &&
                   header.payload_length == 4ul) {
            target_request = v9x_read_u32(v9x_payload);
            v9x_write_u32(v9x_response, target_request);
            v9x_write_u32(v9x_response + 4,
                          v9x_execution_cancel(state, target_request) ? 1ul : 0ul);
            if (!v9x_send_frame(state, V9X_MSG_CANCEL_RESPONSE,
                                header.request_id, v9x_response, 8ul)) return 0;
        } else if (v9x_is_file_message(header.type)) {
            if (!v9x_handle_file_message(state, header.type,
                                         header.request_id, v9x_payload,
                                         header.payload_length)) return 0;
        } else if (header.type == V9X_MSG_REBOOT_REQUEST ||
                   header.type == V9X_MSG_SHUTDOWN_REQUEST) {
            if (!v9x_handle_power_message(state, header.type,
                                          header.request_id, v9x_payload,
                                          header.payload_length)) return 0;
        } else if (header.type == V9X_MSG_SCREENSHOT_REQUEST) {
            if (!v9x_capture_screenshot(state, header.request_id,
                                        v9x_payload,
                                        header.payload_length)) return 0;
        } else if (header.type == V9X_MSG_INPUT_REQUEST) {
            if (!v9x_handle_input(state, header.request_id, v9x_payload,
                                  header.payload_length)) return 0;
        } else if (header.type == V9X_MSG_INFO_REQUEST &&
                   header.payload_length == 0ul) {
            if (!v9x_send_info(state, header.request_id)) return 0;
        } else {
            if (!v9x_send_error(state, header.request_id,
                                V9X_STATUS_UNSUPPORTED_OPERATION,
                                "unsupported operation")) return 0;
        }
    }
}
