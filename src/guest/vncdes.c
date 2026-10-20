#include "vncdes.h"

/* FIPS 46-3 tables, 1-based bit numbers as published. */
static const unsigned char vnc_des_ip[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7
};
static const unsigned char vnc_des_fp[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25
};
static const unsigned char vnc_des_e[48] = {
    32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11,
    12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
    22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1
};
static const unsigned char vnc_des_p[32] = {
    16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
    2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25
};
static const unsigned char vnc_des_pc1[56] = {
    57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18,
    10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22,
    14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4
};
static const unsigned char vnc_des_pc2[48] = {
    14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4,
    26, 8, 16, 7, 27, 20, 13, 2, 41, 52, 31, 37, 47, 55, 30, 40,
    51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32
};
static const unsigned char vnc_des_shifts[16] = {
    1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1
};
static const unsigned char vnc_des_s[8][64] = {
    { 14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7,
      0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
      4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0,
      15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13 },
    { 15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10,
      3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
      0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15,
      13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9 },
    { 10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8,
      13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
      13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7,
      1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12 },
    { 7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15,
      13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
      10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4,
      3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14 },
    { 2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9,
      14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
      4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14,
      11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3 },
    { 12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11,
      10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
      9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6,
      4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13 },
    { 4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1,
      13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
      1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2,
      6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12 },
    { 13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7,
      1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
      7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8,
      2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11 }
};

/* Work on one byte per bit: bit 1 of the standard is the MSB of byte 0. */
static void vnc_des_unpack(const unsigned char *bytes, unsigned char *bits, int count)
{
    int index;
    for (index = 0; index < count; ++index) {
        bits[index] = (unsigned char)((bytes[index / 8] >> (7 - index % 8)) & 1u);
    }
}

static void vnc_des_permute(const unsigned char *from, unsigned char *to,
                            const unsigned char *table, int count)
{
    int index;
    for (index = 0; index < count; ++index) to[index] = from[table[index] - 1];
}

void vnc_des_encrypt(const unsigned char key[8], const unsigned char in[8],
                     unsigned char out[8])
{
    unsigned char key_bits[64];
    unsigned char cd[56];
    unsigned char subkey[48];
    unsigned char block[64];
    unsigned char lr[64];
    unsigned char expanded[48];
    unsigned char sbox_out[32];
    unsigned char mixed[32];
    unsigned char preoutput[64];
    unsigned char temp;
    int round;
    int index;
    int shift;

    vnc_des_unpack(key, key_bits, 64);
    vnc_des_permute(key_bits, cd, vnc_des_pc1, 56);
    vnc_des_unpack(in, block, 64);
    vnc_des_permute(block, lr, vnc_des_ip, 64);
    for (round = 0; round < 16; ++round) {
        /* Rotate C (cd[0..27]) and D (cd[28..55]) left, then pick the key. */
        for (shift = 0; shift < vnc_des_shifts[round]; ++shift) {
            temp = cd[0];
            for (index = 0; index < 27; ++index) cd[index] = cd[index + 1];
            cd[27] = temp;
            temp = cd[28];
            for (index = 28; index < 55; ++index) cd[index] = cd[index + 1];
            cd[55] = temp;
        }
        vnc_des_permute(cd, subkey, vnc_des_pc2, 48);
        /* f(R, K): expand, mix in the key, S-boxes, permute. */
        vnc_des_permute(lr + 32, expanded, vnc_des_e, 48);
        for (index = 0; index < 48; ++index) expanded[index] ^= subkey[index];
        for (index = 0; index < 8; ++index) {
            const unsigned char *six = expanded + index * 6;
            int row = (six[0] << 1) | six[5];
            int column = (six[1] << 3) | (six[2] << 2) | (six[3] << 1) | six[4];
            int value = vnc_des_s[index][row * 16 + column];
            sbox_out[index * 4] = (unsigned char)((value >> 3) & 1);
            sbox_out[index * 4 + 1] = (unsigned char)((value >> 2) & 1);
            sbox_out[index * 4 + 2] = (unsigned char)((value >> 1) & 1);
            sbox_out[index * 4 + 3] = (unsigned char)(value & 1);
        }
        vnc_des_permute(sbox_out, mixed, vnc_des_p, 32);
        /* L, R = R, L ^ f(R, K) */
        for (index = 0; index < 32; ++index) {
            temp = lr[32 + index];
            lr[32 + index] = (unsigned char)(lr[index] ^ mixed[index]);
            lr[index] = temp;
        }
    }
    /* The last round's halves go out swapped: R16 L16. */
    for (index = 0; index < 32; ++index) {
        preoutput[index] = lr[32 + index];
        preoutput[32 + index] = lr[index];
    }
    vnc_des_permute(preoutput, block, vnc_des_fp, 64);
    for (index = 0; index < 8; ++index) {
        int bit;
        out[index] = 0u;
        for (bit = 0; bit < 8; ++bit) {
            out[index] = (unsigned char)((out[index] << 1) | block[index * 8 + bit]);
        }
    }
}

void vnc_auth_response(const char *password, const unsigned char challenge[16],
                       unsigned char response[16])
{
    unsigned char key[8];
    int index;
    for (index = 0; index < 8; ++index) {
        unsigned char c = 0u;
        unsigned char reversed = 0u;
        int bit;
        if (password != 0 && *password != '\0') c = (unsigned char)*password++;
        for (bit = 0; bit < 8; ++bit) {
            reversed = (unsigned char)((reversed << 1) | ((c >> bit) & 1u));
        }
        key[index] = reversed;
    }
    vnc_des_encrypt(key, challenge, response);
    vnc_des_encrypt(key, challenge + 8, response + 8);
}
