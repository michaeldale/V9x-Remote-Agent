#include "agent.h"
#include "v9xremote/protocol.h"

static const char v9x_log_path[] = "C:\\V9XREMOTE\\AGENT.LOG";

void v9x_log_line(const char *event_name)
{
    HANDLE file;
    DWORD written;
    DWORD length = (DWORD)v9x_bounded_length(event_name, 160ul);
    static const char ending[] = "\r\n";
    file = CreateFileA(v9x_log_path, GENERIC_WRITE, FILE_SHARE_READ, 0,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    (void)SetFilePointer(file, 0l, 0, FILE_END);
    (void)WriteFile(file, event_name, length, &written, 0);
    (void)WriteFile(file, ending, 2ul, &written, 0);
    CloseHandle(file);
}

