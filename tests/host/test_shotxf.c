/* Screenshot shaping (src\common\shotxf.c).
 *
 *   test_shotxf.exe            run the checks; exit code names the first failure
 *   test_shotxf.exe dump DIR   write fixture BMPs plus the RGB each must decode
 *                              to (top-down R,G,B bytes) for the PowerShell
 *                              (System.Drawing) and Python readers
 */
#include "v9xremote/shotxf.h"
#include <stdio.h>
#include <string.h>

#define W 37ul
#define H 23ul
#define SRC_BYTES (54ul + ((W * 3ul + 3ul) & ~3ul) * H)
#define OUT_CAP 65536ul
#define WORK_CAP 65536ul

static v9x_u8 source[SRC_BYTES];
static v9x_u8 output[OUT_CAP];
static v9x_u8 work[WORK_CAP];
static v9x_u8 decoded[W * H * 3ul];

static v9x_u32 get32(const v9x_u8 *p)
{
    return (v9x_u32)p[0] | ((v9x_u32)p[1] << 8) | ((v9x_u32)p[2] << 16) |
           ((v9x_u32)p[3] << 24);
}

static void put32(v9x_u8 *p, v9x_u32 v)
{
    p[0] = (v9x_u8)v; p[1] = (v9x_u8)(v >> 8);
    p[2] = (v9x_u8)(v >> 16); p[3] = (v9x_u8)(v >> 24);
}

/* Pattern 0: a dialog (few flat colours, runs). 1: a photo (> 256 colours).
   2: noise from 200 colours (RLE8 loses to raw 8-bit). */
static void pixel(int pattern, v9x_u32 x, v9x_u32 y, v9x_u8 *rgb)
{
    static const v9x_u8 flat[6][3] = {
        {192u, 192u, 192u}, {0u, 0u, 128u}, {255u, 255u, 255u},
        {0u, 128u, 128u}, {128u, 128u, 128u}, {255u, 255u, 0u}
    };
    if (pattern == 0) {
        v9x_u32 index = ((x / 8ul) + (y / 5ul)) % 6ul;
        if ((x * 7ul + y * 3ul) % 11ul == 0ul) {
            rgb[0] = 0u; rgb[1] = 0u; rgb[2] = 0u;
            return;
        }
        rgb[0] = flat[index][0]; rgb[1] = flat[index][1]; rgb[2] = flat[index][2];
    } else if (pattern == 1) {
        rgb[0] = (v9x_u8)(x * 6ul);
        rgb[1] = (v9x_u8)(y * 11ul);
        rgb[2] = (v9x_u8)((x * y) & 0xFFul);
    } else {
        v9x_u32 index = (x * 31ul + y * 17ul + x * y) % 200ul;
        rgb[0] = (v9x_u8)index;
        rgb[1] = (v9x_u8)(255ul - index);
        rgb[2] = (v9x_u8)(index * 3ul);
    }
}

static void make_source(int pattern)
{
    v9x_u32 stride = (W * 3ul + 3ul) & ~3ul;
    v9x_u32 x;
    v9x_u32 y;
    memset(source, 0, sizeof(source));
    source[0] = 'B'; source[1] = 'M';
    put32(source + 2, SRC_BYTES);
    put32(source + 10, 54ul);
    put32(source + 14, 40ul);
    put32(source + 18, W);
    put32(source + 22, H);
    source[26] = 1u;
    source[28] = 24u;
    put32(source + 34, stride * H);
    for (y = 0ul; y < H; ++y) {
        v9x_u8 *row = source + 54ul + (H - 1ul - y) * stride;
        for (x = 0ul; x < W; ++x) {
            v9x_u8 rgb[3];
            pixel(pattern, x, y, rgb);
            row[x * 3ul] = rgb[2];
            row[x * 3ul + 1ul] = rgb[1];
            row[x * 3ul + 2ul] = rgb[0];
        }
    }
}

/* An independent BMP reader (24-bit, 8-bit raw, RLE8) into top-down RGB. */
static int decode(const v9x_u8 *bmp, v9x_u32 length, v9x_u32 *width, v9x_u32 *height)
{
    v9x_u32 offset = get32(bmp + 10);
    v9x_u32 w = get32(bmp + 18);
    v9x_u32 h = get32(bmp + 22);
    v9x_u32 bits = (v9x_u32)bmp[28];
    v9x_u32 compression = get32(bmp + 30);
    v9x_u32 colours = get32(bmp + 46);
    const v9x_u8 *palette = bmp + 54;
    v9x_u32 x = 0ul;
    v9x_u32 y = 0ul;  /* bottom-up row */
    v9x_u32 at;
    if (bmp[0] != 'B' || bmp[1] != 'M' || get32(bmp + 2) != length) return 0;
    if (w * h * 3ul > sizeof(decoded)) return 0;
    *width = w;
    *height = h;
    if (bits == 24ul && compression == 0ul) {
        v9x_u32 stride = (w * 3ul + 3ul) & ~3ul;
        for (y = 0ul; y < h; ++y) {
            for (x = 0ul; x < w; ++x) {
                const v9x_u8 *p = bmp + offset + y * stride + x * 3ul;
                v9x_u8 *d = decoded + ((h - 1ul - y) * w + x) * 3ul;
                d[0] = p[2]; d[1] = p[1]; d[2] = p[0];
            }
        }
        return 1;
    }
    if (bits != 8ul || colours == 0ul || colours > 256ul ||
        offset != 54ul + colours * 4ul) return 0;
    if (compression == 0ul) {
        v9x_u32 stride = (w + 3ul) & ~3ul;
        for (y = 0ul; y < h; ++y) {
            for (x = 0ul; x < w; ++x) {
                v9x_u32 index = bmp[offset + y * stride + x];
                v9x_u8 *d = decoded + ((h - 1ul - y) * w + x) * 3ul;
                if (index >= colours) return 0;
                d[0] = palette[index * 4ul + 2ul];
                d[1] = palette[index * 4ul + 1ul];
                d[2] = palette[index * 4ul];
            }
        }
        return 1;
    }
    if (compression != 1ul) return 0;
    memset(decoded, 0xEE, w * h * 3ul);
    at = offset;
    for (;;) {
        v9x_u32 count;
        v9x_u32 value;
        if (at + 2ul > length) return 0;
        count = bmp[at++];
        value = bmp[at++];
        if (count == 0ul && value == 0ul) { x = 0ul; ++y; continue; }
        if (count == 0ul && value == 1ul) break;
        if (count == 0ul && value == 2ul) return 0;  /* delta: never emitted */
        if (count == 0ul) {
            v9x_u32 n = value;
            if (at + n > length || x + n > w || y >= h) return 0;
            while (n-- != 0ul) {
                v9x_u32 index = bmp[at++];
                v9x_u8 *d = decoded + ((h - 1ul - y) * w + x) * 3ul;
                if (index >= colours) return 0;
                d[0] = palette[index * 4ul + 2ul];
                d[1] = palette[index * 4ul + 1ul];
                d[2] = palette[index * 4ul];
                ++x;
            }
            if ((value & 1ul) != 0ul) ++at;
            continue;
        }
        if (x + count > w || y >= h || value >= colours) return 0;
        while (count-- != 0ul) {
            v9x_u8 *d = decoded + ((h - 1ul - y) * w + x) * 3ul;
            d[0] = palette[value * 4ul + 2ul];
            d[1] = palette[value * 4ul + 1ul];
            d[2] = palette[value * 4ul];
            ++x;
        }
    }
    if (at != length || y != h - 1ul) return 0;
    return 1;
}

static v9x_u16 run(int pattern, v9x_u32 flags, v9x_u32 rx, v9x_u32 ry,
                   v9x_u32 rw, v9x_u32 rh, v9x_u32 scale, v9x_u32 *length,
                   v9x_u32 *ow, v9x_u32 *oh, v9x_u32 *bits)
{
    V9xShotOptions options;
    make_source(pattern);
    options.flags = flags;
    options.x = rx; options.y = ry; options.width = rw; options.height = rh;
    options.scale_percent = scale;
    return v9x_shotxf_apply(source, SRC_BYTES, &options, output, OUT_CAP,
                            work, WORK_CAP, length, ow, oh, bits);
}

/* Decoded output equals the source pixels in [rx, rx+w) x [ry, ry+h). */
static int matches_source(int pattern, v9x_u32 rx, v9x_u32 ry, v9x_u32 w, v9x_u32 h)
{
    v9x_u32 x;
    v9x_u32 y;
    for (y = 0ul; y < h; ++y) {
        for (x = 0ul; x < w; ++x) {
            v9x_u8 rgb[3];
            pixel(pattern, rx + x, ry + y, rgb);
            if (memcmp(rgb, decoded + (y * w + x) * 3ul, 3u) != 0) return 0;
        }
    }
    return 1;
}

static int dump_one(const char *dir, const char *name, int pattern, v9x_u32 flags,
                    v9x_u32 rx, v9x_u32 ry, v9x_u32 rw, v9x_u32 rh, v9x_u32 scale)
{
    char path[512];
    FILE *file;
    v9x_u32 length, ow, oh, bits;
    if (run(pattern, flags, rx, ry, rw, rh, scale, &length, &ow, &oh, &bits) !=
        V9X_SHOTXF_OK) return 0;
    if (!decode(output, length, &ow, &oh)) return 0;
    sprintf(path, "%s\\%s.bmp", dir, name);
    file = fopen(path, "wb");
    if (file == 0) return 0;
    fwrite(output, 1, length, file);
    fclose(file);
    sprintf(path, "%s\\%s.rgb", dir, name);
    file = fopen(path, "wb");
    if (file == 0) return 0;
    fwrite(decoded, 1, ow * oh * 3ul, file);
    fclose(file);
    printf("%s %lux%lu %lu-bit compression=%lu bytes=%lu\n", name,
           ow, oh, bits, get32(output + 30), length);
    return 1;
}

static int dump(const char *dir)
{
    if (!dump_one(dir, "full24", 1, 0ul, 0ul, 0ul, 0ul, 0ul, 100ul)) return 1;
    if (!dump_one(dir, "compact_rle8", 0, V9X_SHOT_COMPACT, 0ul, 0ul, 0ul, 0ul, 0ul)) return 2;
    if (!dump_one(dir, "compact_raw8", 2, V9X_SHOT_COMPACT, 0ul, 0ul, 0ul, 0ul, 0ul)) return 3;
    if (!dump_one(dir, "compact_24", 1, V9X_SHOT_COMPACT, 0ul, 0ul, 0ul, 0ul, 0ul)) return 4;
    if (!dump_one(dir, "gray", 1, V9X_SHOT_GRAY, 0ul, 0ul, 0ul, 0ul, 0ul)) return 5;
    if (!dump_one(dir, "crop_scale", 1, 0ul, 3ul, 2ul, 20ul, 16ul, 50ul)) return 6;
    return 0;
}

int main(int argc, char **argv)
{
    V9xShotOptions options;
    v9x_u8 wire[V9X_SHOT_OPTIONS_SIZE];
    v9x_u32 length, ow, oh, bits;
    v9x_u32 rx, ry, rw, rh;

    if (argc > 2 && strcmp(argv[1], "dump") == 0) return dump(argv[2]);

    /* Options round trip and the "anything asked?" test. */
    options.flags = 3ul; options.x = 1ul; options.y = 2ul;
    options.width = 3ul; options.height = 4ul; options.scale_percent = 50ul;
    v9x_shot_options_encode(&options, wire);
    memset(&options, 0, sizeof(options));
    if (!v9x_shot_options_decode(wire, V9X_SHOT_OPTIONS_SIZE, &options)) return 1;
    if (options.flags != 3ul || options.x != 1ul || options.height != 4ul ||
        options.scale_percent != 50ul) return 2;
    if (v9x_shot_options_decode(wire, V9X_SHOT_OPTIONS_SIZE - 1ul, &options)) return 3;
    memset(&options, 0, sizeof(options));
    if (v9x_shot_options_active(&options)) return 4;
    options.scale_percent = 100ul;
    if (v9x_shot_options_active(&options)) return 5;

    /* Plan: clipping, rounding up, rejection. */
    options.flags = 0ul; options.x = 30ul; options.y = 20ul;
    options.width = 100ul; options.height = 100ul; options.scale_percent = 50ul;
    if (v9x_shotxf_plan(&options, W, H, &rx, &ry, &rw, &rh, &ow, &oh) != V9X_SHOTXF_OK ||
        rx != 30ul || ry != 20ul || rw != 7ul || rh != 3ul || ow != 4ul || oh != 2ul) return 6;
    options.x = W;
    if (v9x_shotxf_plan(&options, W, H, &rx, &ry, &rw, &rh, &ow, &oh) != V9X_SHOTXF_BAD_OPTIONS) return 7;
    options.x = 0ul; options.scale_percent = 101ul;
    if (v9x_shotxf_plan(&options, W, H, &rx, &ry, &rw, &rh, &ow, &oh) != V9X_SHOTXF_BAD_OPTIONS) return 8;
    options.scale_percent = 0ul; options.flags = 0x80ul;
    if (v9x_shotxf_plan(&options, W, H, &rx, &ry, &rw, &rh, &ow, &oh) != V9X_SHOTXF_BAD_OPTIONS) return 9;

    /* Identity at 100 percent: 24-bit, pixel exact. */
    if (run(1, 0ul, 0ul, 0ul, 0ul, 0ul, 100ul, &length, &ow, &oh, &bits) != V9X_SHOTXF_OK) return 10;
    if (bits != 24ul || ow != W || oh != H || length != SRC_BYTES) return 11;
    if (!decode(output, length, &ow, &oh) || !matches_source(1, 0ul, 0ul, W, H)) return 12;

    /* Crop only. */
    if (run(1, 0ul, 5ul, 4ul, 10ul, 6ul, 0ul, &length, &ow, &oh, &bits) != V9X_SHOTXF_OK) return 13;
    if (ow != 10ul || oh != 6ul || !decode(output, length, &ow, &oh) ||
        !matches_source(1, 5ul, 4ul, 10ul, 6ul)) return 14;

    /* 50 percent of an even region averages each 2x2 block. */
    if (run(1, 0ul, 0ul, 0ul, 36ul, 22ul, 50ul, &length, &ow, &oh, &bits) != V9X_SHOTXF_OK) return 15;
    if (ow != 18ul || oh != 11ul || !decode(output, length, &ow, &oh)) return 16;
    {
        v9x_u8 a[3], b[3], c[3], d[3];
        v9x_u32 channel;
        pixel(1, 10ul, 6ul, a); pixel(1, 11ul, 6ul, b);
        pixel(1, 10ul, 7ul, c); pixel(1, 11ul, 7ul, d);
        for (channel = 0ul; channel < 3ul; ++channel) {
            v9x_u32 mean = ((v9x_u32)a[channel] + b[channel] + c[channel] + d[channel] + 2ul) / 4ul;
            if (decoded[(3ul * 18ul + 5ul) * 3ul + channel] != mean) return 17;
        }
    }

    /* COMPACT on a dialog: lossless 8-bit, RLE8, much smaller. */
    if (run(0, V9X_SHOT_COMPACT, 0ul, 0ul, 0ul, 0ul, 0ul, &length, &ow, &oh, &bits) != V9X_SHOTXF_OK) return 18;
    if (bits != 8ul || get32(output + 30) != 1ul || get32(output + 46) != 7ul) return 19;
    if (length * 2ul >= SRC_BYTES) return 20;
    if (!decode(output, length, &ow, &oh) || !matches_source(0, 0ul, 0ul, W, H)) return 21;

    /* COMPACT on noise: lossless 8-bit, raw rows (RLE8 would be larger). */
    if (run(2, V9X_SHOT_COMPACT, 0ul, 0ul, 0ul, 0ul, 0ul, &length, &ow, &oh, &bits) != V9X_SHOTXF_OK) return 22;
    if (bits != 8ul || get32(output + 30) != 0ul) return 23;
    if (!decode(output, length, &ow, &oh) || !matches_source(2, 0ul, 0ul, W, H)) return 24;

    /* COMPACT on a photo: more than 256 colours, stays 24-bit and exact. */
    if (run(1, V9X_SHOT_COMPACT, 0ul, 0ul, 0ul, 0ul, 0ul, &length, &ow, &oh, &bits) != V9X_SHOTXF_OK) return 25;
    if (bits != 24ul || !decode(output, length, &ow, &oh) || !matches_source(1, 0ul, 0ul, W, H)) return 26;

    /* GRAY: 8-bit, R=G=B = luma. */
    if (run(1, V9X_SHOT_GRAY, 0ul, 0ul, 0ul, 0ul, 0ul, &length, &ow, &oh, &bits) != V9X_SHOTXF_OK) return 27;
    if (bits != 8ul || !decode(output, length, &ow, &oh)) return 28;
    {
        v9x_u8 rgb[3];
        v9x_u32 luma;
        pixel(1, 20ul, 9ul, rgb);
        luma = ((v9x_u32)rgb[0] * 77ul + rgb[1] * 150ul + rgb[2] * 29ul + 128ul) >> 8;
        if (decoded[(9ul * W + 20ul) * 3ul] != luma ||
            decoded[(9ul * W + 20ul) * 3ul + 1ul] != luma) return 29;
    }

    /* Buffers that are too small are refused, not overrun. */
    {
        V9xShotOptions full;
        memset(&full, 0, sizeof(full));
        full.flags = V9X_SHOT_COMPACT;
        make_source(0);
        if (v9x_shotxf_apply(source, SRC_BYTES, &full, output, 100ul, work, WORK_CAP,
                             &length, &ow, &oh, &bits) != V9X_SHOTXF_NO_ROOM) return 30;
        if (v9x_shotxf_apply(source, SRC_BYTES - 1ul, &full, output, OUT_CAP, work,
                             WORK_CAP, &length, &ow, &oh, &bits) != V9X_SHOTXF_BAD_SOURCE) return 31;
    }

    printf("shotxf tests passed\n");
    return 0;
}
