/*
 * Remote help relay framing and signatures. See relay.h.
 *
 * No C library: this links into the runtime-free agent.
 */
#include "v9xremote/relay.h"
#include "v9xremote/ed25519.h"
#include "v9xremote/sha256.h"

static const char v9x_relay_crockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static v9x_u32 v9x_relay_length(const char *text)
{
    v9x_u32 length = 0ul;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static v9x_u32 v9x_relay_read32(const v9x_u8 *bytes)
{
    return (v9x_u32)bytes[0] | ((v9x_u32)bytes[1] << 8) |
           ((v9x_u32)bytes[2] << 16) | ((v9x_u32)bytes[3] << 24);
}

v9x_u16 v9x_relay_machine_valid(const char *machine)
{
    v9x_u32 index;
    v9x_u32 table;
    v9x_u16 found;

    if (machine == 0 || machine[0] != 'M' || machine[1] != '-') {
        return V9X_FALSE;
    }
    for (index = 2ul; index < V9X_RELAY_MACHINE_LENGTH; ++index) {
        found = V9X_FALSE;
        for (table = 0ul; v9x_relay_crockford[table] != '\0'; ++table) {
            if (machine[index] == v9x_relay_crockford[table]) {
                found = V9X_TRUE;
                break;
            }
        }
        if (!found) {
            return V9X_FALSE;
        }
    }
    return (v9x_u16)(machine[V9X_RELAY_MACHINE_LENGTH] == '\0');
}

v9x_u16 v9x_relay_hex_id_valid(const char *id)
{
    v9x_u32 index;
    char c;

    if (id == 0) {
        return V9X_FALSE;
    }
    for (index = 0ul; index < V9X_RELAY_HEX_ID_LENGTH; ++index) {
        c = id[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return V9X_FALSE;
        }
    }
    return (v9x_u16)(id[V9X_RELAY_HEX_ID_LENGTH] == '\0');
}

/* 8-4-4-4-12 hex digits of either case, as installid.c writes them. */
static v9x_u16 v9x_relay_install_valid(const char *install)
{
    v9x_u32 index;
    char c;

    if (install == 0) {
        return V9X_FALSE;
    }
    for (index = 0ul; index < V9X_RELAY_INSTALL_LENGTH; ++index) {
        c = install[index];
        if (index == 8ul || index == 13ul || index == 18ul || index == 23ul) {
            if (c != '-') {
                return V9X_FALSE;
            }
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F'))) {
            return V9X_FALSE;
        }
    }
    return (v9x_u16)(install[V9X_RELAY_INSTALL_LENGTH] == '\0');
}

void v9x_relay_hex(const v9x_u8 *bytes, v9x_u32 count, char *out)
{
    static const char digits[] = "0123456789abcdef";
    v9x_u32 index;

    for (index = 0ul; index < count; ++index) {
        out[2ul * index] = digits[bytes[index] >> 4];
        out[2ul * index + 1ul] = digits[bytes[index] & 0x0Fu];
    }
    out[2ul * count] = '\0';
}

static void v9x_relay_append(char *out, v9x_u32 *used, const char *text)
{
    while (*text != '\0') {
        out[(*used)++] = *text++;
    }
}

v9x_u16 v9x_relay_message(const char *machine,
                          const char *install,
                          const char *nonce,
                          const char *session,
                          v9x_u32 seq,
                          const v9x_u8 *data,
                          v9x_u32 length,
                          char *out,
                          v9x_u32 *out_length)
{
    struct v9x_sha256 hash;
    v9x_u8 digest[32];
    char hex[65];
    char number[11];
    v9x_u32 used = 0ul;
    v9x_u32 digits = 0ul;
    v9x_u32 value;

    if (!v9x_relay_machine_valid(machine) ||
        !v9x_relay_install_valid(install) ||
        !v9x_relay_hex_id_valid(nonce) ||
        !v9x_relay_hex_id_valid(session) ||
        out == 0 || out_length == 0 || (data == 0 && length != 0ul)) {
        return V9X_FALSE;
    }
    v9x_sha256_init(&hash);
    v9x_sha256_update(&hash, data, length);
    v9x_sha256_final(&hash, digest);
    v9x_relay_hex(digest, 32ul, hex);

    /* seq in decimal, most significant digit first. */
    value = seq;
    do {
        number[10ul - digits - 1ul] = (char)('0' + (int)(value % 10ul));
        value /= 10ul;
        ++digits;
    } while (value != 0ul);
    number[10] = '\0';

    v9x_relay_append(out, &used, "V9XRELAY1\nmachine=");
    v9x_relay_append(out, &used, machine);
    v9x_relay_append(out, &used, "\ninstall=");
    v9x_relay_append(out, &used, install);
    v9x_relay_append(out, &used, "\nnonce=");
    v9x_relay_append(out, &used, nonce);
    v9x_relay_append(out, &used, "\nsession=");
    v9x_relay_append(out, &used, session);
    v9x_relay_append(out, &used, "\nseq=");
    v9x_relay_append(out, &used, number + 10ul - digits);
    v9x_relay_append(out, &used, "\nsha256=");
    v9x_relay_append(out, &used, hex);
    v9x_relay_append(out, &used, "\n");
    out[used] = '\0';
    *out_length = used;
    return (v9x_u16)(used < V9X_RELAY_MESSAGE_MAX &&
                     used == v9x_relay_length(out));
}

v9x_u16 v9x_relay_next(const v9x_u8 *batch,
                       v9x_u32 batch_length,
                       v9x_u32 *offset,
                       struct v9x_relay_record *record)
{
    v9x_u32 at;
    v9x_u32 length;

    if (batch == 0 || offset == 0 || record == 0 || *offset > batch_length) {
        return V9X_RELAY_RECORD_BAD;
    }
    at = *offset;
    if (at == batch_length) {
        return V9X_RELAY_RECORD_END;
    }
    if (batch_length - at < V9X_RELAY_RECORD_HEADER ||
        batch[at] != 'V' || batch[at + 1ul] != '9' ||
        batch[at + 2ul] != 'X' || batch[at + 3ul] != 'D') {
        return V9X_RELAY_RECORD_BAD;
    }
    length = v9x_relay_read32(batch + at + 8ul);
    if (length == 0ul || length > V9X_RELAY_CHUNK_MAX ||
        length > batch_length - at - V9X_RELAY_RECORD_HEADER) {
        return V9X_RELAY_RECORD_BAD;
    }
    record->seq = v9x_relay_read32(batch + at + 4ul);
    record->length = length;
    record->signature = batch + at + 12ul;
    record->data = batch + at + V9X_RELAY_RECORD_HEADER;
    *offset = at + V9X_RELAY_RECORD_HEADER + length;
    return V9X_RELAY_RECORD_OK;
}

v9x_u16 v9x_relay_verify(const struct v9x_relay_record *record,
                         const v9x_u8 public_key[32],
                         const char *machine,
                         const char *install,
                         const char *nonce,
                         const char *session)
{
    char message[V9X_RELAY_MESSAGE_MAX];
    v9x_u32 length;

    if (record == 0 || public_key == 0 ||
        !v9x_relay_message(machine, install, nonce, session, record->seq,
                           record->data, record->length, message, &length)) {
        return V9X_FALSE;
    }
    return v9x_ed25519_verify(record->signature, public_key,
                              (const v9x_u8 *)message, length);
}
