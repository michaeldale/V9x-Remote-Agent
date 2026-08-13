#include "agent.h"
#include "v9xremote/crc32.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"

#define V9X_SCREEN_MAX_BYTES 16777216ul
#define V9X_SCREEN_HELPER_TIMEOUT 7500ul

static const char v9x_screen_helper[] = "C:\\V9XREMOTE\\V9XSHOT.EXE";
static const char v9x_screen_capture[] = "C:\\V9XREMOTE\\TEMP\\CAPTURE.$$$";
static const char v9x_screen_metadata[] = "C:\\V9XREMOTE\\TEMP\\CAPTURE.DAT";
static unsigned char v9x_screen_response[320];

static int v9x_screen_path(const unsigned char *payload, unsigned long length,
                           char *path)
{
    unsigned long path_length;
    unsigned long index;
    if (length < 3ul) return 0;
    path_length = (unsigned long)v9x_read_u16(payload);
    if (path_length == 0ul || path_length > 259ul || path_length + 2ul != length) {
        return 0;
    }
    for (index = 0ul; index < path_length; ++index) {
        if (payload[index + 2ul] == 0u || payload[index + 2ul] > 0x7fu) return 0;
        path[index] = (char)payload[index + 2ul];
    }
    path[path_length] = '\0';
    return 1;
}

static int v9x_ascii_path_equal(const char *left, const char *right)
{
    unsigned char a;
    unsigned char b;
    for (;;) {
        a = (unsigned char)*left++;
        b = (unsigned char)*right++;
        if (a >= 'a' && a <= 'z') a = (unsigned char)(a - ('a' - 'A'));
        if (b >= 'a' && b <= 'z') b = (unsigned char)(b - ('a' - 'A'));
        if (a != b) return 0;
        if (a == 0u) return 1;
    }
}

static int v9x_screen_error(V9xAgentState *state, DWORD request_id,
                            DWORD status, DWORD native_error,
                            const char *detail)
{
    unsigned long offset = 8ul;
    v9x_write_u32(v9x_screen_response, status);
    v9x_write_u32(v9x_screen_response + 4, native_error);
    if (!v9x_append_string(v9x_screen_response, sizeof(v9x_screen_response),
                           &offset, detail)) return 0;
    return v9x_send_frame(state, V9X_MSG_ERROR_RESPONSE, request_id,
                          v9x_screen_response, offset);
}

static int v9x_write_all(HANDLE file, const unsigned char *data, DWORD length,
                         DWORD *native_error)
{
    DWORD offset = 0ul;
    DWORD written;
    while (offset < length) {
        if (!WriteFile(file, data + offset, length - offset, &written, 0) ||
            written == 0ul) {
            *native_error = GetLastError();
            return 0;
        }
        offset += written;
    }
    return 1;
}

static int v9x_read_all(HANDLE file, unsigned char *data, DWORD length,
                        DWORD *native_error)
{
    DWORD offset = 0ul;
    DWORD received;
    while (offset < length) {
        if (!ReadFile(file, data + offset, length - offset, &received, 0) ||
            received == 0ul) {
            *native_error = GetLastError();
            return 0;
        }
        offset += received;
    }
    return 1;
}

int v9x_desktop_ready(void)
{
    HWND desktop = GetDesktopWindow();
    HWND shell_desktop = FindWindowA("Progman", 0);
    HWND taskbar = FindWindowA("Shell_TrayWnd", 0);
    return desktop != 0 && IsWindowVisible(desktop) &&
           (shell_desktop != 0 || taskbar != 0);
}

void v9x_screen_info(DWORD *width, DWORD *height, DWORD *bits_per_pixel)
{
    /* Keep HELLO and INFO out of GDI.  A damaged display driver must not be
       able to fault the long-lived agent merely because a client connects. */
    *width = (DWORD)GetSystemMetrics(SM_CXSCREEN);
    *height = (DWORD)GetSystemMetrics(SM_CYSCREEN);
    *bits_per_pixel = 0ul;
}

static int v9x_run_screen_helper(DWORD *native_error)
{
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    char command_line[64];
    DWORD wait_result;
    DWORD exit_code = 0ul;
    unsigned long index;

    for (index = 0ul; index < sizeof(startup); ++index)
        ((unsigned char *)&startup)[index] = 0u;
    for (index = 0ul; index < sizeof(process); ++index)
        ((unsigned char *)&process)[index] = 0u;
    startup.cb = sizeof(startup);
    for (index = 0ul; v9x_screen_helper[index] != '\0'; ++index)
        command_line[index] = v9x_screen_helper[index];
    command_line[index] = '\0';

    (void)DeleteFileA(v9x_screen_capture);
    (void)DeleteFileA(v9x_screen_metadata);
    if (!CreateProcessA(v9x_screen_helper, command_line, 0, 0, FALSE,
                        CREATE_NEW_PROCESS_GROUP, 0, "C:\\V9XREMOTE\\TEMP",
                        &startup, &process)) {
        *native_error = GetLastError();
        return 0;
    }
    CloseHandle(process.hThread);
    wait_result = WaitForSingleObject(process.hProcess,
                                      V9X_SCREEN_HELPER_TIMEOUT);
    if (wait_result == WAIT_TIMEOUT) {
        v9x_log_line("screenshot-helper-timeout");
        (void)TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        (void)WaitForSingleObject(process.hProcess, 1000ul);
        *native_error = ERROR_TIMEOUT;
        CloseHandle(process.hProcess);
        return 0;
    }
    if (wait_result != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &exit_code)) {
        *native_error = GetLastError();
        CloseHandle(process.hProcess);
        return 0;
    }
    CloseHandle(process.hProcess);
    if (exit_code != 0ul) {
        v9x_log_line("screenshot-helper-failed");
        *native_error = exit_code;
        return 0;
    }
    return 1;
}

int v9x_capture_screenshot(V9xAgentState *state, unsigned long request_id,
                           const unsigned char *payload,
                           unsigned long length)
{
    char path[260];
    unsigned char metadata[20];
    unsigned char *image = 0;
    HGLOBAL allocation = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD width;
    DWORD height;
    DWORD source_bpp;
    DWORD file_bytes;
    DWORD actual_bytes;
    DWORD native_error = 0ul;
    DWORD crc;
    unsigned long response_offset = 20ul;
    int success = 0;

    if (!v9x_screen_path(payload, length, path)) {
        return v9x_screen_error(state, request_id, V9X_STATUS_INVALID_PAYLOAD,
                                ERROR_INVALID_PARAMETER,
                                "invalid screenshot path");
    }
    if (v9x_ascii_path_equal(path, v9x_screen_capture) ||
        v9x_ascii_path_equal(path, v9x_screen_metadata)) {
        return v9x_screen_error(state, request_id, V9X_STATUS_INVALID_PAYLOAD,
                                ERROR_INVALID_PARAMETER,
                                "screenshot path is reserved");
    }
    if (InterlockedCompareExchange(&state->exec_active, 0l, 0l) != 0l) {
        return v9x_screen_error(state, request_id, V9X_STATUS_BUSY, 0ul,
                                "execution active");
    }
    if (!v9x_desktop_ready()) {
        return v9x_screen_error(state, request_id, V9X_STATUS_BUSY, 0ul,
                                "desktop not ready for screenshot");
    }
    v9x_log_line("screenshot-helper-start");
    if (!v9x_run_screen_helper(&native_error)) goto cleanup;

    file = CreateFileA(v9x_screen_metadata, GENERIC_READ, FILE_SHARE_READ, 0,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) { native_error = GetLastError(); goto cleanup; }
    if (GetFileSize(file, 0) != sizeof(metadata) ||
        !v9x_read_all(file, metadata, sizeof(metadata), &native_error)) goto cleanup;
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    if (v9x_read_u32(metadata) != 0x31533956ul) {
        native_error = ERROR_INVALID_DATA; goto cleanup;
    }
    width = v9x_read_u32(metadata + 4);
    height = v9x_read_u32(metadata + 8);
    source_bpp = v9x_read_u32(metadata + 12);
    file_bytes = v9x_read_u32(metadata + 16);
    if (width == 0ul || height == 0ul || width > 4096ul || height > 4096ul ||
        file_bytes < 54ul || file_bytes > V9X_SCREEN_MAX_BYTES + 54ul) {
        native_error = ERROR_INVALID_DATA; goto cleanup;
    }

    file = CreateFileA(v9x_screen_capture, GENERIC_READ, FILE_SHARE_READ, 0,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) { native_error = GetLastError(); goto cleanup; }
    actual_bytes = GetFileSize(file, 0);
    if (actual_bytes != file_bytes || actual_bytes == 0xfffffffful) {
        native_error = ERROR_INVALID_DATA; goto cleanup;
    }
    allocation = GlobalAlloc(GMEM_FIXED, file_bytes);
    if (allocation == 0) { native_error = ERROR_NOT_ENOUGH_MEMORY; goto cleanup; }
    image = (unsigned char *)allocation;
    if (!v9x_read_all(file, image, file_bytes, &native_error)) goto cleanup;
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    if (image[0] != 'B' || image[1] != 'M' ||
        v9x_read_u32(image + 2) != file_bytes ||
        v9x_read_u32(image + 18) != width ||
        v9x_read_u32(image + 22) != height) {
        native_error = ERROR_INVALID_DATA; goto cleanup;
    }

    file = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) { native_error = GetLastError(); goto cleanup; }
    if (!v9x_write_all(file, image, file_bytes, &native_error) ||
        !FlushFileBuffers(file)) {
        if (native_error == 0ul) native_error = GetLastError();
        goto cleanup;
    }
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    crc = v9x_crc32_begin();
    crc = v9x_crc32_update(crc, image, file_bytes);
    crc = v9x_crc32_end(crc);
    v9x_write_u32(v9x_screen_response, width);
    v9x_write_u32(v9x_screen_response + 4, height);
    v9x_write_u32(v9x_screen_response + 8, source_bpp);
    v9x_write_u32(v9x_screen_response + 12, file_bytes);
    v9x_write_u32(v9x_screen_response + 16, crc);
    if (!v9x_append_string(v9x_screen_response, sizeof(v9x_screen_response),
                           &response_offset, path)) goto cleanup;
    success = 1;

cleanup:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!success) (void)DeleteFileA(path);
    if (allocation != 0) (void)GlobalFree(allocation);
    (void)DeleteFileA(v9x_screen_capture);
    (void)DeleteFileA(v9x_screen_metadata);
    if (!success) {
        return v9x_screen_error(state, request_id, V9X_STATUS_IO_FAILED,
                                native_error, "screenshot helper failed");
    }
    v9x_log_line("screenshot-complete");
    return v9x_send_frame(state, V9X_MSG_SCREENSHOT_RESPONSE, request_id,
                          v9x_screen_response, response_offset);
}
