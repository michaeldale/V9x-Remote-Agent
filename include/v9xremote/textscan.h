/*
 * Line filtering for READ_TEXT (`v9xctl cat`): the last N lines of a file,
 * optionally only those containing a substring, into one bounded buffer.
 *
 * The agent feeds the file through in chunks, twice when a tail is asked
 * for: pass 1 (count) only counts matching lines, pass 2 (emit) skips all
 * but the last `tail` of them. Lines end at LF; a CR before it is dropped
 * and every emitted line ends with LF. A line longer than
 * V9X_TEXT_LINE_MAX is cut there (the rest is not matched or emitted) and
 * flagged. When the output fills, emission stops and the result is flagged
 * truncated rather than cutting a line in half.
 *
 * Pure like the rest of src\common: no I/O, no allocation, no C runtime.
 */
#ifndef V9XREMOTE_TEXTSCAN_H
#define V9XREMOTE_TEXTSCAN_H

#include "v9xremote/types.h"

#define V9X_TEXT_LINE_MAX 1024ul
#define V9X_TEXT_MATCH_MAX 128ul

/* Result flags (READ_TEXT_RESPONSE). */
#define V9X_TEXT_TRUNCATED 0x00000001ul  /* output full: later lines omitted */
#define V9X_TEXT_LONG_LINE 0x00000002ul  /* some line was cut at LINE_MAX */

typedef struct V9xTextScan {
    /* setup */
    v9x_u32 skip;             /* matching lines to pass over before emitting */
    int emit;                 /* 0 = count pass, 1 = emit pass */
    const char *match;        /* case-insensitive ASCII substring, or 0 */
    v9x_u32 match_length;
    v9x_u8 *output;
    v9x_u32 capacity;
    /* results */
    v9x_u32 used;
    v9x_u32 lines;            /* every line seen */
    v9x_u32 matched;          /* lines that matched (all lines without a match) */
    v9x_u32 emitted;
    v9x_u32 flags;
    /* the line being assembled */
    v9x_u32 line_length;
    int line_cut;
    v9x_u8 line[V9X_TEXT_LINE_MAX];
} V9xTextScan;

void v9x_text_begin(V9xTextScan *scan, int emit, v9x_u32 skip,
                    const char *match, v9x_u32 match_length,
                    v9x_u8 *output, v9x_u32 capacity);
void v9x_text_feed(V9xTextScan *scan, const v9x_u8 *data, v9x_u32 length);
/* Flushes a last line without LF. */
void v9x_text_end(V9xTextScan *scan);

#endif
