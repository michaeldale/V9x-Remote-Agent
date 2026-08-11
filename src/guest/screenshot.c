#include "agent.h"
#include "v9xremote/crc32.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"

#define V9X_SCREEN_MAX_BYTES 16777216ul

static unsigned char v9x_screen_response[320];

static void v9x_screen_zero(void *target, unsigned long length)
{
    unsigned char *bytes = (unsigned char *)target;
    unsigned long index;
    for (index = 0ul; index < length; ++index) bytes[index] = 0u;
}

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
    HDC screen = GetDC(0);
    *width = (DWORD)GetSystemMetrics(SM_CXSCREEN);
    *height = (DWORD)GetSystemMetrics(SM_CYSCREEN);
    *bits_per_pixel = 0ul;
    if (screen != 0) {
        *bits_per_pixel = (DWORD)(GetDeviceCaps(screen, BITSPIXEL) *
                                  GetDeviceCaps(screen, PLANES));
        (void)ReleaseDC(0, screen);
    }
}

int v9x_capture_screenshot(V9xAgentState *state, unsigned long request_id,
                           const unsigned char *payload,
                           unsigned long length)
{
    char path[260];
    HDC screen = 0;
    HDC memory = 0;
    HBITMAP bitmap = 0;
    HGDIOBJ old_object = 0;
    BITMAPINFO info;
    unsigned char bitmap_header[14];
    unsigned char dib_header[40];
    unsigned char *pixels = 0;
    HGLOBAL allocation = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD width;
    DWORD height;
    DWORD source_bpp;
    DWORD row_bytes;
    DWORD image_bytes;
    DWORD file_bytes;
    DWORD native_error = 0ul;
    DWORD crc;
    unsigned long response_offset = 20ul;
    int success = 0;

    if (!v9x_screen_path(payload, length, path)) {
        return v9x_screen_error(state, request_id, V9X_STATUS_INVALID_PAYLOAD,
                                ERROR_INVALID_PARAMETER,
                                "invalid screenshot path");
    }
    if (InterlockedCompareExchange(&state->exec_active, 0l, 0l) != 0l) {
        return v9x_screen_error(state, request_id, V9X_STATUS_BUSY, 0ul,
                                "execution active");
    }
    v9x_screen_info(&width, &height, &source_bpp);
    if (width == 0ul || height == 0ul || width > 4096ul || height > 4096ul ||
        width > (0xfffffffful - 3ul) / 3ul) {
        return v9x_screen_error(state, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                                ERROR_INSUFFICIENT_BUFFER,
                                "screen dimensions exceed limits");
    }
    row_bytes = (width * 3ul + 3ul) & ~3ul;
    if (height > V9X_SCREEN_MAX_BYTES / row_bytes) {
        return v9x_screen_error(state, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                                ERROR_INSUFFICIENT_BUFFER,
                                "screenshot exceeds memory limit");
    }
    image_bytes = row_bytes * height;
    file_bytes = 54ul + image_bytes;
    screen = GetDC(0);
    if (screen == 0) { native_error = GetLastError(); goto cleanup; }
    memory = CreateCompatibleDC(screen);
    if (memory == 0) { native_error = GetLastError(); goto cleanup; }
    bitmap = CreateCompatibleBitmap(screen, (int)width, (int)height);
    if (bitmap == 0) { native_error = GetLastError(); goto cleanup; }
    old_object = SelectObject(memory, bitmap);
    if (old_object == 0 || old_object == HGDI_ERROR) {
        native_error = GetLastError(); goto cleanup;
    }
    if (!BitBlt(memory, 0, 0, (int)width, (int)height,
                screen, 0, 0, SRCCOPY)) {
        native_error = GetLastError(); goto cleanup;
    }
    (void)SelectObject(memory, old_object);
    old_object = 0;
    v9x_screen_zero(&info, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = (LONG)width;
    info.bmiHeader.biHeight = (LONG)height;
    info.bmiHeader.biPlanes = 1u;
    info.bmiHeader.biBitCount = 24u;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = image_bytes;
    allocation = GlobalAlloc(GMEM_FIXED, image_bytes);
    if (allocation == 0) { native_error = GetLastError(); goto cleanup; }
    pixels = (unsigned char *)allocation;
    if (GetDIBits(screen, bitmap, 0u, (UINT)height, pixels, &info,
                  DIB_RGB_COLORS) != (int)height) {
        native_error = GetLastError(); goto cleanup;
    }
    v9x_screen_zero(bitmap_header, sizeof(bitmap_header));
    bitmap_header[0] = 'B'; bitmap_header[1] = 'M';
    v9x_write_u32(bitmap_header + 2, file_bytes);
    v9x_write_u32(bitmap_header + 10, 54ul);
    v9x_screen_zero(dib_header, sizeof(dib_header));
    v9x_write_u32(dib_header, 40ul);
    v9x_write_u32(dib_header + 4, width);
    v9x_write_u32(dib_header + 8, height);
    v9x_write_u16(dib_header + 12, 1u);
    v9x_write_u16(dib_header + 14, 24u);
    v9x_write_u32(dib_header + 20, image_bytes);
    file = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) { native_error = GetLastError(); goto cleanup; }
    if (!v9x_write_all(file, bitmap_header, sizeof(bitmap_header), &native_error) ||
        !v9x_write_all(file, dib_header, sizeof(dib_header), &native_error) ||
        !v9x_write_all(file, pixels, image_bytes, &native_error) ||
        !FlushFileBuffers(file)) {
        if (native_error == 0ul) native_error = GetLastError();
        goto cleanup;
    }
    crc = v9x_crc32_begin();
    crc = v9x_crc32_update(crc, bitmap_header, sizeof(bitmap_header));
    crc = v9x_crc32_update(crc, dib_header, sizeof(dib_header));
    crc = v9x_crc32_update(crc, pixels, image_bytes);
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
    if (old_object != 0 && memory != 0) (void)SelectObject(memory, old_object);
    if (bitmap != 0) (void)DeleteObject(bitmap);
    if (memory != 0) (void)DeleteDC(memory);
    if (screen != 0) (void)ReleaseDC(0, screen);
    if (!success) {
        return v9x_screen_error(state, request_id, V9X_STATUS_IO_FAILED,
                                native_error, "screenshot capture failed");
    }
    v9x_log_line("screenshot-complete");
    return v9x_send_frame(state, V9X_MSG_SCREENSHOT_RESPONSE, request_id,
                          v9x_screen_response, response_offset);
}
