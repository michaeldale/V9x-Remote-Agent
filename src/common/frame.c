#include "v9xremote/protocol.h"

unsigned short v9x_read_u16(const unsigned char *source)
{
    return (unsigned short)((unsigned short)source[0] |
                            ((unsigned short)source[1] << 8));
}

unsigned long v9x_read_u32(const unsigned char *source)
{
    return (unsigned long)source[0] |
           ((unsigned long)source[1] << 8) |
           ((unsigned long)source[2] << 16) |
           ((unsigned long)source[3] << 24);
}

void v9x_write_u16(unsigned char *target, unsigned short value)
{
    target[0] = (unsigned char)(value & 0xffu);
    target[1] = (unsigned char)((value >> 8) & 0xffu);
}

void v9x_write_u32(unsigned char *target, unsigned long value)
{
    target[0] = (unsigned char)(value & 0xfful);
    target[1] = (unsigned char)((value >> 8) & 0xfful);
    target[2] = (unsigned char)((value >> 16) & 0xfful);
    target[3] = (unsigned char)((value >> 24) & 0xfful);
}

int v9x_decode_header(const unsigned char *source, V9xFrameHeader *header)
{
    if (source[0] != V9X_PROTOCOL_MAGIC_0 ||
        source[1] != V9X_PROTOCOL_MAGIC_1 ||
        source[2] != V9X_PROTOCOL_MAGIC_2 ||
        source[3] != V9X_PROTOCOL_MAGIC_3) {
        return 0;
    }
    header->version = v9x_read_u16(source + 4);
    header->type = v9x_read_u16(source + 6);
    header->request_id = v9x_read_u32(source + 8);
    header->flags = v9x_read_u32(source + 12);
    header->payload_length = v9x_read_u32(source + 16);
    header->reserved = v9x_read_u32(source + 20);
    return 1;
}

void v9x_encode_header(unsigned char *target, const V9xFrameHeader *header)
{
    target[0] = V9X_PROTOCOL_MAGIC_0;
    target[1] = V9X_PROTOCOL_MAGIC_1;
    target[2] = V9X_PROTOCOL_MAGIC_2;
    target[3] = V9X_PROTOCOL_MAGIC_3;
    v9x_write_u16(target + 4, header->version);
    v9x_write_u16(target + 6, header->type);
    v9x_write_u32(target + 8, header->request_id);
    v9x_write_u32(target + 12, header->flags);
    v9x_write_u32(target + 16, header->payload_length);
    v9x_write_u32(target + 20, header->reserved);
}

