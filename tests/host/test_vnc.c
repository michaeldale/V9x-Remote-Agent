/* Host tests for V9XVNC.EXE's portable pieces: DES (VNC authentication)
   and the ZRLE deflate encoder.

     test_vnc.exe                 DES known-answer tests; exit 0 on success
     test_vnc.exe auth <pw> <hex> print the VNC auth response to a challenge
     test_vnc.exe deflate <file>  write deflate cases for Test-VncDeflate.py:
                                  per case u32 raw length, raw bytes, u32
                                  compressed length, compressed bytes (all
                                  little endian), one zlib stream throughout */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/guest/vncdes.h"
#include "../../src/guest/vncz.h"

static int hex_equal(const unsigned char *bytes, const char *hex)
{
    char text[3];
    int index;
    for (index = 0; hex[index * 2] != '\0'; ++index) {
        text[0] = hex[index * 2];
        text[1] = hex[index * 2 + 1];
        text[2] = '\0';
        if ((unsigned char)strtoul(text, 0, 16) != bytes[index]) return 0;
    }
    return 1;
}

static int des_tests(void)
{
    /* FIPS 81 / Stallings worked example, and the all-zero key. */
    static const unsigned char key1[8] = { 0x13, 0x34, 0x57, 0x79, 0x9b, 0xbc, 0xdf, 0xf1 };
    static const unsigned char in1[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    static const unsigned char key2[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    static const unsigned char in2[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    unsigned char out[8];
    unsigned char challenge[16];
    unsigned char response[16];
    unsigned char again[16];
    int index;
    int failures = 0;
    vnc_des_encrypt(key1, in1, out);
    if (!hex_equal(out, "85e813540f0ab405")) {
        printf("FAIL: DES known answer 1\n");
        ++failures;
    }
    vnc_des_encrypt(key2, in2, out);
    if (!hex_equal(out, "8ca64de9c1b123a7")) {
        printf("FAIL: DES known answer 2\n");
        ++failures;
    }
    /* VNC auth: deterministic, and a different password gives a different
       answer. Printed so the Python client's DES can be cross-checked. */
    for (index = 0; index < 16; ++index) challenge[index] = (unsigned char)(index * 17 + 3);
    vnc_auth_response("secret", challenge, response);
    vnc_auth_response("secret", challenge, again);
    if (memcmp(response, again, 16) != 0) { printf("FAIL: auth not deterministic\n"); ++failures; }
    vnc_auth_response("Secret", challenge, again);
    if (memcmp(response, again, 16) == 0) { printf("FAIL: auth ignores the password\n"); ++failures; }
    printf("vnc-auth secret:");
    for (index = 0; index < 16; ++index) printf("%02x", response[index]);
    printf("\n");
    return failures;
}

static void put32(FILE *file, unsigned long value)
{
    fputc((int)(value & 0xff), file);
    fputc((int)((value >> 8) & 0xff), file);
    fputc((int)((value >> 16) & 0xff), file);
    fputc((int)((value >> 24) & 0xff), file);
}

static int deflate_cases(const char *path)
{
    static unsigned long head[VNC_DEFLATE_HASH_SIZE];
    static const unsigned long sizes[] = { 0ul, 17ul, 1ul, 100000ul, 300000ul, 70000ul, 5000ul };
    VncDeflate stream;
    FILE *file = fopen(path, "wb");
    unsigned long seed = 12345ul;
    int which;
    if (file == 0) return 1;
    vnc_deflate_init(&stream, head);
    for (which = 0; which < (int)(sizeof(sizes) / sizeof(sizes[0])); ++which) {
        unsigned long length = sizes[which];
        unsigned char *raw = (unsigned char *)malloc(length + 1ul);
        unsigned long bound = vnc_deflate_bound(length);
        unsigned char *packed = (unsigned char *)malloc(bound);
        unsigned long written;
        unsigned long index;
        for (index = 0ul; index < length; ++index) {
            seed = seed * 1103515245ul + 12345ul;
            switch (which) {
            case 1: raw[index] = (unsigned char)"hello hello hello"[index]; break;
            case 3: raw[index] = (unsigned char)(seed >> 16); break;           /* noise */
            case 4: raw[index] = (unsigned char)((index % 1000ul) < 600ul ?   /* ZRLE-like */
                                                 (index / 7ul) & 3ul : seed >> 24); break;
            case 5: raw[index] = 0u; break;                                   /* one long run */
            default: raw[index] = (unsigned char)(index * 31ul); break;
            }
        }
        written = vnc_deflate_sync(&stream, raw, length, packed, bound);
        if (written == 0ul) {
            printf("FAIL: deflate case %d overflowed its bound\n", which);
            fclose(file);
            return 1;
        }
        put32(file, length);
        fwrite(raw, 1, length, file);
        put32(file, written);
        fwrite(packed, 1, written, file);
        printf("deflate case %d: %lu -> %lu\n", which, length, written);
        free(raw);
        free(packed);
    }
    fclose(file);
    return 0;
}

/* "auth <password> <32 hex challenge>": print the VNC response, so the
   Python test client answers a challenge with this (tested) DES. */
static int auth_answer(const char *password, const char *hex)
{
    unsigned char challenge[16];
    unsigned char response[16];
    char text[3];
    int index;
    if (strlen(hex) != 32u) return 2;
    for (index = 0; index < 16; ++index) {
        text[0] = hex[index * 2];
        text[1] = hex[index * 2 + 1];
        text[2] = '\0';
        challenge[index] = (unsigned char)strtoul(text, 0, 16);
    }
    vnc_auth_response(password, challenge, response);
    for (index = 0; index < 16; ++index) printf("%02x", response[index]);
    printf("\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "deflate") == 0) return deflate_cases(argv[2]);
    if (argc == 4 && strcmp(argv[1], "auth") == 0) return auth_answer(argv[2], argv[3]);
    if (des_tests() != 0) return 1;
    printf("PASS: DES known answers and VNC auth\n");
    return 0;
}
