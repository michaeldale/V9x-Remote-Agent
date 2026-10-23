/* Line filtering behind READ_TEXT / `v9xctl cat` (src\common\textscan.c). */
#include "v9xremote/textscan.h"
#include <stdio.h>
#include <string.h>

static V9xTextScan scan;
static v9x_u8 out[256];

/* Feed `text` in `chunk`-byte pieces (to cross line boundaries mid-chunk),
   two passes when tail is set, as the agent does. Returns the output. */
static const char *run(const char *text, v9x_u32 chunk, v9x_u32 tail,
                       const char *match, v9x_u32 capacity)
{
    v9x_u32 length = (v9x_u32)strlen(text);
    v9x_u32 skip = 0ul;
    v9x_u32 at;
    v9x_u32 match_length = match != 0 ? (v9x_u32)strlen(match) : 0ul;
    if (tail != 0ul) {
        v9x_text_begin(&scan, 0, 0ul, match, match_length, 0, 0ul);
        for (at = 0ul; at < length; at += chunk) {
            v9x_text_feed(&scan, (const v9x_u8 *)text + at,
                          length - at < chunk ? length - at : chunk);
        }
        v9x_text_end(&scan);
        if (scan.matched > tail) skip = scan.matched - tail;
    }
    v9x_text_begin(&scan, 1, skip, match, match_length, out, capacity);
    for (at = 0ul; at < length; at += chunk) {
        v9x_text_feed(&scan, (const v9x_u8 *)text + at,
                      length - at < chunk ? length - at : chunk);
    }
    v9x_text_end(&scan);
    out[scan.used] = '\0';
    return (const char *)out;
}

int main(void)
{
    static const char log[] = "boot ok\r\nINFO one\nwarn: disk\r\nINFO two\nlast line no lf";
    static char long_line[1500];
    if (strcmp(run(log, 3ul, 0ul, 0, 200ul),
               "boot ok\nINFO one\nwarn: disk\nINFO two\nlast line no lf\n") != 0) return 1;
    if (scan.lines != 5ul || scan.matched != 5ul || scan.emitted != 5ul || scan.flags != 0ul) return 2;
    if (strcmp(run(log, 7ul, 2ul, 0, 200ul), "INFO two\nlast line no lf\n") != 0) return 3;
    if (strcmp(run(log, 1ul, 0ul, "info", 200ul), "INFO one\nINFO two\n") != 0) return 4;
    if (scan.matched != 2ul || scan.lines != 5ul) return 5;
    if (strcmp(run(log, 64ul, 1ul, "INFO", 200ul), "INFO two\n") != 0) return 6;
    if (strcmp(run(log, 64ul, 0ul, "nothing", 200ul), "") != 0 || scan.matched != 0ul) return 7;
    /* Full output stops on a line boundary and says so. */
    if (strcmp(run(log, 64ul, 0ul, 0, 20ul), "boot ok\nINFO one\n") != 0 ||
        (scan.flags & V9X_TEXT_TRUNCATED) == 0ul) return 8;
    /* Over-long lines are cut and flagged; the next line is intact. */
    memset(long_line, 'x', sizeof(long_line) - 6u);
    strcpy(long_line + sizeof(long_line) - 6u, "\nend\n");
    run(long_line, 100ul, 1ul, 0, 255ul);
    if (strcmp((const char *)out, "end\n") != 0 || (scan.flags & V9X_TEXT_LONG_LINE) == 0ul) return 9;
    if (strcmp(run("", 4ul, 3ul, 0, 10ul), "") != 0 || scan.lines != 0ul) return 10;
    printf("textscan tests passed\n");
    return 0;
}
