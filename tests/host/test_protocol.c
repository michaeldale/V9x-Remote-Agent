#include "v9xremote/protocol.h"
#include "v9xremote/crc32.h"
#include "v9xremote/installid.h"
#include "v9xremote/mdns.h"
#include <stdio.h>
#include <string.h>

static int bytes_equal(const unsigned char *left, const unsigned char *right,
                       unsigned long length)
{
    unsigned long index;
    for (index = 0ul; index < length; ++index) {
        if (left[index] != right[index]) return 0;
    }
    return 1;
}

/* The fixed service the PowerShell and Python codec tests also decode (via
   the hex printed by `test_protocol.exe dump-mdns`). */
static void fixture_service(V9xMdnsService *service)
{
    service->instance = "WIN98-1a2b3c4d";
    service->host = "v9x-1a2b3c4d";
    service->port = 9869u;
    service->address[0] = 192u;
    service->address[1] = 168u;
    service->address[2] = 1u;
    service->address[3] = 50u;
    service->txt[0] = "id=1a2b3c4d-0000-4000-8000-000000000001";
    service->txt[1] = "ver=0.8.0";
    service->txt_count = 2u;
}

static int test_install_id(void)
{
    V9xIdPool pool;
    char first[V9X_INSTALL_ID_SIZE];
    char second[V9X_INSTALL_ID_SIZE];
    v9x_id_begin(&pool);
    v9x_id_mix(&pool, 1ul);
    v9x_id_mix_text(&pool, "WIN98");
    v9x_id_finish(&pool, first);
    if (!v9x_id_valid(first)) return 20;
    if (first[14] != '4') return 21;
    if (strchr("89ab", first[19]) == 0) return 22;
    v9x_id_begin(&pool);
    v9x_id_mix(&pool, 1ul);
    v9x_id_mix_text(&pool, "WIN98");
    v9x_id_finish(&pool, second);
    if (strcmp(first, second) != 0) return 23;
    v9x_id_begin(&pool);
    v9x_id_mix(&pool, 2ul);
    v9x_id_mix_text(&pool, "WIN98");
    v9x_id_finish(&pool, second);
    if (strcmp(first, second) == 0) return 24;
    if (!v9x_id_valid("1A2B3C4D-0000-4000-8000-00000000000F")) return 25;
    if (v9x_id_valid("1a2b3c4d-0000-4000-8000-00000000000")) return 26;
    if (v9x_id_valid("1a2b3c4d-0000-4000-8000-0000000000001")) return 27;
    if (v9x_id_valid("1a2b3c4dx0000-4000-8000-000000000001")) return 28;
    if (v9x_id_valid("")) return 29;
    return 0;
}

static int test_mdns(void)
{
    static const unsigned char query[] = {
        0x12, 0x34, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        10, '_', 'v', '9', 'x', 'r', 'e', 'm', 'o', 't', 'e',
        4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0,
        0x00, 0x0c, 0x80, 0x01,
        14, 'W', 'I', 'N', '9', '8', '-', '1', 'a', '2', 'b', '3', 'c', '4', 'd',
        0xc0, 0x0c, 0x00, 0x21, 0x00, 0x01
    };
    static const unsigned char loop[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xc0, 0x0c, 0x00, 0x0c, 0x00, 0x01
    };
    V9xMdnsService service;
    V9xMdnsQuery parsed;
    unsigned char out[V9X_MDNS_PACKET_MAX];
    unsigned long length;
    fixture_service(&service);
    if (!v9x_mdns_parse_query(query, sizeof(query), &service, &parsed)) return 30;
    if (parsed.id != 0x1234u || !parsed.unicast) return 31;
    if (parsed.wants != (V9X_MDNS_WANT_SERVICE | V9X_MDNS_WANT_SRV)) return 32;
    if (v9x_mdns_parse_query(loop, sizeof(loop), &service, &parsed)) return 33;
    if (v9x_mdns_parse_query(query, 20ul, &service, &parsed)) return 34;
    length = v9x_mdns_build_response(&service, V9X_MDNS_WANT_SERVICE, 0x1234u,
                                     1, 0, out, sizeof(out));
    if (length == 0ul) return 35;
    if (out[0] != 0x12u || out[1] != 0x34u || out[2] != 0x84u) return 36;
    if (out[7] != 1u || out[11] != 3u) return 37;
    if (memcmp(out + 12, query + 12, 23u) != 0) return 38;
    /* type PTR, class IN without cache flush, TTL capped at 10 for legacy */
    if (out[35] != 0u || out[36] != 0x0cu || out[37] != 0u || out[38] != 1u ||
        out[42] != 10u) return 39;
    length = v9x_mdns_build_response(&service, V9X_MDNS_WANT_ALL, 0x1234u,
                                     0, 1, out, sizeof(out));
    if (length == 0ul || out[0] != 0u || out[7] != 5u || out[11] != 0u) return 40;
    if (out[39] != 0u || out[40] != 0u || out[41] != 0u || out[42] != 0u) {
        return 41;                                              /* goodbye TTL */
    }
    if (v9x_mdns_build_response(&service, V9X_MDNS_WANT_ALL, 0u, 0, 0, out, 40ul) != 0ul) {
        return 42;
    }
    if (v9x_mdns_build_response(&service, 0u, 0u, 0, 0, out, sizeof(out)) != 0ul) {
        return 43;
    }
    return 0;
}

static int dump_mdns(void)
{
    V9xMdnsService service;
    unsigned char out[V9X_MDNS_PACKET_MAX];
    unsigned long length;
    unsigned long index;
    fixture_service(&service);
    length = v9x_mdns_build_response(&service, V9X_MDNS_WANT_ALL, 0u, 0, 0,
                                     out, sizeof(out));
    for (index = 0ul; index < length; ++index) printf("%02x", out[index]);
    printf("\n");
    return length == 0ul;
}

int main(int argc, char **argv)
{
    int result;
    unsigned char encoded[V9X_HEADER_SIZE];
    unsigned char expected_magic[4] = { 'V', '9', 'X', 'R' };
    unsigned char strings[32];
    unsigned long offset = 0ul;
    V9xFrameHeader input;
    V9xFrameHeader output;
    static const unsigned char crc_text[] = "123456789";

    if (argc > 1 && strcmp(argv[1], "dump-mdns") == 0) return dump_mdns();

    input.version = V9X_PROTOCOL_VERSION;
    input.type = V9X_MSG_INFO_REQUEST;
    input.request_id = 0x12345678ul;
    input.flags = 0x01020304ul;
    input.payload_length = V9X_MAX_PAYLOAD;
    input.reserved = 0ul;
    v9x_encode_header(encoded, &input);
    if (!bytes_equal(encoded, expected_magic, 4ul)) return 1;
    if (!v9x_decode_header(encoded, &output)) return 2;
    if (output.version != input.version || output.type != input.type ||
        output.request_id != input.request_id || output.flags != input.flags ||
        output.payload_length != input.payload_length || output.reserved != 0ul) {
        return 3;
    }

    encoded[0] = 'X';
    if (v9x_decode_header(encoded, &output)) return 4;
    if (v9x_bounded_length("abcdef", 3ul) != 3ul) return 5;
    if (v9x_bounded_length("abc", 10ul) != 3ul) return 6;
    if (!v9x_append_string(strings, sizeof(strings), &offset, "hello")) return 7;
    if (offset != 7ul || v9x_read_u16(strings) != 5u || strings[2] != 'h') return 8;
    if (v9x_append_string(strings, offset, &offset, "overflow")) return 9;
    if (v9x_crc32_end(v9x_crc32_update(v9x_crc32_begin(), crc_text, 9ul)) !=
        0xcbf43926ul) return 10;
    if ((V9X_CAPABILITIES & V9X_CAP_INVENTORY) == 0ul) return 11;
    if ((V9X_MSG_INVENTORY_REQUEST | 0x8000u) != V9X_MSG_INVENTORY_RESPONSE) return 12;
    result = test_install_id();
    if (result != 0) return result;
    return test_mdns();
}
