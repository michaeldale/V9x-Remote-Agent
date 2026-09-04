/* Per-connection serve loop and the HELLO/PING/INFO responders for the DOS
   agent. Ported from src\guest\protocol.c, with three simplifications the DOS
   model allows: a single connection (no per-connection send lock), Watt-32
   transport instead of Winsock, and errno instead of GetLastError. The wire
   layout of every response is byte-for-byte identical to the Win9x agent so the
   existing host tooling is unaffected; the GUI-only fields (screen size) are
   zero-filled and the capability mask is the reduced DOS set. */

#include <dos.h>
#include <direct.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "dosagent.h"
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

static int v9x_is_ascii_text(const unsigned char *text, unsigned long length)
{
    unsigned long index;
    for (index = 0ul; index < length; ++index) {
        if (text[index] == 0u || text[index] > 0x7fu) return 0;
    }
    return 1;
}

/* Format a host-byte-order IPv4 address as a dotted quad. */
static void v9x_ip_to_string(unsigned long ip, char *target)
{
    unsigned long offset = 0ul;
    offset = v9x_append_decimal(target, offset, (ip >> 24) & 0xfful);
    target[offset++] = '.';
    offset = v9x_append_decimal(target, offset, (ip >> 16) & 0xfful);
    target[offset++] = '.';
    offset = v9x_append_decimal(target, offset, (ip >> 8) & 0xfful);
    target[offset++] = '.';
    offset = v9x_append_decimal(target, offset, ip & 0xfful);
    target[offset] = '\0';
}

int v9x_send_frame(V9xDos *dos, unsigned short type, unsigned long request_id,
                   const unsigned char *payload, unsigned long length)
{
    V9xFrameHeader header;
    unsigned char header_bytes[V9X_HEADER_SIZE];
    header.version = V9X_PROTOCOL_VERSION;
    header.type = type;
    header.request_id = request_id;
    header.flags = 0ul;
    header.payload_length = length;
    header.reserved = 0ul;
    v9x_encode_header(header_bytes, &header);
    if (dos->socket < 0 ||
        !v9x_net_send_exact(dos->socket, header_bytes, V9X_HEADER_SIZE)) {
        return 0;
    }
    if (length != 0ul && !v9x_net_send_exact(dos->socket, payload, length)) {
        return 0;
    }
    return 1;
}

int v9x_send_error(V9xDos *dos, unsigned long request_id,
                   unsigned long status, const char *detail)
{
    unsigned long offset = 8ul;
    v9x_write_u32(dos->response, status);
    v9x_write_u32(dos->response + 4, (unsigned long)errno);
    if (!v9x_append_string(dos->response, sizeof(dos->response), &offset,
                           detail)) {
        return 0;
    }
    return v9x_send_frame(dos, V9X_MSG_ERROR_RESPONSE, request_id,
                          dos->response, offset);
}

static int v9x_send_hello(V9xDos *dos, unsigned long request_id)
{
    unsigned long offset;
    char local_ip[16];
    v9x_ip_to_string(v9x_net_local_ip(), local_ip);
    v9x_write_u16(dos->response, V9X_PROTOCOL_VERSION);
    v9x_write_u16(dos->response + 2, 0u);
    v9x_write_u32(dos->response + 4, V9X_DOS_CAPABILITIES);
    v9x_write_u32(dos->response + 8, V9X_MAX_PAYLOAD);
    v9x_write_u32(dos->response + 12, dos->boot_counter);
    v9x_write_u16(dos->response + 16, dos->listen_port);
    v9x_write_u16(dos->response + 18, V9X_DOS_STACK_MARKER);
    offset = 20ul;
    if (!v9x_append_string(dos->response, sizeof(dos->response), &offset,
                           V9X_BUILD_ID) ||
        !v9x_append_string(dos->response, sizeof(dos->response), &offset,
                           "")) return 0;
    /* GUI-only fields have no meaning on DOS: report "ready" and zero geometry. */
    dos->response[offset++] = 1u;
    dos->response[offset++] = 0u;
    v9x_write_u16(dos->response + offset, 0u); offset += 2ul;
    v9x_write_u32(dos->response + offset, 0ul); offset += 4ul;
    v9x_write_u32(dos->response + offset, 0ul); offset += 4ul;
    v9x_write_u32(dos->response + offset, 0ul); offset += 4ul;
    if (!v9x_append_string(dos->response, sizeof(dos->response), &offset,
                           local_ip) ||
        !v9x_append_string(dos->response, sizeof(dos->response), &offset,
                           dos->allowed_client)) return 0;
    return v9x_send_frame(dos, V9X_MSG_HELLO_RESPONSE, request_id,
                          dos->response, offset);
}

static int v9x_send_ping(V9xDos *dos, unsigned long request_id)
{
    v9x_write_u32(dos->response, v9x_dos_now_ms() - dos->start_ms);
    v9x_write_u32(dos->response + 4, dos->boot_counter);
    return v9x_send_frame(dos, V9X_MSG_PING_RESPONSE, request_id,
                          dos->response, 8ul);
}

static int v9x_send_info(V9xDos *dos, unsigned long request_id)
{
    char os_version[64];
    char current_directory[260];
    char local_ip[16];
    unsigned long dos_version;
    unsigned long offset;

    current_directory[0] = '\0';
    (void)getcwd(current_directory, sizeof(current_directory));
    v9x_ip_to_string(v9x_net_local_ip(), local_ip);

    /* Encode as GetVersion() does for the low word: major | (minor << 8). */
    dos_version = (unsigned long)_osmajor | ((unsigned long)_osminor << 8);
    os_version[0] = 'D'; os_version[1] = 'O'; os_version[2] = 'S';
    os_version[3] = ' ';
    offset = v9x_append_decimal(os_version, 4ul, (unsigned long)_osmajor);
    os_version[offset++] = '.';
    offset = v9x_append_decimal(os_version, offset, (unsigned long)_osminor);
    os_version[offset] = '\0';

    v9x_write_u32(dos->response, dos->boot_counter);
    v9x_write_u32(dos->response + 4, v9x_dos_now_ms() - dos->start_ms);
    v9x_write_u32(dos->response + 8, V9X_DOS_CAPABILITIES);
    v9x_write_u16(dos->response + 12, dos->listen_port);
    v9x_write_u16(dos->response + 14, V9X_DOS_STACK_MARKER);
    v9x_write_u32(dos->response + 16, dos_version);
    offset = 20ul;
    if (!v9x_append_string(dos->response, sizeof(dos->response), &offset,
                           V9X_AGENT_VERSION) ||
        !v9x_append_string(dos->response, sizeof(dos->response), &offset,
                           V9X_BUILD_ID) ||
        !v9x_append_string(dos->response, sizeof(dos->response), &offset, "") ||
        !v9x_append_string(dos->response, sizeof(dos->response), &offset,
                           os_version) ||
        !v9x_append_string(dos->response, sizeof(dos->response), &offset, "") ||
        !v9x_append_string(dos->response, sizeof(dos->response), &offset, "") ||
        !v9x_append_string(dos->response, sizeof(dos->response), &offset,
                           current_directory) ||
        !v9x_append_string(dos->response, sizeof(dos->response), &offset, "")) {
        return v9x_send_error(dos, request_id, V9X_STATUS_INTERNAL_ERROR,
                              "info response overflow");
    }
    dos->response[offset++] = 1u;
    dos->response[offset++] = 0u;
    v9x_write_u16(dos->response + offset, 0u); offset += 2ul;
    v9x_write_u32(dos->response + offset, 0ul); offset += 4ul;
    v9x_write_u32(dos->response + offset, 0ul); offset += 4ul;
    v9x_write_u32(dos->response + offset, 0ul); offset += 4ul;
    if (!v9x_append_string(dos->response, sizeof(dos->response), &offset,
                           local_ip) ||
        !v9x_append_string(dos->response, sizeof(dos->response), &offset,
                           dos->allowed_client)) {
        return v9x_send_error(dos, request_id, V9X_STATUS_INTERNAL_ERROR,
                              "network info response overflow");
    }
    return v9x_send_frame(dos, V9X_MSG_INFO_RESPONSE, request_id,
                          dos->response, offset);
}

void v9x_serve_client(V9xDos *dos)
{
    V9xFrameHeader header;
    int handshaken = 0;
    unsigned short minimum;
    unsigned short maximum;
    unsigned long target_request;

    for (;;) {
        if (!v9x_net_recv_exact(dos->socket, dos->header_bytes,
                                V9X_HEADER_SIZE)) return;
        if (!v9x_decode_header(dos->header_bytes, &header)) return;
        if (header.payload_length > V9X_MAX_PAYLOAD || header.reserved != 0ul ||
            header.request_id == 0ul) return;
        if (header.payload_length != 0ul &&
            !v9x_net_recv_exact(dos->socket, dos->payload,
                                header.payload_length)) return;

        if (!handshaken) {
            if (header.type != V9X_MSG_HELLO_REQUEST ||
                header.payload_length < 6ul) {
                (void)v9x_send_error(dos, header.request_id,
                                     V9X_STATUS_INVALID_PAYLOAD,
                                     "HELLO required");
                return;
            }
            minimum = v9x_read_u16(dos->payload);
            maximum = v9x_read_u16(dos->payload + 2);
            if (minimum > V9X_PROTOCOL_VERSION ||
                maximum < V9X_PROTOCOL_VERSION) {
                (void)v9x_send_error(dos, header.request_id,
                                     V9X_STATUS_UNSUPPORTED_VERSION,
                                     "no compatible version");
                return;
            }
            if ((unsigned long)v9x_read_u16(dos->payload + 4) + 6ul !=
                    header.payload_length ||
                !v9x_is_ascii_text(dos->payload + 6,
                                   header.payload_length - 6ul)) {
                (void)v9x_send_error(dos, header.request_id,
                                     V9X_STATUS_INVALID_PAYLOAD,
                                     "invalid client label");
                return;
            }
            if (!v9x_send_hello(dos, header.request_id)) return;
            handshaken = 1;
        } else if (header.version != V9X_PROTOCOL_VERSION) {
            if (!v9x_send_error(dos, header.request_id,
                                V9X_STATUS_UNSUPPORTED_VERSION,
                                "unsupported frame version")) return;
        } else if (header.type == V9X_MSG_PING_REQUEST &&
                   header.payload_length == 0ul) {
            if (!v9x_send_ping(dos, header.request_id)) return;
        } else if (header.type == V9X_MSG_EXEC_REQUEST) {
            if (!v9x_dos_execute(dos, header.request_id, dos->payload,
                                 header.payload_length)) return;
        } else if (header.type == V9X_MSG_CANCEL_REQUEST &&
                   header.payload_length == 4ul) {
            /* No concurrent or backgrounded execution exists on DOS, so there is
               never anything to cancel: acknowledge with found = 0. */
            target_request = v9x_read_u32(dos->payload);
            v9x_write_u32(dos->response, target_request);
            v9x_write_u32(dos->response + 4, 0ul);
            if (!v9x_send_frame(dos, V9X_MSG_CANCEL_RESPONSE,
                                header.request_id, dos->response, 8ul)) return;
        } else if (v9x_is_file_message(header.type)) {
            if (!v9x_handle_file_message(dos, header.type, header.request_id,
                                         dos->payload,
                                         header.payload_length)) return;
        } else if (header.type == V9X_MSG_INFO_REQUEST &&
                   header.payload_length == 0ul) {
            if (!v9x_send_info(dos, header.request_id)) return;
        } else {
            if (!v9x_send_error(dos, header.request_id,
                                V9X_STATUS_UNSUPPORTED_OPERATION,
                                "unsupported operation")) return;
        }
    }
}
