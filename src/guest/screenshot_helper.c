#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define V9X_SCREEN_MAX_BYTES 16777216ul

static const char v9x_capture_path[] = "C:\\V9XREMOTE\\TEMP\\CAPTURE.$$$";
static const char v9x_metadata_path[] = "C:\\V9XREMOTE\\TEMP\\CAPTURE.DAT";

static void v9x_zero(void *target, unsigned long length)
{
    unsigned char *bytes = (unsigned char *)target;
    unsigned long index;
    for (index = 0ul; index < length; ++index) bytes[index] = 0u;
}

static void v9x_put_u32(unsigned char *target, DWORD value)
{
    target[0] = (unsigned char)(value & 0xfful);
    target[1] = (unsigned char)((value >> 8) & 0xfful);
    target[2] = (unsigned char)((value >> 16) & 0xfful);
    target[3] = (unsigned char)((value >> 24) & 0xfful);
}

static int v9x_write_all(HANDLE file, const unsigned char *data, DWORD length)
{
    DWORD offset = 0ul;
    DWORD written;
    while (offset < length) {
        if (!WriteFile(file, data + offset, length - offset, &written, 0) ||
            written == 0ul) return 0;
        offset += written;
    }
    return 1;
}

static DWORD v9x_capture(void)
{
    HDC screen = 0;
    HDC memory = 0;
    HBITMAP bitmap = 0;
    HGDIOBJ old_object = 0;
    BITMAPINFO info;
    unsigned char bitmap_header[14];
    unsigned char dib_header[40];
    unsigned char metadata[20];
    unsigned char *pixels = 0;
    HGLOBAL allocation = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD width;
    DWORD height;
    DWORD stable_width;
    DWORD stable_height;
    DWORD source_bpp;
    DWORD row_bytes;
    DWORD image_bytes;
    DWORD file_bytes;
    DWORD result = ERROR_GEN_FAILURE;

    (void)DeleteFileA(v9x_capture_path);
    (void)DeleteFileA(v9x_metadata_path);
    width = (DWORD)GetSystemMetrics(SM_CXSCREEN);
    height = (DWORD)GetSystemMetrics(SM_CYSCREEN);
    Sleep(250ul);
    stable_width = (DWORD)GetSystemMetrics(SM_CXSCREEN);
    stable_height = (DWORD)GetSystemMetrics(SM_CYSCREEN);
    if (width != stable_width || height != stable_height) return ERROR_RETRY;
    if (width == 0ul || height == 0ul || width > 4096ul || height > 4096ul ||
        width > (0xfffffffful - 3ul) / 3ul) return ERROR_INVALID_DATA;
    row_bytes = (width * 3ul + 3ul) & ~3ul;
    if (height > V9X_SCREEN_MAX_BYTES / row_bytes) return ERROR_NOT_ENOUGH_MEMORY;
    image_bytes = row_bytes * height;
    file_bytes = 54ul + image_bytes;

    screen = GetDC(0);
    if (screen == 0) { result = GetLastError(); goto cleanup; }
    source_bpp = (DWORD)(GetDeviceCaps(screen, BITSPIXEL) *
                         GetDeviceCaps(screen, PLANES));
    memory = CreateCompatibleDC(screen);
    if (memory == 0) { result = GetLastError(); goto cleanup; }
    bitmap = CreateCompatibleBitmap(screen, (int)width, (int)height);
    if (bitmap == 0) { result = GetLastError(); goto cleanup; }
    old_object = SelectObject(memory, bitmap);
    if (old_object == 0 || old_object == HGDI_ERROR) {
        result = GetLastError(); goto cleanup;
    }
    if (!BitBlt(memory, 0, 0, (int)width, (int)height,
                screen, 0, 0, SRCCOPY)) {
        result = GetLastError(); goto cleanup;
    }
    (void)SelectObject(memory, old_object);
    old_object = 0;
    v9x_zero(&info, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = (LONG)width;
    info.bmiHeader.biHeight = (LONG)height;
    info.bmiHeader.biPlanes = 1u;
    info.bmiHeader.biBitCount = 24u;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = image_bytes;
    allocation = GlobalAlloc(GMEM_FIXED, image_bytes);
    if (allocation == 0) { result = ERROR_NOT_ENOUGH_MEMORY; goto cleanup; }
    pixels = (unsigned char *)allocation;
    if (GetDIBits(screen, bitmap, 0u, (UINT)height, pixels, &info,
                  DIB_RGB_COLORS) != (int)height) {
        result = GetLastError(); goto cleanup;
    }

    v9x_zero(bitmap_header, sizeof(bitmap_header));
    bitmap_header[0] = 'B'; bitmap_header[1] = 'M';
    v9x_put_u32(bitmap_header + 2, file_bytes);
    v9x_put_u32(bitmap_header + 10, 54ul);
    v9x_zero(dib_header, sizeof(dib_header));
    v9x_put_u32(dib_header, 40ul);
    v9x_put_u32(dib_header + 4, width);
    v9x_put_u32(dib_header + 8, height);
    dib_header[12] = 1u;
    dib_header[14] = 24u;
    v9x_put_u32(dib_header + 20, image_bytes);
    file = CreateFileA(v9x_capture_path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) { result = GetLastError(); goto cleanup; }
    if (!v9x_write_all(file, bitmap_header, sizeof(bitmap_header)) ||
        !v9x_write_all(file, dib_header, sizeof(dib_header)) ||
        !v9x_write_all(file, pixels, image_bytes) || !FlushFileBuffers(file)) {
        result = GetLastError(); goto cleanup;
    }
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;

    v9x_put_u32(metadata, 0x31533956ul); /* V9S1 */
    v9x_put_u32(metadata + 4, width);
    v9x_put_u32(metadata + 8, height);
    v9x_put_u32(metadata + 12, source_bpp);
    v9x_put_u32(metadata + 16, file_bytes);
    file = CreateFileA(v9x_metadata_path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) { result = GetLastError(); goto cleanup; }
    if (!v9x_write_all(file, metadata, sizeof(metadata)) ||
        !FlushFileBuffers(file)) {
        result = GetLastError(); goto cleanup;
    }
    result = 0ul;

cleanup:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (allocation != 0) (void)GlobalFree(allocation);
    if (old_object != 0 && memory != 0) (void)SelectObject(memory, old_object);
    if (bitmap != 0) (void)DeleteObject(bitmap);
    if (memory != 0) (void)DeleteDC(memory);
    if (screen != 0) (void)ReleaseDC(0, screen);
    if (result != 0ul) {
        (void)DeleteFileA(v9x_capture_path);
        (void)DeleteFileA(v9x_metadata_path);
    }
    return result;
}

void WINAPI V9xScreenshotEntry(void)
{
    (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                       SEM_NOOPENFILEERRORBOX);
    ExitProcess(v9x_capture());
}
