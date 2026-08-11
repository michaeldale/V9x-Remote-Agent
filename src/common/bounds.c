#include "v9xremote/protocol.h"

unsigned long v9x_bounded_length(const char *text, unsigned long maximum)
{
    unsigned long length = 0ul;
    if (text == 0) {
        return 0ul;
    }
    while (length < maximum && text[length] != '\0') {
        ++length;
    }
    return length;
}

int v9x_append_string(unsigned char *target, unsigned long capacity,
                      unsigned long *offset, const char *text)
{
    unsigned long length;
    unsigned long index;
    if (target == 0 || offset == 0 || *offset > capacity) {
        return 0;
    }
    length = v9x_bounded_length(text, 65535ul);
    if (length > 65535ul || capacity - *offset < length + 2ul) {
        return 0;
    }
    v9x_write_u16(target + *offset, (unsigned short)length);
    *offset += 2ul;
    for (index = 0ul; index < length; ++index) {
        target[*offset + index] = (unsigned char)text[index];
    }
    *offset += length;
    return 1;
}

