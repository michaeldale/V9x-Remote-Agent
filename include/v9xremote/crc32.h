#ifndef V9XREMOTE_CRC32_H
#define V9XREMOTE_CRC32_H

unsigned long v9x_crc32_begin(void);
unsigned long v9x_crc32_update(unsigned long crc,
                               const unsigned char *data,
                               unsigned long length);
unsigned long v9x_crc32_end(unsigned long crc);

#endif

