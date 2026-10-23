#include "v9xremote/shotxf.h"

/* See include\v9xremote\shotxf.h. */

#define V9X_BMP_HEADER 54ul
#define V9X_PALETTE_BYTES 1024ul
#define V9X_HASH_SLOTS 1024ul
/* work: palette (BGRX x 256), hash keys (u32 x slots, colour + 1, 0 empty),
   hash values (u8 x slots), then one index byte per output pixel. */
#define V9X_WORK_KEYS V9X_PALETTE_BYTES
#define V9X_WORK_VALUES (V9X_WORK_KEYS + V9X_HASH_SLOTS * 4ul)
#define V9X_WORK_INDICES (V9X_WORK_VALUES + V9X_HASH_SLOTS)

static v9x_u32 v9x_get32(const v9x_u8 *data)
{
    return (v9x_u32)data[0] | ((v9x_u32)data[1] << 8) |
           ((v9x_u32)data[2] << 16) | ((v9x_u32)data[3] << 24);
}

static v9x_u32 v9x_get16(const v9x_u8 *data)
{
    return (v9x_u32)data[0] | ((v9x_u32)data[1] << 8);
}

static void v9x_put32(v9x_u8 *data, v9x_u32 value)
{
    data[0] = (v9x_u8)(value & 0xFFul);
    data[1] = (v9x_u8)((value >> 8) & 0xFFul);
    data[2] = (v9x_u8)((value >> 16) & 0xFFul);
    data[3] = (v9x_u8)((value >> 24) & 0xFFul);
}

static void v9x_put16(v9x_u8 *data, v9x_u32 value)
{
    data[0] = (v9x_u8)(value & 0xFFul);
    data[1] = (v9x_u8)((value >> 8) & 0xFFul);
}

int v9x_shot_options_decode(const v9x_u8 *data, v9x_u32 length,
                            V9xShotOptions *options)
{
    if (data == 0 || options == 0 || length != V9X_SHOT_OPTIONS_SIZE) return 0;
    options->flags = v9x_get32(data);
    options->x = v9x_get32(data + 4);
    options->y = v9x_get32(data + 8);
    options->width = v9x_get32(data + 12);
    options->height = v9x_get32(data + 16);
    options->scale_percent = v9x_get32(data + 20);
    return 1;
}

void v9x_shot_options_encode(const V9xShotOptions *options, v9x_u8 *data)
{
    v9x_put32(data, options->flags);
    v9x_put32(data + 4, options->x);
    v9x_put32(data + 8, options->y);
    v9x_put32(data + 12, options->width);
    v9x_put32(data + 16, options->height);
    v9x_put32(data + 20, options->scale_percent);
}

int v9x_shot_options_active(const V9xShotOptions *options)
{
    return options->flags != 0ul ||
           (options->width != 0ul && options->height != 0ul) ||
           (options->scale_percent != 0ul && options->scale_percent != 100ul);
}

v9x_u16 v9x_shotxf_plan(const V9xShotOptions *options,
                        v9x_u32 screen_width, v9x_u32 screen_height,
                        v9x_u32 *region_x, v9x_u32 *region_y,
                        v9x_u32 *region_width, v9x_u32 *region_height,
                        v9x_u32 *output_width, v9x_u32 *output_height)
{
    v9x_u32 scale = options->scale_percent == 0ul ? 100ul : options->scale_percent;
    if ((options->flags & ~V9X_SHOT_KNOWN_FLAGS) != 0ul || scale > 100ul ||
        screen_width == 0ul || screen_height == 0ul ||
        screen_width > 4096ul || screen_height > 4096ul) {
        return V9X_SHOTXF_BAD_OPTIONS;
    }
    if (options->width == 0ul || options->height == 0ul) {
        *region_x = 0ul;
        *region_y = 0ul;
        *region_width = screen_width;
        *region_height = screen_height;
    } else {
        /* Clip to the screen; a region entirely off screen is an error so a
           wrong coordinate does not come back as an empty success. */
        if (options->x >= screen_width || options->y >= screen_height) {
            return V9X_SHOTXF_BAD_OPTIONS;
        }
        *region_x = options->x;
        *region_y = options->y;
        *region_width = screen_width - options->x;
        *region_height = screen_height - options->y;
        if (options->width < *region_width) *region_width = options->width;
        if (options->height < *region_height) *region_height = options->height;
    }
    *output_width = (*region_width * scale + 99ul) / 100ul;
    *output_height = (*region_height * scale + 99ul) / 100ul;
    if (*output_width == 0ul) *output_width = 1ul;
    if (*output_height == 0ul) *output_height = 1ul;
    return V9X_SHOTXF_OK;
}

static v9x_u32 v9x_stride24(v9x_u32 width) { return (width * 3ul + 3ul) & ~3ul; }
static v9x_u32 v9x_stride8(v9x_u32 width) { return (width + 3ul) & ~3ul; }

v9x_u32 v9x_shotxf_output_capacity(v9x_u32 output_width, v9x_u32 output_height)
{
    /* The 24-bit image is built first; the 8-bit forms are never larger
       than it plus a palette. */
    return V9X_BMP_HEADER + V9X_PALETTE_BYTES +
           v9x_stride24(output_width) * output_height;
}

v9x_u32 v9x_shotxf_work_capacity(v9x_u32 output_width, v9x_u32 output_height)
{
    return V9X_WORK_INDICES + output_width * output_height;
}

static void v9x_bmp_header(v9x_u8 *out, v9x_u32 width, v9x_u32 height,
                           v9x_u32 bits, v9x_u32 compression,
                           v9x_u32 colours, v9x_u32 image_bytes)
{
    v9x_u32 offset = V9X_BMP_HEADER + colours * 4ul;
    v9x_u32 index;
    for (index = 0ul; index < V9X_BMP_HEADER; ++index) out[index] = 0u;
    out[0] = 'B';
    out[1] = 'M';
    v9x_put32(out + 2, offset + image_bytes);
    v9x_put32(out + 10, offset);
    v9x_put32(out + 14, 40ul);
    v9x_put32(out + 18, width);
    v9x_put32(out + 22, height);
    v9x_put16(out + 26, 1ul);
    v9x_put16(out + 28, bits);
    v9x_put32(out + 30, compression);
    v9x_put32(out + 34, image_bytes);
    v9x_put32(out + 38, 2835ul);  /* 72 dpi */
    v9x_put32(out + 42, 2835ul);
    v9x_put32(out + 46, colours);
    v9x_put32(out + 50, colours);
}

/* Crop and box-scale the source into a 24-bit bottom-up BMP in `out`. */
static void v9x_shotxf_resample(const v9x_u8 *pixels, v9x_u32 source_height,
                                v9x_u32 source_stride,
                                v9x_u32 rx, v9x_u32 ry, v9x_u32 rw, v9x_u32 rh,
                                v9x_u8 *out, v9x_u32 ow, v9x_u32 oh)
{
    v9x_u32 stride = v9x_stride24(ow);
    v9x_u32 oy;
    v9x_u32 ox;
    v9x_u32 index;
    v9x_bmp_header(out, ow, oh, 24ul, 0ul, 0ul, stride * oh);
    for (oy = 0ul; oy < oh; ++oy) {
        /* Top-down source rows y0..y1-1 of the region. */
        v9x_u32 y0 = ry + (oy * rh) / oh;
        v9x_u32 y1 = ry + ((oy + 1ul) * rh) / oh;
        v9x_u8 *row = out + V9X_BMP_HEADER + (oh - 1ul - oy) * stride;
        if (y1 <= y0) y1 = y0 + 1ul;
        for (ox = 0ul; ox < ow; ++ox) {
            v9x_u32 x0 = rx + (ox * rw) / ow;
            v9x_u32 x1 = rx + ((ox + 1ul) * rw) / ow;
            v9x_u32 sum_b = 0ul;
            v9x_u32 sum_g = 0ul;
            v9x_u32 sum_r = 0ul;
            v9x_u32 count;
            v9x_u32 y;
            v9x_u32 x;
            if (x1 <= x0) x1 = x0 + 1ul;
            count = (x1 - x0) * (y1 - y0);
            for (y = y0; y < y1; ++y) {
                const v9x_u8 *source = pixels +
                    (source_height - 1ul - y) * source_stride + x0 * 3ul;
                for (x = x0; x < x1; ++x) {
                    sum_b += source[0];
                    sum_g += source[1];
                    sum_r += source[2];
                    source += 3;
                }
            }
            row[ox * 3ul] = (v9x_u8)((sum_b + count / 2ul) / count);
            row[ox * 3ul + 1ul] = (v9x_u8)((sum_g + count / 2ul) / count);
            row[ox * 3ul + 2ul] = (v9x_u8)((sum_r + count / 2ul) / count);
        }
        for (index = ow * 3ul; index < stride; ++index) row[index] = 0u;
    }
}

/* Index every pixel of the 24-bit image into work. Returns the palette size
   (1..256), or 0 when COMPACT finds more than 256 colours. */
static v9x_u32 v9x_shotxf_index(const v9x_u8 *image24, v9x_u32 width,
                                v9x_u32 height, int gray, v9x_u8 *work)
{
    v9x_u8 *palette = work;
    v9x_u8 *keys = work + V9X_WORK_KEYS;
    v9x_u8 *values = work + V9X_WORK_VALUES;
    v9x_u8 *indices = work + V9X_WORK_INDICES;
    v9x_u32 stride = v9x_stride24(width);
    v9x_u32 colours = 0ul;
    v9x_u32 x;
    v9x_u32 y;
    if (gray) {
        for (x = 0ul; x < 256ul; ++x) {
            palette[x * 4ul] = (v9x_u8)x;
            palette[x * 4ul + 1ul] = (v9x_u8)x;
            palette[x * 4ul + 2ul] = (v9x_u8)x;
            palette[x * 4ul + 3ul] = 0u;
        }
        colours = 256ul;
    } else {
        for (x = 0ul; x < V9X_HASH_SLOTS * 4ul; ++x) keys[x] = 0u;
    }
    /* indices are bottom-up rows, like the BMP. */
    for (y = 0ul; y < height; ++y) {
        const v9x_u8 *row = image24 + y * stride;
        v9x_u8 *target = indices + y * width;
        for (x = 0ul; x < width; ++x) {
            v9x_u32 b = row[x * 3ul];
            v9x_u32 g = row[x * 3ul + 1ul];
            v9x_u32 r = row[x * 3ul + 2ul];
            v9x_u32 key;
            v9x_u32 slot;
            if (gray) {
                target[x] = (v9x_u8)((r * 77ul + g * 150ul + b * 29ul + 128ul) >> 8);
                continue;
            }
            key = (r << 16) | (g << 8) | b;
            slot = ((key * 2654435761ul) >> 22) & (V9X_HASH_SLOTS - 1ul);
            for (;;) {
                v9x_u32 stored = v9x_get32(keys + slot * 4ul);
                if (stored == key + 1ul) {
                    target[x] = values[slot];
                    break;
                }
                if (stored == 0ul) {
                    if (colours == 256ul) return 0ul;
                    v9x_put32(keys + slot * 4ul, key + 1ul);
                    values[slot] = (v9x_u8)colours;
                    palette[colours * 4ul] = (v9x_u8)b;
                    palette[colours * 4ul + 1ul] = (v9x_u8)g;
                    palette[colours * 4ul + 2ul] = (v9x_u8)r;
                    palette[colours * 4ul + 3ul] = 0u;
                    target[x] = (v9x_u8)colours;
                    ++colours;
                    break;
                }
                slot = (slot + 1ul) & (V9X_HASH_SLOTS - 1ul);
            }
        }
    }
    return colours;
}

/* RLE8-encode the bottom-up index rows into out (at most `capacity` bytes).
   Returns the encoded length, or 0 if it does not fit. */
static v9x_u32 v9x_shotxf_rle8(const v9x_u8 *indices, v9x_u32 width,
                               v9x_u32 height, v9x_u8 *out, v9x_u32 capacity)
{
    v9x_u32 at = 0ul;
    v9x_u32 y;
    for (y = 0ul; y < height; ++y) {
        const v9x_u8 *row = indices + y * width;
        v9x_u32 i = 0ul;
        while (i < width) {
            v9x_u32 run = 1ul;
            v9x_u32 j;
            while (i + run < width && run < 255ul && row[i + run] == row[i]) ++run;
            if (run >= 2ul) {
                if (at + 2ul > capacity) return 0ul;
                out[at++] = (v9x_u8)run;
                out[at++] = row[i];
                i += run;
                continue;
            }
            /* A literal stretch, up to the next run of three. */
            j = i;
            while (j < width && j - i < 255ul) {
                if (j + 2ul < width && row[j] == row[j + 1ul] && row[j] == row[j + 2ul]) break;
                ++j;
            }
            if (j - i < 3ul) {
                /* Absolute mode needs at least three bytes. */
                for (; i < j; ++i) {
                    if (at + 2ul > capacity) return 0ul;
                    out[at++] = 1u;
                    out[at++] = row[i];
                }
                continue;
            }
            if (at + 2ul + (j - i) + 1ul > capacity) return 0ul;
            out[at++] = 0u;
            out[at++] = (v9x_u8)(j - i);
            for (; i < j; ++i) out[at++] = row[i];
            if ((at & 1ul) != 0ul) out[at++] = 0u;
        }
        if (at + 2ul > capacity) return 0ul;
        out[at++] = 0u;
        out[at++] = (v9x_u8)(y + 1ul == height ? 1u : 0u);  /* end of bitmap / line */
    }
    return at;
}

v9x_u16 v9x_shotxf_apply(const v9x_u8 *source, v9x_u32 source_length,
                         const V9xShotOptions *options,
                         v9x_u8 *output, v9x_u32 output_capacity,
                         v9x_u8 *work, v9x_u32 work_capacity,
                         v9x_u32 *output_length,
                         v9x_u32 *output_width, v9x_u32 *output_height,
                         v9x_u32 *output_bits)
{
    v9x_u32 width;
    v9x_u32 height;
    v9x_u32 pixel_offset;
    v9x_u32 source_stride;
    v9x_u32 rx;
    v9x_u32 ry;
    v9x_u32 rw;
    v9x_u32 rh;
    v9x_u32 ow;
    v9x_u32 oh;
    v9x_u32 colours;
    v9x_u32 raw8;
    v9x_u32 header8;
    v9x_u32 encoded;
    v9x_u32 y;
    v9x_u16 status;

    if (source == 0 || options == 0 || output == 0 || work == 0 ||
        source_length < V9X_BMP_HEADER || source[0] != 'B' || source[1] != 'M') {
        return V9X_SHOTXF_BAD_SOURCE;
    }
    pixel_offset = v9x_get32(source + 10);
    width = v9x_get32(source + 18);
    height = v9x_get32(source + 22);
    if (v9x_get16(source + 28) != 24ul || v9x_get32(source + 30) != 0ul ||
        width == 0ul || height == 0ul || width > 4096ul || height > 4096ul ||
        pixel_offset < V9X_BMP_HEADER || pixel_offset > source_length) {
        return V9X_SHOTXF_BAD_SOURCE;
    }
    source_stride = v9x_stride24(width);
    if (source_length - pixel_offset < source_stride * height) {
        return V9X_SHOTXF_BAD_SOURCE;
    }
    status = v9x_shotxf_plan(options, width, height, &rx, &ry, &rw, &rh, &ow, &oh);
    if (status != V9X_SHOTXF_OK) return status;
    if (output_capacity < v9x_shotxf_output_capacity(ow, oh) ||
        work_capacity < v9x_shotxf_work_capacity(ow, oh)) {
        return V9X_SHOTXF_NO_ROOM;
    }

    v9x_shotxf_resample(source + pixel_offset, height, source_stride,
                        rx, ry, rw, rh, output, ow, oh);
    *output_width = ow;
    *output_height = oh;
    *output_bits = 24ul;
    *output_length = V9X_BMP_HEADER + v9x_stride24(ow) * oh;
    if ((options->flags & (V9X_SHOT_COMPACT | V9X_SHOT_GRAY)) == 0ul) {
        return V9X_SHOTXF_OK;
    }
    colours = v9x_shotxf_index(output + V9X_BMP_HEADER, ow, oh,
                               (options->flags & V9X_SHOT_GRAY) != 0ul, work);
    if (colours == 0ul) return V9X_SHOTXF_OK;  /* > 256 colours: keep 24-bit */

    /* The 24-bit image is no longer needed: write the 8-bit one over it,
       RLE8 when that is smaller than the raw rows. */
    header8 = V9X_BMP_HEADER + colours * 4ul;
    raw8 = v9x_stride8(ow) * oh;
    for (y = 0ul; y < colours * 4ul; ++y) output[V9X_BMP_HEADER + y] = work[y];
    encoded = v9x_shotxf_rle8(work + V9X_WORK_INDICES, ow, oh,
                              output + header8, raw8);
    if (encoded != 0ul && encoded < raw8) {
        v9x_bmp_header(output, ow, oh, 8ul, 1ul /* BI_RLE8 */, colours, encoded);
        *output_length = header8 + encoded;
    } else {
        v9x_u32 stride = v9x_stride8(ow);
        v9x_u32 x;
        for (y = 0ul; y < oh; ++y) {
            v9x_u8 *row = output + header8 + y * stride;
            const v9x_u8 *from = work + V9X_WORK_INDICES + y * ow;
            for (x = 0ul; x < ow; ++x) row[x] = from[x];
            for (; x < stride; ++x) row[x] = 0u;
        }
        v9x_bmp_header(output, ow, oh, 8ul, 0ul, colours, raw8);
        *output_length = header8 + raw8;
    }
    *output_bits = 8ul;
    return V9X_SHOTXF_OK;
}
