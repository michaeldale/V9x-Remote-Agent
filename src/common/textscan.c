#include "v9xremote/textscan.h"

/* See include\v9xremote\textscan.h. */

void v9x_text_begin(V9xTextScan *scan, int emit, v9x_u32 skip,
                    const char *match, v9x_u32 match_length,
                    v9x_u8 *output, v9x_u32 capacity)
{
    scan->skip = skip;
    scan->emit = emit;
    scan->match = match_length != 0ul ? match : 0;
    scan->match_length = match_length;
    scan->output = output;
    scan->capacity = capacity;
    scan->used = 0ul;
    scan->lines = 0ul;
    scan->matched = 0ul;
    scan->emitted = 0ul;
    scan->flags = 0ul;
    scan->line_length = 0ul;
    scan->line_cut = 0;
}

static v9x_u8 v9x_text_lower(v9x_u8 ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (v9x_u8)(ch + ('a' - 'A')) : ch;
}

static int v9x_text_matches(const V9xTextScan *scan)
{
    v9x_u32 start;
    v9x_u32 index;
    if (scan->match == 0) return 1;
    if (scan->match_length > scan->line_length) return 0;
    for (start = 0ul; start + scan->match_length <= scan->line_length; ++start) {
        for (index = 0ul; index < scan->match_length; ++index) {
            if (v9x_text_lower(scan->line[start + index]) !=
                v9x_text_lower((v9x_u8)scan->match[index])) break;
        }
        if (index == scan->match_length) return 1;
    }
    return 0;
}

static void v9x_text_line_done(V9xTextScan *scan)
{
    v9x_u32 length = scan->line_length;
    v9x_u32 index;
    if (length != 0ul && scan->line[length - 1ul] == '\r') --length;
    scan->line_length = length;
    ++scan->lines;
    if (scan->line_cut) scan->flags |= V9X_TEXT_LONG_LINE;
    if (v9x_text_matches(scan)) {
        ++scan->matched;
        if (scan->emit && scan->matched > scan->skip &&
            (scan->flags & V9X_TEXT_TRUNCATED) == 0ul) {
            if (scan->used + length + 1ul > scan->capacity) {
                scan->flags |= V9X_TEXT_TRUNCATED;
            } else {
                for (index = 0ul; index < length; ++index) {
                    scan->output[scan->used++] = scan->line[index];
                }
                scan->output[scan->used++] = '\n';
                ++scan->emitted;
            }
        }
    }
    scan->line_length = 0ul;
    scan->line_cut = 0;
}

void v9x_text_feed(V9xTextScan *scan, const v9x_u8 *data, v9x_u32 length)
{
    v9x_u32 index;
    for (index = 0ul; index < length; ++index) {
        v9x_u8 ch = data[index];
        if (ch == '\n') {
            v9x_text_line_done(scan);
        } else if (scan->line_length < V9X_TEXT_LINE_MAX) {
            scan->line[scan->line_length++] = ch;
        } else {
            scan->line_cut = 1;
        }
    }
}

void v9x_text_end(V9xTextScan *scan)
{
    if (scan->line_length != 0ul || scan->line_cut) v9x_text_line_done(scan);
}
