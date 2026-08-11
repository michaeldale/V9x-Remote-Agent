#include "v9xremote/protocol.h"
#include "v9xremote/crc32.h"

static int bytes_equal(const unsigned char *left, const unsigned char *right,
                       unsigned long length)
{
    unsigned long index;
    for (index = 0ul; index < length; ++index) {
        if (left[index] != right[index]) return 0;
    }
    return 1;
}

int main(void)
{
    unsigned char encoded[V9X_HEADER_SIZE];
    unsigned char expected_magic[4] = { 'V', '9', 'X', 'R' };
    unsigned char strings[32];
    unsigned long offset = 0ul;
    V9xFrameHeader input;
    V9xFrameHeader output;
    static const unsigned char crc_text[] = "123456789";

    input.version = V9X_PROTOCOL_VERSION;
    input.type = V9X_MSG_INFO_REQUEST;
    input.request_id = 0x12345678ul;
    input.flags = 0x01020304ul;
    input.payload_length = V9X_MAX_PAYLOAD;
    input.reserved = 0ul;
    v9x_encode_header(encoded, &input);
    if (!bytes_equal(encoded, expected_magic, 4ul)) return 1;
    if (!v9x_decode_header(encoded, &output)) return 2;
    if (output.version != input.version || output.type != input.type ||
        output.request_id != input.request_id || output.flags != input.flags ||
        output.payload_length != input.payload_length || output.reserved != 0ul) {
        return 3;
    }

    encoded[0] = 'X';
    if (v9x_decode_header(encoded, &output)) return 4;
    if (v9x_bounded_length("abcdef", 3ul) != 3ul) return 5;
    if (v9x_bounded_length("abc", 10ul) != 3ul) return 6;
    if (!v9x_append_string(strings, sizeof(strings), &offset, "hello")) return 7;
    if (offset != 7ul || v9x_read_u16(strings) != 5u || strings[2] != 'h') return 8;
    if (v9x_append_string(strings, offset, &offset, "overflow")) return 9;
    if (v9x_crc32_end(v9x_crc32_update(v9x_crc32_begin(), crc_text, 9ul)) !=
        0xcbf43926ul) return 10;
    return 0;
}
