#include "v9xremote/mdns.h"

#define V9X_MDNS_NAME_MAX 256
#define V9X_MDNS_TTL_HOST 120ul
#define V9X_MDNS_TTL_OTHER 4500ul
#define V9X_MDNS_TTL_LEGACY 10ul

typedef struct V9xMdnsWriter {
    unsigned char *out;
    unsigned long capacity;
    unsigned long offset;
    int failed;
} V9xMdnsWriter;

static unsigned long v9x_mdns_length(const char *text)
{
    unsigned long length = 0ul;
    if (text == 0) return 0ul;
    while (text[length] != '\0') ++length;
    return length;
}

static char v9x_mdns_lower(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static int v9x_mdns_same(const char *left, const char *right)
{
    while (*left != '\0' && v9x_mdns_lower(*left) == v9x_mdns_lower(*right)) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

/* target = first "." rest, or just rest when first is 0. */
static void v9x_mdns_join(char *target, const char *first, const char *rest)
{
    unsigned long at = 0ul;
    if (first != 0) {
        while (*first != '\0' && at < V9X_MDNS_NAME_MAX - 2ul) target[at++] = *first++;
        target[at++] = '.';
    }
    while (*rest != '\0' && at < V9X_MDNS_NAME_MAX - 1ul) target[at++] = *rest++;
    target[at] = '\0';
}

/* Decodes the name at *offset into dotted text, following compression
   pointers. *offset moves past the name as it appears in place. */
static int v9x_mdns_read_name(const unsigned char *packet, unsigned long length,
                              unsigned long *offset, char *name)
{
    unsigned long at = *offset;
    unsigned long out = 0ul;
    unsigned long jumps = 0ul;
    int jumped = 0;
    for (;;) {
        unsigned long label;
        if (at >= length) return 0;
        label = packet[at];
        if (label == 0ul) {
            if (!jumped) *offset = at + 1ul;
            break;
        }
        if ((label & 0xc0ul) == 0xc0ul) {
            if (at + 1ul >= length || ++jumps > 16ul) return 0;
            if (!jumped) *offset = at + 2ul;
            jumped = 1;
            at = ((label & 0x3ful) << 8) | packet[at + 1ul];
            continue;
        }
        if ((label & 0xc0ul) != 0ul || at + 1ul + label > length) return 0;
        if (out + label + 1ul >= V9X_MDNS_NAME_MAX) return 0;
        if (out != 0ul) name[out++] = '.';
        ++at;
        while (label-- != 0ul) name[out++] = (char)packet[at++];
    }
    name[out] = '\0';
    return 1;
}

int v9x_mdns_parse_query(const unsigned char *packet, unsigned long length,
                         const V9xMdnsService *service, V9xMdnsQuery *query)
{
    char name[V9X_MDNS_NAME_MAX];
    char instance[V9X_MDNS_NAME_MAX];
    char host[V9X_MDNS_NAME_MAX];
    unsigned long offset = 12ul;
    unsigned long questions;
    unsigned long index;
    query->id = 0u;
    query->wants = 0u;
    query->unicast = 0;
    if (length < 12ul) return 0;
    if ((packet[2] & 0x80u) != 0u) return 0;          /* a response */
    if ((packet[2] & 0x78u) != 0u) return 0;          /* opcode must be QUERY */
    query->id = (unsigned short)(((unsigned int)packet[0] << 8) | packet[1]);
    questions = ((unsigned long)packet[4] << 8) | packet[5];
    v9x_mdns_join(instance, service->instance, V9X_MDNS_SERVICE);
    v9x_mdns_join(host, service->host, "local");
    for (index = 0ul; index < questions; ++index) {
        unsigned int type;
        unsigned int klass;
        if (!v9x_mdns_read_name(packet, length, &offset, name)) return 0;
        if (offset + 4ul > length) return 0;
        type = ((unsigned int)packet[offset] << 8) | packet[offset + 1ul];
        klass = ((unsigned int)packet[offset + 2ul] << 8) | packet[offset + 3ul];
        offset += 4ul;
        if ((klass & 0x7fffu) != 1u && (klass & 0x7fffu) != 255u) continue;
        if ((klass & 0x8000u) != 0u) query->unicast = 1;
        if (v9x_mdns_same(name, V9X_MDNS_SERVICE)) {
            if (type == V9X_MDNS_TYPE_PTR || type == V9X_MDNS_TYPE_ANY) {
                query->wants |= V9X_MDNS_WANT_SERVICE;
            }
        } else if (v9x_mdns_same(name, V9X_MDNS_ENUM)) {
            if (type == V9X_MDNS_TYPE_PTR || type == V9X_MDNS_TYPE_ANY) {
                query->wants |= V9X_MDNS_WANT_ENUM;
            }
        } else if (v9x_mdns_same(name, instance)) {
            if (type == V9X_MDNS_TYPE_SRV || type == V9X_MDNS_TYPE_ANY) {
                query->wants |= V9X_MDNS_WANT_SRV;
            }
            if (type == V9X_MDNS_TYPE_TXT || type == V9X_MDNS_TYPE_ANY) {
                query->wants |= V9X_MDNS_WANT_TXT;
            }
        } else if (v9x_mdns_same(name, host)) {
            if (type == V9X_MDNS_TYPE_A || type == V9X_MDNS_TYPE_ANY) {
                query->wants |= V9X_MDNS_WANT_A;
            }
        }
    }
    return 1;
}

static void v9x_mdns_put8(V9xMdnsWriter *writer, unsigned long value)
{
    if (writer->offset >= writer->capacity) {
        writer->failed = 1;
        return;
    }
    writer->out[writer->offset++] = (unsigned char)(value & 0xfful);
}

static void v9x_mdns_put16(V9xMdnsWriter *writer, unsigned long value)
{
    v9x_mdns_put8(writer, value >> 8);
    v9x_mdns_put8(writer, value);
}

static void v9x_mdns_put32(V9xMdnsWriter *writer, unsigned long value)
{
    v9x_mdns_put16(writer, (value >> 16) & 0xfffful);
    v9x_mdns_put16(writer, value & 0xfffful);
}

static void v9x_mdns_put_label(V9xMdnsWriter *writer, const char *text,
                               unsigned long length)
{
    if (length == 0ul || length > 63ul) {
        writer->failed = 1;
        return;
    }
    v9x_mdns_put8(writer, length);
    while (length-- != 0ul) v9x_mdns_put8(writer, (unsigned char)*text++);
}

/* first is one literal label (may be 0); dotted is split on '.'. */
static void v9x_mdns_put_name(V9xMdnsWriter *writer, const char *first,
                              const char *dotted)
{
    const char *start = dotted;
    if (first != 0) v9x_mdns_put_label(writer, first, v9x_mdns_length(first));
    for (;;) {
        if (*dotted == '.' || *dotted == '\0') {
            v9x_mdns_put_label(writer, start, (unsigned long)(dotted - start));
            if (*dotted == '\0') break;
            start = dotted + 1;
        }
        ++dotted;
    }
    v9x_mdns_put8(writer, 0ul);
}

static unsigned long v9x_mdns_ttl(unsigned long ttl, int legacy, int goodbye)
{
    if (goodbye) return 0ul;
    if (legacy && ttl > V9X_MDNS_TTL_LEGACY) return V9X_MDNS_TTL_LEGACY;
    return ttl;
}

static void v9x_mdns_record(V9xMdnsWriter *writer, const V9xMdnsService *service,
                            unsigned int which, int legacy, int goodbye)
{
    unsigned long length_at;
    unsigned long start;
    unsigned long klass = 1ul;
    unsigned long ttl = V9X_MDNS_TTL_OTHER;
    unsigned int index;
    if (which == V9X_MDNS_WANT_SRV || which == V9X_MDNS_WANT_TXT ||
        which == V9X_MDNS_WANT_A) {
        if (!legacy) klass |= 0x8000ul;       /* unique record: cache flush */
    }
    if (which == V9X_MDNS_WANT_SRV || which == V9X_MDNS_WANT_A) {
        ttl = V9X_MDNS_TTL_HOST;
    }
    switch (which) {
    case V9X_MDNS_WANT_SERVICE:
        v9x_mdns_put_name(writer, 0, V9X_MDNS_SERVICE);
        v9x_mdns_put16(writer, V9X_MDNS_TYPE_PTR);
        break;
    case V9X_MDNS_WANT_ENUM:
        v9x_mdns_put_name(writer, 0, V9X_MDNS_ENUM);
        v9x_mdns_put16(writer, V9X_MDNS_TYPE_PTR);
        break;
    case V9X_MDNS_WANT_SRV:
        v9x_mdns_put_name(writer, service->instance, V9X_MDNS_SERVICE);
        v9x_mdns_put16(writer, V9X_MDNS_TYPE_SRV);
        break;
    case V9X_MDNS_WANT_TXT:
        v9x_mdns_put_name(writer, service->instance, V9X_MDNS_SERVICE);
        v9x_mdns_put16(writer, V9X_MDNS_TYPE_TXT);
        break;
    default:
        v9x_mdns_put_name(writer, service->host, "local");
        v9x_mdns_put16(writer, V9X_MDNS_TYPE_A);
        break;
    }
    v9x_mdns_put16(writer, klass);
    v9x_mdns_put32(writer, v9x_mdns_ttl(ttl, legacy, goodbye));
    length_at = writer->offset;
    v9x_mdns_put16(writer, 0ul);
    start = writer->offset;
    switch (which) {
    case V9X_MDNS_WANT_SERVICE:
        v9x_mdns_put_name(writer, service->instance, V9X_MDNS_SERVICE);
        break;
    case V9X_MDNS_WANT_ENUM:
        v9x_mdns_put_name(writer, 0, V9X_MDNS_SERVICE);
        break;
    case V9X_MDNS_WANT_SRV:
        v9x_mdns_put16(writer, 0ul);
        v9x_mdns_put16(writer, 0ul);
        v9x_mdns_put16(writer, service->port);
        v9x_mdns_put_name(writer, service->host, "local");
        break;
    case V9X_MDNS_WANT_TXT:
        if (service->txt_count == 0u) v9x_mdns_put8(writer, 0ul);
        for (index = 0u; index < service->txt_count && index < V9X_MDNS_MAX_TXT;
             ++index) {
            unsigned long length = v9x_mdns_length(service->txt[index]);
            const char *text = service->txt[index];
            if (length > 255ul) length = 255ul;
            v9x_mdns_put8(writer, length);
            while (length-- != 0ul) v9x_mdns_put8(writer, (unsigned char)*text++);
        }
        break;
    default:
        for (index = 0u; index < 4u; ++index) {
            v9x_mdns_put8(writer, service->address[index]);
        }
        break;
    }
    if (!writer->failed) {
        unsigned long rdlength = writer->offset - start;
        writer->out[length_at] = (unsigned char)(rdlength >> 8);
        writer->out[length_at + 1ul] = (unsigned char)(rdlength & 0xfful);
    }
}

static unsigned int v9x_mdns_emit(V9xMdnsWriter *writer,
                                  const V9xMdnsService *service,
                                  unsigned int set, int legacy, int goodbye)
{
    static const unsigned int order[5] = {
        V9X_MDNS_WANT_SERVICE, V9X_MDNS_WANT_ENUM, V9X_MDNS_WANT_SRV,
        V9X_MDNS_WANT_TXT, V9X_MDNS_WANT_A
    };
    unsigned int count = 0u;
    unsigned int index;
    for (index = 0u; index < 5u; ++index) {
        if ((set & order[index]) != 0u) {
            v9x_mdns_record(writer, service, order[index], legacy, goodbye);
            ++count;
        }
    }
    return count;
}

unsigned long v9x_mdns_build_response(const V9xMdnsService *service,
                                      unsigned int wants, unsigned short id,
                                      int legacy, int goodbye,
                                      unsigned char *out,
                                      unsigned long capacity)
{
    V9xMdnsWriter writer;
    unsigned int extra = 0u;
    unsigned int answers;
    unsigned int additionals;
    wants &= V9X_MDNS_WANT_ALL;
    if (wants == 0u) return 0ul;
    if ((wants & (V9X_MDNS_WANT_SERVICE | V9X_MDNS_WANT_SRV | V9X_MDNS_WANT_TXT)) != 0u) {
        extra = (V9X_MDNS_WANT_SRV | V9X_MDNS_WANT_TXT | V9X_MDNS_WANT_A) & ~wants;
    }
    writer.out = out;
    writer.capacity = capacity;
    writer.offset = 0ul;
    writer.failed = 0;
    v9x_mdns_put16(&writer, legacy ? id : 0u);
    v9x_mdns_put16(&writer, 0x8400ul);                 /* response, authoritative */
    v9x_mdns_put16(&writer, 0ul);
    v9x_mdns_put16(&writer, 0ul);                      /* answer count, below */
    v9x_mdns_put16(&writer, 0ul);
    v9x_mdns_put16(&writer, 0ul);                      /* additional count */
    answers = v9x_mdns_emit(&writer, service, wants, legacy, goodbye);
    additionals = v9x_mdns_emit(&writer, service, extra, legacy, goodbye);
    if (writer.failed) return 0ul;
    out[6] = (unsigned char)(answers >> 8);
    out[7] = (unsigned char)(answers & 0xffu);
    out[10] = (unsigned char)(additionals >> 8);
    out[11] = (unsigned char)(additionals & 0xffu);
    return writer.offset;
}
