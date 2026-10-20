#ifndef V9XREMOTE_CRC32_H
#define V9XREMOTE_CRC32_H

/* CRC-32 as zip uses it: reflected 0xEDB88320, preset all ones, result
 * complemented. "123456789" checks as 0xCBF43926. */
#define V9X_CRC32_INITIAL 0xFFFFFFFFul

unsigned long v9x_crc32_begin(void);
unsigned long v9x_crc32_update(unsigned long crc,
                               const unsigned char *data,
                               unsigned long length);
unsigned long v9x_crc32_end(unsigned long crc);

/* The names the code ported from the Velocity9x updater uses. */
#define v9x_crc32_final(crc) v9x_crc32_end(crc)
unsigned long v9x_crc32(const unsigned char *data, unsigned long length);

#endif
