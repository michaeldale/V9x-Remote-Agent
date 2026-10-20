/*
 * Tests for the relay framing and chunk signatures (src\common\relay.c).
 *
 * Signed at run time with a fixed test seed. The canonical text is pinned
 * byte for byte: tools\v9xsign.c (the only signer; v9x_mcp.py and
 * v9xctl.ps1 call it) and the agent both build it with relay.c.
 */
#include <stdio.h>
#include <string.h>

#include "v9xremote/ed25519.h"
#include "v9xremote/relay.h"

static unsigned int relay_failures = 0u;

#define RYCHECK(expression) do { \
    if (!(expression)) { \
        printf("FAIL %s:%u: %s\n", __FILE__, (unsigned int)__LINE__, #expression); \
        ++relay_failures; \
    } \
} while (0)

static const v9x_u8 relay_seed[32] = {
    0x76, 0x39, 0x78, 0x55, 0x10, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13,
    0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b
};

#define MACHINE "M-4F7K2Q"
#define INSTALL "0f8e3c2a-1b4d-4e6f-8a9b-0c1d2e3f4a5b"
#define NONCE   "00112233445566778899aabbccddeeff"
#define SESSION "ffeeddccbbaa99887766554433221100"

static const char expected_message[] =
    "V9XRELAY1\n"
    "machine=M-4F7K2Q\n"
    "install=0f8e3c2a-1b4d-4e6f-8a9b-0c1d2e3f4a5b\n"
    "nonce=00112233445566778899aabbccddeeff\n"
    "session=ffeeddccbbaa99887766554433221100\n"
    "seq=42\n"
    /* SHA-256 of "abc" (FIPS 180-2 B.1). */
    "sha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n";

static v9x_u8 relay_public[32];
static v9x_u8 batch[2u * (76u + 16u)];

static v9x_u32 put_record(v9x_u8 *out, v9x_u32 seq, const char *data)
{
    char message[V9X_RELAY_MESSAGE_MAX];
    v9x_u32 message_length;
    v9x_u32 length = (v9x_u32)strlen(data);

    v9x_relay_message(MACHINE, INSTALL, NONCE, SESSION, seq,
                      (const v9x_u8 *)data, length, message, &message_length);
    memcpy(out, "V9XD", 4u);
    out[4] = (v9x_u8)seq; out[5] = (v9x_u8)(seq >> 8);
    out[6] = (v9x_u8)(seq >> 16); out[7] = (v9x_u8)(seq >> 24);
    out[8] = (v9x_u8)length; out[9] = (v9x_u8)(length >> 8);
    out[10] = (v9x_u8)(length >> 16); out[11] = (v9x_u8)(length >> 24);
    v9x_ed25519_sign(relay_seed, relay_public, (const v9x_u8 *)message,
                     message_length, out + 12);
    memcpy(out + 76, data, length);
    return 76ul + length;
}

static void test_message(void)
{
    char message[V9X_RELAY_MESSAGE_MAX];
    v9x_u32 length = 0ul;

    RYCHECK(v9x_relay_message(MACHINE, INSTALL, NONCE, SESSION, 42ul,
                              (const v9x_u8 *)"abc", 3ul, message, &length));
    RYCHECK(length == strlen(expected_message));
    RYCHECK(strcmp(message, expected_message) == 0);

    RYCHECK(v9x_relay_message(MACHINE, INSTALL, NONCE, SESSION, 4294967295ul,
                              (const v9x_u8 *)"abc", 3ul, message, &length));
    RYCHECK(strstr(message, "\nseq=4294967295\n") != 0);
    RYCHECK(v9x_relay_message(MACHINE, INSTALL, NONCE, SESSION, 0ul,
                              (const v9x_u8 *)"abc", 3ul, message, &length));
    RYCHECK(strstr(message, "\nseq=0\n") != 0);

    /* Malformed identifiers are refused. */
    RYCHECK(!v9x_relay_message("M-4F7K2I", INSTALL, NONCE, SESSION, 1ul,
                               (const v9x_u8 *)"a", 1ul, message, &length));
    RYCHECK(!v9x_relay_message("M-4F7K2QQ", INSTALL, NONCE, SESSION, 1ul,
                               (const v9x_u8 *)"a", 1ul, message, &length));
    RYCHECK(!v9x_relay_message(MACHINE, "0f8e3c2a1b4d-4e6f-8a9b-0c1d2e3f4a5bb",
                               NONCE, SESSION, 1ul,
                               (const v9x_u8 *)"a", 1ul, message, &length));
    RYCHECK(!v9x_relay_message(MACHINE, INSTALL,
                               "00112233445566778899AABBCCDDEEFF", SESSION, 1ul,
                               (const v9x_u8 *)"a", 1ul, message, &length));
    RYCHECK(!v9x_relay_message(MACHINE, INSTALL, NONCE, "ffee\nseq=1", 1ul,
                               (const v9x_u8 *)"a", 1ul, message, &length));
    RYCHECK(v9x_relay_machine_valid("M-000000"));
    RYCHECK(!v9x_relay_machine_valid("m-000000"));
    RYCHECK(!v9x_relay_machine_valid(""));
}

static void test_records(void)
{
    struct v9x_relay_record record;
    v9x_u32 used;
    v9x_u32 offset = 0ul;
    v9x_u8 wrong[32];

    used = put_record(batch, 1ul, "hello");
    used += put_record(batch + used, 2ul, "world!");

    RYCHECK(v9x_relay_next(batch, used, &offset, &record) == V9X_RELAY_RECORD_OK);
    RYCHECK(record.seq == 1ul && record.length == 5ul);
    RYCHECK(memcmp(record.data, "hello", 5u) == 0);
    RYCHECK(v9x_relay_verify(&record, relay_public, MACHINE, INSTALL, NONCE, SESSION));
    /* Same bytes, another session, nonce or machine: refused. */
    RYCHECK(!v9x_relay_verify(&record, relay_public, MACHINE, INSTALL, NONCE,
                              "ffeeddccbbaa99887766554433221101"));
    RYCHECK(!v9x_relay_verify(&record, relay_public, MACHINE, INSTALL,
                              "00112233445566778899aabbccddeef0", SESSION));
    RYCHECK(!v9x_relay_verify(&record, relay_public, "M-4F7K2R", INSTALL, NONCE, SESSION));
    memcpy(wrong, relay_public, 32u);
    wrong[0] ^= 0x01u;
    RYCHECK(!v9x_relay_verify(&record, wrong, MACHINE, INSTALL, NONCE, SESSION));

    RYCHECK(v9x_relay_next(batch, used, &offset, &record) == V9X_RELAY_RECORD_OK);
    RYCHECK(record.seq == 2ul && record.length == 6ul);
    RYCHECK(v9x_relay_verify(&record, relay_public, MACHINE, INSTALL, NONCE, SESSION));
    RYCHECK(v9x_relay_next(batch, used, &offset, &record) == V9X_RELAY_RECORD_END);

    /* A changed data byte, or a seq rewritten in transit. */
    offset = 0ul;
    batch[76] ^= 0x20u;
    RYCHECK(v9x_relay_next(batch, used, &offset, &record) == V9X_RELAY_RECORD_OK);
    RYCHECK(!v9x_relay_verify(&record, relay_public, MACHINE, INSTALL, NONCE, SESSION));
    batch[76] ^= 0x20u;
    batch[4] = 7u;
    offset = 0ul;
    RYCHECK(v9x_relay_next(batch, used, &offset, &record) == V9X_RELAY_RECORD_OK);
    RYCHECK(!v9x_relay_verify(&record, relay_public, MACHINE, INSTALL, NONCE, SESSION));
    batch[4] = 1u;

    /* Truncated at every point short of a whole record: BAD, never OK. */
    {
        v9x_u32 cut;
        v9x_u32 accepted = 0ul;
        for (cut = 1ul; cut < 81ul; ++cut) {
            offset = 0ul;
            if (v9x_relay_next(batch, cut, &offset, &record) != V9X_RELAY_RECORD_BAD) {
                ++accepted;
            }
        }
        RYCHECK(accepted == 0ul);
    }
    /* Wrong magic, zero length, length over the cap. */
    offset = 0ul;
    batch[3] = 'U';
    RYCHECK(v9x_relay_next(batch, used, &offset, &record) == V9X_RELAY_RECORD_BAD);
    batch[3] = 'D';
    batch[8] = 0u;
    RYCHECK(v9x_relay_next(batch, used, &offset, &record) == V9X_RELAY_RECORD_BAD);
    batch[8] = 0u; batch[10] = 0x20u;
    RYCHECK(v9x_relay_next(batch, used, &offset, &record) == V9X_RELAY_RECORD_BAD);
}

unsigned int v9x_run_relay_tests(void)
{
    v9x_ed25519_public_key(relay_seed, relay_public);
    test_message();
    test_records();
    return relay_failures;
}
