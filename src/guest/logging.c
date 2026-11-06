#include "agent.h"
#include "v9xremote/protocol.h"

static const char v9x_log_path[] = "C:\\V9XREMOTE\\AGENT.LOG";
static const char v9x_log_old_path[] = "C:\\V9XREMOTE\\AGENT.OLD";
static const char v9x_events_path[] = "C:\\V9XREMOTE\\EVENTS.LOG";
static const char v9x_events_old_path[] = "C:\\V9XREMOTE\\EVENTS.OLD";
#define V9X_EVENTS_ROTATE_BYTES 65536ul
#define V9X_LOG_ROTATE_BYTES 262144ul
#define V9X_LOG_EVENT_MAX 160ul
#define V9X_LOG_DETAIL_MAX 240ul
#define V9X_LOG_LINE_MAX 512ul

static DWORD v9x_log_boot = 0ul;
static volatile LONG v9x_log_frozen = 0l;

void v9x_log_set_boot(DWORD boot_counter)
{
    v9x_log_boot = boot_counter;
}

/* RESET freezes the log once its flush starts, so no agent thread (a client
   reconnecting to look for the reset, say) dirties the disk cache again. */
void v9x_log_freeze(int frozen)
{
    v9x_log_frozen = frozen ? 1l : 0l;
}

static unsigned long v9x_log_pad(char *target, unsigned long offset,
                                 unsigned long value, unsigned long digits)
{
    char reversed[10];
    unsigned long count = 0ul;
    unsigned long index;
    do {
        reversed[count++] = (char)('0' + (value % 10ul));
        value /= 10ul;
    } while (value != 0ul && count < sizeof(reversed));
    while (count < digits && count < sizeof(reversed)) reversed[count++] = '0';
    for (index = 0ul; index < count; ++index) {
        target[offset + index] = reversed[count - index - 1ul];
    }
    return offset + count;
}

static unsigned long v9x_log_copy(char *target, unsigned long offset,
                                  const char *text, unsigned long maximum)
{
    unsigned long length = v9x_bounded_length(text, maximum);
    unsigned long index;
    for (index = 0ul; index < length; ++index) {
        unsigned char ch = (unsigned char)text[index];
        if (ch < 0x20u || ch > 0x7eu) ch = '?';
        target[offset + index] = (char)ch;
    }
    return offset + length;
}

/* Rotate a log to its .OLD once it passes `cap` bytes so it never grows
   without bound (design section 8.8). The move needs no open handle, so this
   runs before the append opens the file. */
static void v9x_log_rotate_file(const char *path, const char *old_path,
                                unsigned long cap)
{
    HANDLE file;
    DWORD size;
    DWORD high = 0ul;
    file = CreateFileA(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return;
    size = GetFileSize(file, &high);
    CloseHandle(file);
    if (high == 0ul && size != 0xfffffffful && size < cap) {
        return;
    }
    (void)DeleteFileA(old_path);
    (void)MoveFileA(path, old_path);
}

/* "YYYY-MM-DD HH:MM" (and ":SS" when `seconds`) at target[0]; returns the
   length. */
unsigned long v9x_log_stamp(char *target, int seconds)
{
    SYSTEMTIME now;
    unsigned long offset = 0ul;
    GetLocalTime(&now);
    offset = v9x_log_pad(target, offset, now.wYear, 4ul);
    target[offset++] = '-';
    offset = v9x_log_pad(target, offset, now.wMonth, 2ul);
    target[offset++] = '-';
    offset = v9x_log_pad(target, offset, now.wDay, 2ul);
    target[offset++] = ' ';
    offset = v9x_log_pad(target, offset, now.wHour, 2ul);
    target[offset++] = ':';
    offset = v9x_log_pad(target, offset, now.wMinute, 2ul);
    if (seconds) {
        target[offset++] = ':';
        offset = v9x_log_pad(target, offset, now.wSecond, 2ul);
    }
    target[offset] = '\0';
    return offset;
}

/* Append bytes to a log, rotating it first. Two agent threads can append at
   once and the file is opened without write sharing, so a sharing
   violation is retried briefly instead of losing the line. */
int v9x_log_append_file(const char *path, const char *old_path,
                        unsigned long cap, const char *data,
                        unsigned long length)
{
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD written = 0ul;
    int attempt;
    if (v9x_log_frozen) return 0;
    v9x_log_rotate_file(path, old_path, cap);
    for (attempt = 0; attempt < 20; ++attempt) {
        file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_SHARING_VIOLATION) return 0;
        Sleep(10ul);
    }
    if (file == INVALID_HANDLE_VALUE) return 0;
    (void)SetFilePointer(file, 0l, 0, FILE_END);
    (void)WriteFile(file, data, (DWORD)length, &written, 0);
    CloseHandle(file);
    return written == (DWORD)length;
}

void v9x_log_event(const char *event_name, const char *detail)
{
    char line[V9X_LOG_LINE_MAX];
    unsigned long offset = 0ul;

    if (v9x_log_frozen) return;
    offset = v9x_log_stamp(line, 1);
    line[offset++] = ' ';
    line[offset++] = 'b';
    offset = v9x_log_pad(line, offset, v9x_log_boot, 1ul);
    line[offset++] = ' ';
    offset = v9x_log_copy(line, offset, event_name, V9X_LOG_EVENT_MAX);
    if (detail != 0 && detail[0] != '\0') {
        line[offset++] = ' ';
        offset = v9x_log_copy(line, offset, detail, V9X_LOG_DETAIL_MAX);
    }
    line[offset++] = '\r';
    line[offset++] = '\n';
    (void)v9x_log_append_file(v9x_log_path, v9x_log_old_path,
                              V9X_LOG_ROTATE_BYTES, line, offset);
}

void v9x_log_line(const char *event_name)
{
    v9x_log_event(event_name, 0);
}

/* The tester-facing activity log, EVENTS.LOG: one plain-English line per
   thing a person at the machine may want to know ("Developer connected",
   "Idle: safe to reboot", the developer's own NOTE lines), opened from the
   tray. AGENT.LOG stays the technical record. `text` and `detail` are
   joined as given; control characters become spaces. */
void v9x_event(const char *text, const char *detail)
{
    char line[V9X_LOG_LINE_MAX];
    unsigned long offset;
    const char *part[2];
    int which;
    if (v9x_log_frozen) return;
    offset = v9x_log_stamp(line, 1);
    line[offset++] = ' ';
    part[0] = text;
    part[1] = detail;
    for (which = 0; which < 2; ++which) {
        const char *at = part[which];
        if (at == 0) continue;
        while (*at != '\0' && offset < V9X_LOG_LINE_MAX - 3ul) {
            unsigned char ch = (unsigned char)*at++;
            line[offset++] = ch < 0x20u ? ' ' : (char)ch;
        }
    }
    line[offset++] = '\r';
    line[offset++] = '\n';
    (void)v9x_log_append_file(v9x_events_path, v9x_events_old_path,
                              V9X_EVENTS_ROTATE_BYTES, line, offset);
}
