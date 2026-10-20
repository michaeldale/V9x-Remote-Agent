#include "v9xremote/installid.h"

/* 32-bit finaliser from MurmurHash3: every input bit affects every output
   bit. Only 32-bit multiplies, so no 64-bit runtime helpers are needed. */
static unsigned long v9x_id_fmix(unsigned long value)
{
    value &= 0xfffffffful;
    value ^= value >> 16;
    value = (value * 0x85ebca6bul) & 0xfffffffful;
    value ^= value >> 13;
    value = (value * 0xc2b2ae35ul) & 0xfffffffful;
    value ^= value >> 16;
    return value;
}

static void v9x_id_stir(V9xIdPool *pool)
{
    unsigned long index;
    for (index = 0ul; index < 4ul; ++index) {
        pool->words[index] = v9x_id_fmix(pool->words[index] +
                                         pool->words[(index + 3ul) & 3ul] +
                                         0x9e3779b9ul * (index + 1ul));
    }
}

void v9x_id_begin(V9xIdPool *pool)
{
    pool->words[0] = 0x6a09e667ul;
    pool->words[1] = 0xbb67ae85ul;
    pool->words[2] = 0x3c6ef372ul;
    pool->words[3] = 0xa54ff53aul;
    pool->count = 0ul;
}

void v9x_id_mix(V9xIdPool *pool, unsigned long value)
{
    pool->words[pool->count & 3ul] ^= value & 0xfffffffful;
    ++pool->count;
    v9x_id_stir(pool);
}

void v9x_id_mix_text(V9xIdPool *pool, const char *text)
{
    unsigned long packed = 0ul;
    unsigned long shift = 0ul;
    if (text == 0) return;
    while (*text != '\0') {
        packed |= ((unsigned long)(unsigned char)*text) << shift;
        shift += 8ul;
        if (shift == 32ul) {
            v9x_id_mix(pool, packed);
            packed = 0ul;
            shift = 0ul;
        }
        ++text;
    }
    v9x_id_mix(pool, packed ^ shift);
}

void v9x_id_finish(V9xIdPool *pool, char *target)
{
    static const char digits[] = "0123456789abcdef";
    unsigned char raw[16];
    unsigned long index;
    unsigned long out = 0ul;
    v9x_id_stir(pool);
    v9x_id_stir(pool);
    for (index = 0ul; index < 16ul; ++index) {
        raw[index] = (unsigned char)(pool->words[index >> 2] >> ((index & 3ul) * 8ul));
    }
    raw[6] = (unsigned char)((raw[6] & 0x0fu) | 0x40u);
    raw[8] = (unsigned char)((raw[8] & 0x3fu) | 0x80u);
    for (index = 0ul; index < 16ul; ++index) {
        if (index == 4ul || index == 6ul || index == 8ul || index == 10ul) {
            target[out++] = '-';
        }
        target[out++] = digits[raw[index] >> 4];
        target[out++] = digits[raw[index] & 0x0fu];
    }
    target[out] = '\0';
}

int v9x_id_valid(const char *text)
{
    unsigned long index;
    char ch;
    if (text == 0) return 0;
    for (index = 0ul; index < V9X_INSTALL_ID_LENGTH; ++index) {
        ch = text[index];
        if (index == 8ul || index == 13ul || index == 18ul || index == 23ul) {
            if (ch != '-') return 0;
        } else if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                     (ch >= 'A' && ch <= 'F'))) {
            return 0;
        }
    }
    return text[V9X_INSTALL_ID_LENGTH] == '\0';
}

/* 1 when line (not terminated) is "key=..." with optional blanks around key. */
static int v9x_ini_is_key(const char *line, unsigned long length, const char *key)
{
    unsigned long index = 0ul;
    while (index < length && (line[index] == ' ' || line[index] == '\t')) ++index;
    while (*key != '\0') {
        char ch;
        if (index >= length) return 0;
        ch = line[index];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if (ch != *key) return 0;
        ++index;
        ++key;
    }
    while (index < length && (line[index] == ' ' || line[index] == '\t')) ++index;
    return index < length && line[index] == '=';
}

int v9x_ini_repair(const char *text, unsigned long length, char *out,
                   unsigned long capacity, const char *unique_key,
                   unsigned long *out_length)
{
    unsigned long at = 0ul;
    unsigned long used = 0ul;
    int seen = 0;
    int blank = 0;
    while (at < length) {
        unsigned long start = at;
        unsigned long end;
        unsigned long need;
        unsigned long index;
        while (at < length && text[at] != '\r' && text[at] != '\n') ++at;
        end = at;
        if (at < length) {
            if (text[at] == '\r' && at + 1ul < length && text[at + 1ul] == '\n') {
                at += 2ul;
            } else {
                ++at;
            }
        }
        if (end == start) {
            if (used != 0ul) blank = 1;
            continue;
        }
        if (text[start] == '[') {
            seen = 0;
        } else if (unique_key != 0 &&
                   v9x_ini_is_key(text + start, end - start, unique_key)) {
            if (seen) continue;
            seen = 1;
        }
        need = (end - start) + 2ul + (blank ? 2ul : 0ul);
        if (need > capacity - used) return 0;
        if (blank) {
            out[used++] = '\r';
            out[used++] = '\n';
            blank = 0;
        }
        for (index = start; index < end; ++index) out[used++] = text[index];
        out[used++] = '\r';
        out[used++] = '\n';
    }
    *out_length = used;
    return 1;
}
