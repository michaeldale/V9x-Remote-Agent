#include "agent.h"
#include "v9xremote/protocol.h"

static const char v9x_log_path[] = "C:\\V9XREMOTE\\AGENT.LOG";
static const char v9x_log_old_path[] = "C:\\V9XREMOTE\\AGENT.OLD";
#define V9X_LOG_ROTATE_BYTES 262144ul
#define V9X_LOG_EVENT_MAX 160ul
#define V9X_LOG_DETAIL_MAX 240ul
#define V9X_LOG_LINE_MAX 512ul

static DWORD v9x_log_boot = 0ul;

void v9x_log_set_boot(DWORD boot_counter)
{
    v9x_log_boot = boot_counter;
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

/* Rotate AGENT.LOG to AGENT.OLD once it passes the size cap so the log never
   grows without bound (design section 8.8). The move needs no open handle, so
   this runs before the append opens the file. */
static void v9x_log_rotate(void)
{
    HANDLE file;
    DWORD size;
    DWORD high = 0ul;
    file = CreateFileA(v9x_log_path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return;
    size = GetFileSize(file, &high);
    CloseHandle(file);
    if (high == 0ul && size != 0xfffffffful && size < V9X_LOG_ROTATE_BYTES) {
        return;
    }
    (void)DeleteFileA(v9x_log_old_path);
    (void)MoveFileA(v9x_log_path, v9x_log_old_path);
}

void v9x_log_event(const char *event_name, const char *detail)
{
    HANDLE file;
    DWORD written;
    SYSTEMTIME now;
    char line[V9X_LOG_LINE_MAX];
    unsigned long offset = 0ul;

    v9x_log_rotate();

    GetLocalTime(&now);
    offset = v9x_log_pad(line, offset, now.wYear, 4ul);
    line[offset++] = '-';
    offset = v9x_log_pad(line, offset, now.wMonth, 2ul);
    line[offset++] = '-';
    offset = v9x_log_pad(line, offset, now.wDay, 2ul);
    line[offset++] = ' ';
    offset = v9x_log_pad(line, offset, now.wHour, 2ul);
    line[offset++] = ':';
    offset = v9x_log_pad(line, offset, now.wMinute, 2ul);
    line[offset++] = ':';
    offset = v9x_log_pad(line, offset, now.wSecond, 2ul);
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

    file = CreateFileA(v9x_log_path, GENERIC_WRITE, FILE_SHARE_READ, 0,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    (void)SetFilePointer(file, 0l, 0, FILE_END);
    (void)WriteFile(file, line, (DWORD)offset, &written, 0);
    CloseHandle(file);
}

void v9x_log_line(const char *event_name)
{
    v9x_log_event(event_name, 0);
}
