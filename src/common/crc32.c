#include "v9xremote/crc32.h"

static unsigned long v9x_crc32_table[256];
static int v9x_crc32_ready;

static void v9x_crc32_initialize(void)
{
    unsigned long index;
    unsigned long value;
    unsigned int bit;
    if (v9x_crc32_ready) return;
    for (index = 0ul; index < 256ul; ++index) {
        value = index;
        for (bit = 0u; bit < 8u; ++bit) {
            value = (value >> 1) ^
                    (0xedb88320ul & (0ul - (value & 1ul)));
        }
        v9x_crc32_table[index] = value;
    }
    v9x_crc32_ready = 1;
}

unsigned long v9x_crc32_begin(void)
{
    v9x_crc32_initialize();
    return 0xfffffffful;
}

unsigned long v9x_crc32_update(unsigned long crc,
                               const unsigned char *data,
                               unsigned long length)
{
    unsigned long index;
    v9x_crc32_initialize();
    for (index = 0ul; index < length; ++index) {
        crc = (crc >> 8) ^
              v9x_crc32_table[(crc ^ (unsigned long)data[index]) & 0xfful];
    }
    return crc;
}

unsigned long v9x_crc32_end(unsigned long crc)
{
    return crc ^ 0xfffffffful;
}
