#include "vncz.h"

/* RFC 1951 length and distance code tables. */
static const unsigned short vnc_length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char vnc_length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short vnc_distance_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289,
    16385, 24577
};
static const unsigned char vnc_distance_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

typedef struct VncBits {
    unsigned char *out;
    unsigned long length;
    unsigned long capacity;
    unsigned long bits;
    int count;
    int failed;
} VncBits;

/* Deflate packs bits LSB first. */
static void vnc_bits_put(VncBits *sink, unsigned long value, int count)
{
    sink->bits |= value << sink->count;
    sink->count += count;
    while (sink->count >= 8) {
        if (sink->length >= sink->capacity) {
            sink->failed = 1;
            sink->count = 0;
            sink->bits = 0ul;
            return;
        }
        sink->out[sink->length++] = (unsigned char)(sink->bits & 0xfful);
        sink->bits >>= 8;
        sink->count -= 8;
    }
}

/* Huffman codes are defined MSB first, so they go out bit-reversed. */
static void vnc_bits_code(VncBits *sink, unsigned int code, int count)
{
    unsigned long reversed = 0ul;
    int index;
    for (index = 0; index < count; ++index) {
        reversed = (reversed << 1) | (unsigned long)(code & 1u);
        code >>= 1;
    }
    vnc_bits_put(sink, reversed, count);
}

/* The fixed literal/length code of RFC 1951 section 3.2.6. */
static void vnc_symbol(VncBits *sink, unsigned int symbol)
{
    if (symbol < 144u) vnc_bits_code(sink, 0x30u + symbol, 8);
    else if (symbol < 256u) vnc_bits_code(sink, 0x190u + symbol - 144u, 9);
    else if (symbol < 280u) vnc_bits_code(sink, symbol - 256u, 7);
    else vnc_bits_code(sink, 0xc0u + symbol - 280u, 8);
}

static void vnc_match(VncBits *sink, unsigned int length, unsigned long distance)
{
    int index = 28;
    while (vnc_length_base[index] > length) --index;
    vnc_symbol(sink, 257u + (unsigned int)index);
    vnc_bits_put(sink, (unsigned long)(length - vnc_length_base[index]),
                 vnc_length_extra[index]);
    index = 29;
    while ((unsigned long)vnc_distance_base[index] > distance) --index;
    vnc_bits_code(sink, (unsigned int)index, 5);
    vnc_bits_put(sink, distance - (unsigned long)vnc_distance_base[index],
                 vnc_distance_extra[index]);
}

static unsigned int vnc_hash3(const unsigned char *at)
{
    return ((((unsigned int)at[0] << 8) | (unsigned int)at[1]) ^
            ((unsigned int)at[2] << 4)) & (VNC_DEFLATE_HASH_SIZE - 1);
}

void vnc_deflate_init(VncDeflate *stream, unsigned long *head)
{
    stream->header_written = 0;
    stream->head = head;
}

unsigned long vnc_deflate_bound(unsigned long length)
{
    return length + length / 8ul + 16ul;
}

unsigned long vnc_deflate_sync(VncDeflate *stream, const unsigned char *in,
                               unsigned long length, unsigned char *out,
                               unsigned long capacity)
{
    VncBits sink;
    unsigned long position = 0ul;
    unsigned long index;
    sink.out = out;
    sink.length = 0ul;
    sink.capacity = capacity;
    sink.bits = 0ul;
    sink.count = 0;
    sink.failed = 0;
    if (!stream->header_written) {
        vnc_bits_put(&sink, 0x78ul, 8);  /* deflate, 32 KiB window */
        vnc_bits_put(&sink, 0x01ul, 8);  /* fastest, no dictionary; 0x7801 % 31 == 0 */
        stream->header_written = 1;
    }
    vnc_bits_put(&sink, 0ul, 1);  /* not the final block */
    vnc_bits_put(&sink, 1ul, 2);  /* fixed Huffman codes */
    for (index = 0ul; index < VNC_DEFLATE_HASH_SIZE; ++index) stream->head[index] = 0ul;
    while (position < length && !sink.failed) {
        unsigned int best = 0u;
        unsigned long best_distance = 0ul;
        if (position + 3ul <= length) {
            unsigned int hash = vnc_hash3(in + position);
            unsigned long candidate = stream->head[hash];
            stream->head[hash] = position + 1ul;
            if (candidate != 0ul && position - (candidate - 1ul) <= 32768ul) {
                unsigned long from = candidate - 1ul;
                unsigned long limit = length - position;
                unsigned int run = 0u;
                if (limit > 258ul) limit = 258ul;
                while ((unsigned long)run < limit && in[from + run] == in[position + run]) ++run;
                if (run >= 3u) {
                    best = run;
                    best_distance = position - from;
                }
            }
        }
        if (best != 0u) {
            unsigned long step;
            vnc_match(&sink, best, best_distance);
            for (step = 1ul; step < (unsigned long)best; ++step) {
                if (position + step + 3ul <= length) {
                    stream->head[vnc_hash3(in + position + step)] = position + step + 1ul;
                }
            }
            position += best;
        } else {
            vnc_symbol(&sink, in[position]);
            ++position;
        }
    }
    vnc_symbol(&sink, 256u);      /* end of block */
    vnc_bits_put(&sink, 0ul, 3);  /* sync flush: empty stored block... */
    if (sink.count > 0) vnc_bits_put(&sink, 0ul, 8 - sink.count);
    vnc_bits_put(&sink, 0x0000ul, 16);  /* ...LEN 0 */
    vnc_bits_put(&sink, 0xfffful, 16);  /* NLEN */
    return sink.failed ? 0ul : sink.length;
}
