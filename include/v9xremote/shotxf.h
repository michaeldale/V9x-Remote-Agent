/*
 * Screenshot shaping: crop, scale and repack the agent's 24-bit capture
 * before it crosses the wire.
 *
 * The screenshot helper always writes a bottom-up 24-bit BI_RGB BMP: 0.9 MB
 * at 640x480 and 2.4 MB at 1024x768, which takes tens of seconds over the
 * remote-help relay, and a full frame is also the most expensive thing an AI
 * client can look at. A host that sees V9X_CAP_SCREENSHOT_OPTIONS sends a
 * V9xShotOptions block after the path; the agent runs this transform on the
 * capture and writes the result instead. Every output is still a standard
 * Windows BMP (24-bit BI_RGB, 8-bit BI_RGB or 8-bit BI_RLE8), so any image
 * library opens it.
 *
 * - Region: a rectangle in screen pixels, clipped to the screen.
 * - Scale: 1-100 percent, each output pixel the average of the source box it
 *   covers (text stays readable at 50 percent; nearest-neighbour does not).
 * - COMPACT: if the image has at most 256 distinct colours (any 8-bpp
 *   desktop, and most 16/24-bpp dialogs), store it as 8-bit with an exact
 *   palette. Lossless. Otherwise it stays 24-bit.
 * - GRAY: 8-bit greyscale. Lossy; for reading text and layout.
 * Any 8-bit result is RLE8-compressed when that is smaller.
 *
 * Pure like the rest of src\common: no I/O, no allocation, no C runtime, no
 * static mutable state. The caller provides the output and work buffers,
 * sized with the two capacity functions.
 */
#ifndef V9XREMOTE_SHOTXF_H
#define V9XREMOTE_SHOTXF_H

#include "v9xremote/types.h"

#define V9X_SHOT_COMPACT 0x00000001ul
#define V9X_SHOT_GRAY    0x00000002ul
#define V9X_SHOT_KNOWN_FLAGS (V9X_SHOT_COMPACT | V9X_SHOT_GRAY)

/* Wire size of the options block that follows the path string. */
#define V9X_SHOT_OPTIONS_SIZE 24ul

#define V9X_SHOTXF_OK          ((v9x_u16)0u)
#define V9X_SHOTXF_BAD_SOURCE  ((v9x_u16)1u)  /* not a bottom-up 24-bit BI_RGB BMP */
#define V9X_SHOTXF_BAD_OPTIONS ((v9x_u16)2u)  /* unknown flag, scale > 100, empty region */
#define V9X_SHOTXF_NO_ROOM     ((v9x_u16)3u)  /* output or work buffer too small */

typedef struct V9xShotOptions {
    v9x_u32 flags;          /* V9X_SHOT_* */
    v9x_u32 x;              /* region in screen pixels; width or height 0 */
    v9x_u32 y;              /* means the whole screen */
    v9x_u32 width;
    v9x_u32 height;
    v9x_u32 scale_percent;  /* 1..100; 0 means 100 */
} V9xShotOptions;

/* Little-endian u32 x6, in the field order above. Returns 0 if length is
   not V9X_SHOT_OPTIONS_SIZE. Values are checked by v9x_shotxf_plan. */
int v9x_shot_options_decode(const v9x_u8 *data, v9x_u32 length,
                            V9xShotOptions *options);
void v9x_shot_options_encode(const V9xShotOptions *options, v9x_u8 *data);

/* True if the options ask for anything (so the raw capture can be sent
   untouched otherwise). */
int v9x_shot_options_active(const V9xShotOptions *options);

/* Resolve the region and output size for a screen of screen_width x
   screen_height. Returns V9X_SHOTXF_OK or V9X_SHOTXF_BAD_OPTIONS. */
v9x_u16 v9x_shotxf_plan(const V9xShotOptions *options,
                        v9x_u32 screen_width, v9x_u32 screen_height,
                        v9x_u32 *region_x, v9x_u32 *region_y,
                        v9x_u32 *region_width, v9x_u32 *region_height,
                        v9x_u32 *output_width, v9x_u32 *output_height);

/* Buffer sizes for an output of output_width x output_height. */
v9x_u32 v9x_shotxf_output_capacity(v9x_u32 output_width, v9x_u32 output_height);
v9x_u32 v9x_shotxf_work_capacity(v9x_u32 output_width, v9x_u32 output_height);

/* Shape `source` (the helper's BMP) into `output`. On success *output_length
   is the BMP's size and *output_bits 24 or 8. `work` must not overlap
   either image. */
v9x_u16 v9x_shotxf_apply(const v9x_u8 *source, v9x_u32 source_length,
                         const V9xShotOptions *options,
                         v9x_u8 *output, v9x_u32 output_capacity,
                         v9x_u8 *work, v9x_u32 work_capacity,
                         v9x_u32 *output_length,
                         v9x_u32 *output_width, v9x_u32 *output_height,
                         v9x_u32 *output_bits);

#endif
