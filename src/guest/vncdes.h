#ifndef V9X_VNCDES_H
#define V9X_VNCDES_H

/* DES, only as far as VNC authentication needs it: single-block ECB
   encryption. Runtime-free C89; table-driven and bit-by-bit, which is slow
   and fine for two blocks per login. */

void vnc_des_encrypt(const unsigned char key[8], const unsigned char in[8],
                     unsigned char out[8]);

/* RFB VNC authentication: the password (at most 8 characters, zero padded)
   is the DES key with the bits of each byte reversed, and the response is
   the 16-byte challenge encrypted as two ECB blocks. */
void vnc_auth_response(const char *password, const unsigned char challenge[16],
                       unsigned char response[16]);

#endif
