/* V9XVNC.EXE: a minimal RFB (VNC) server for live remote control.

   A separate long-lived process, never part of the agent: every GDI call a
   broken display driver can hang lives here, and the agent stays reachable
   to stop or kill it. One viewer at a time, security type None (the same
   no-auth stance as the agent, contained by the network), RFB 3.3/3.7/3.8,
   Raw and Hextile encodings plus the DesktopSize pseudo-encoding.

   Capture polls the screen into a 32 bpp DIB section, diffs it against the
   previous frame in 16x16 tiles, and sends only the dirty tiles. Input goes
   through SetCursorPos / mouse_event / keybd_event, which every Win32
   version this project targets has.

   Off by default: it serves only when AGENT.INI has [vnc] enabled=1, which
   SETUP.EXE's VNC option writes (exit 9 otherwise).

   Usage: V9XVNC.EXE [-port n]   serve (port defaults to [vnc] port=5900)
          V9XVNC.EXE -stop       ask a running server to exit (1: none runs)
          V9XVNC.EXE -status     exit 0 when a server is running, 1 when not

   On Windows 2000 with the service installed, a plain start and -stop go
   through the SCM instead (start or stop the service; 0 ok, 1 failed). */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock.h>
#include "vncdes.h"
#include "vncz.h"
#include "vncsvc.h"

#define VNC_TILE 16
#define VNC_MAX_DIM 4096
#define VNC_MAX_TILES ((VNC_MAX_DIM / VNC_TILE) * (VNC_MAX_DIM / VNC_TILE))
#define VNC_OUT_SIZE 65536
#define VNC_IO_TIMEOUT_MS 30000ul
#define VNC_MAX_CUT_TEXT 1048576ul
#define VNC_LOG_LIMIT 65536ul
#define VNC_EXIT_DISABLED 9ul

#define VNC_ENC_RAW 0l
#define VNC_ENC_HEXTILE 5l
#define VNC_ENC_DESKTOP_SIZE (-223l)
#define VNC_ENC_RICH_CURSOR (-239l)
#define VNC_ENC_POINTER_POS (-232l)
#define VNC_CURSOR_MAX 64

#define VNC_HEX_RAW 1u
#define VNC_HEX_BG 2u
#define VNC_HEX_FG 4u
#define VNC_HEX_ANY 8u
#define VNC_HEX_COLOURED 16u

static const char vnc_config_path[] = "C:\\V9XREMOTE\\AGENT.INI";
static const char vnc_log_path[] = "C:\\V9XREMOTE\\VNC.LOG";
static const char vnc_stop_name[] = "V9XVNC.STOP";
static const char vnc_mutex_name[] = "V9XVNC.INSTANCE";
/* Exists only while a viewer is connected; the agent's tray icon turns
   yellow while it does (tray.c). */
static const char vnc_session_name[] = "V9XVNC.SESSION";
static const char vnc_stall_path[] = "C:\\V9XREMOTE\\TEMP\\VNCSTALL.DAT";

/* GetCursorInfo is Windows 98 and later; loaded at run time so the server
   still starts on Windows 95 (it then draws no cursor). Own struct name so
   the header's WINVER guards do not matter. */
typedef struct {
    DWORD size;
    DWORD flags;
    HCURSOR cursor;
    POINT position;
} VncCursorInfo;
typedef BOOL (WINAPI *VncGetCursorInfo)(VncCursorInfo *);

typedef struct {
    unsigned char bpp;
    unsigned char depth;
    unsigned char big_endian;
    unsigned char true_colour;
    unsigned int red_max;
    unsigned int green_max;
    unsigned int blue_max;
    unsigned char red_shift;
    unsigned char green_shift;
    unsigned char blue_shift;
} VncFormat;

/* Capture state. */
static HDC vnc_memory_dc;
static HBITMAP vnc_dib;
static HGDIOBJ vnc_old_bitmap;
static DWORD *vnc_bits;
static DWORD *vnc_prev;
static HGLOBAL vnc_prev_allocation;
static int vnc_width;
static int vnc_height;
static int vnc_tiles_x;
static int vnc_tiles_y;
/* The two large buffers are allocated at start: Watcom stores DGROUP's BSS
   in the image, and these alone would add 128 KiB of zeros to the EXE. */
static unsigned char *vnc_dirty;
/* Sampled capture state (see vnc_capture_sampled). */
static unsigned char *vnc_read_mask;
static unsigned char *vnc_suspect;
static volatile LONG vnc_capture_full = 1l;
static RECT vnc_cursor_area;          /* drawn-cursor region of last frame */
static int vnc_cursor_area_valid;
static VncGetCursorInfo vnc_get_cursor_info;

/* Connection state. */
static SOCKET vnc_socket = INVALID_SOCKET;
static HANDLE vnc_stop_event;
static VncDesktop vnc_worker_desktop;   /* capture thread's input desktop */
static VncDesktop vnc_input_desktop;    /* network thread's input desktop */
static VncFormat vnc_format;
static DWORD vnc_red_table[256];
static DWORD vnc_green_table[256];
static DWORD vnc_blue_table[256];
static unsigned int vnc_pixel_bytes;
static unsigned int vnc_cpixel_bytes;
static unsigned int vnc_cpixel_skip;
static int vnc_use_hextile;
static int vnc_use_desktop_size;
static int vnc_update_wanted;
static int vnc_full_wanted;
static unsigned char *vnc_out;
static unsigned long vnc_out_length;
static unsigned char vnc_in[1024];
static int vnc_hex_bg_valid;
static int vnc_hex_fg_valid;
static DWORD vnc_hex_bg;
static DWORD vnc_hex_fg;
static DWORD vnc_tile[VNC_TILE * VNC_TILE];
static unsigned char vnc_covered[VNC_TILE * VNC_TILE];
static unsigned char vnc_subrects[VNC_TILE * VNC_TILE * 6];

/* Input state: what this viewer is holding down, released on disconnect. */
static unsigned char vnc_keys_down[256];
static unsigned char vnc_keys_extended[256];
static unsigned char vnc_buttons;
static int vnc_pointer_x = -1;
static int vnc_pointer_y = -1;

/* ---------------------------------------------------------------- helpers */

static void vnc_zero(void *target, unsigned long length)
{
    unsigned char *bytes = (unsigned char *)target;
    while (length-- != 0ul) *bytes++ = 0u;
}

static void vnc_copy(void *target, const void *source, unsigned long length)
{
    unsigned char *to = (unsigned char *)target;
    const unsigned char *from = (const unsigned char *)source;
    while (length-- != 0ul) *to++ = *from++;
}

static unsigned int vnc_u16(const unsigned char *source)
{
    return ((unsigned int)source[0] << 8) | (unsigned int)source[1];
}

static DWORD vnc_u32(const unsigned char *source)
{
    return ((DWORD)source[0] << 24) | ((DWORD)source[1] << 16) |
           ((DWORD)source[2] << 8) | (DWORD)source[3];
}

static void vnc_put16(unsigned char *target, unsigned int value)
{
    target[0] = (unsigned char)((value >> 8) & 0xffu);
    target[1] = (unsigned char)(value & 0xffu);
}

static void vnc_put32(unsigned char *target, DWORD value)
{
    target[0] = (unsigned char)((value >> 24) & 0xfful);
    target[1] = (unsigned char)((value >> 16) & 0xfful);
    target[2] = (unsigned char)((value >> 8) & 0xfful);
    target[3] = (unsigned char)(value & 0xfful);
}

static unsigned long vnc_decimal(char *target, DWORD value)
{
    char digits[10];
    unsigned long count = 0ul;
    unsigned long index;
    do {
        digits[count++] = (char)('0' + (int)(value % 10ul));
        value /= 10ul;
    } while (value != 0ul);
    for (index = 0ul; index < count; ++index) {
        target[index] = digits[count - 1ul - index];
    }
    return count;
}

/* Append "HH:MM:SS event detail" to VNC.LOG; truncate it past 64 KiB. */
static void vnc_log(const char *event, DWORD value)
{
    char line[160];
    unsigned long length = 0ul;
    SYSTEMTIME now;
    HANDLE file;
    DWORD written;
    GetLocalTime(&now);
    line[length++] = (char)('0' + now.wHour / 10u);
    line[length++] = (char)('0' + now.wHour % 10u);
    line[length++] = ':';
    line[length++] = (char)('0' + now.wMinute / 10u);
    line[length++] = (char)('0' + now.wMinute % 10u);
    line[length++] = ':';
    line[length++] = (char)('0' + now.wSecond / 10u);
    line[length++] = (char)('0' + now.wSecond % 10u);
    line[length++] = ' ';
    while (*event != '\0' && length < 140ul) line[length++] = *event++;
    line[length++] = ' ';
    length += vnc_decimal(line + length, value);
    line[length++] = '\r';
    line[length++] = '\n';
    file = CreateFileA(vnc_log_path, GENERIC_WRITE, FILE_SHARE_READ, 0,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return;
    if (GetFileSize(file, 0) > VNC_LOG_LIMIT) {
        (void)SetFilePointer(file, 0l, 0, FILE_BEGIN);
        (void)SetEndOfFile(file);
    }
    (void)SetFilePointer(file, 0l, 0, FILE_END);
    (void)WriteFile(file, line, length, &written, 0);
    CloseHandle(file);
}

static int vnc_stop_requested(void)
{
    return WaitForSingleObject(vnc_stop_event, 0ul) == WAIT_OBJECT_0;
}

/* ---------------------------------------------------------------- network */

/* The listener, and the one address allowed to use it, for turning away a
   second viewer while a session is open. */
static SOCKET vnc_listener = INVALID_SOCKET;
static int vnc_restrict_client;
static unsigned long vnc_allowed_client;

/* During a session: bit 1 when the viewer sent something, bit 2 when
   another connection is waiting on the listener; 0 on timeout, -1 on error. */
static int vnc_wait_session(DWORD milliseconds)
{
    fd_set readable;
    struct timeval wait;
    int ready;
    int result = 0;
    FD_ZERO(&readable);
    FD_SET(vnc_socket, &readable);
    if (vnc_listener != INVALID_SOCKET) FD_SET(vnc_listener, &readable);
    wait.tv_sec = (long)(milliseconds / 1000ul);
    wait.tv_usec = (long)((milliseconds % 1000ul) * 1000ul);
    ready = select(0, &readable, 0, 0, &wait);
    if (ready <= 0) return ready;
    if (FD_ISSET(vnc_socket, &readable)) result |= 1;
    if (vnc_listener != INVALID_SOCKET && FD_ISSET(vnc_listener, &readable)) result |= 2;
    return result;
}

/* One viewer at a time: a second one gets RFB's connection-failed reply with
   a reason its user can read, instead of hanging in the listen backlog. */
static void vnc_turn_away(void)
{
    static const char reason[] = "Another VNC viewer is connected to this machine.";
    unsigned char reply[12 + 8 + sizeof(reason)];
    struct sockaddr_in address;
    int address_length = sizeof(address);
    SOCKET extra = accept(vnc_listener, (struct sockaddr *)&address, &address_length);
    if (extra == INVALID_SOCKET) return;
    if (!vnc_restrict_client || address.sin_addr.s_addr == vnc_allowed_client) {
        vnc_copy(reply, "RFB 003.003\n", 12ul);
        vnc_put32(reply + 12, 0ul);
        vnc_put32(reply + 16, (DWORD)(sizeof(reason) - 1u));
        vnc_copy(reply + 20, reason, sizeof(reason) - 1u);
        (void)send(extra, (const char *)reply, (int)(20u + sizeof(reason) - 1u), 0);
        vnc_log("viewer-busy", 0ul);
    } else {
        vnc_log("client-rejected", 0ul);
    }
    closesocket(extra);
}

static int vnc_wait_readable(DWORD milliseconds)
{
    fd_set readable;
    struct timeval wait;
    FD_ZERO(&readable);
    FD_SET(vnc_socket, &readable);
    wait.tv_sec = (long)(milliseconds / 1000ul);
    wait.tv_usec = (long)((milliseconds % 1000ul) * 1000ul);
    return select(0, &readable, 0, 0, &wait);
}

static int vnc_recv_all(unsigned char *target, unsigned long length)
{
    int received;
    while (length != 0ul) {
        if (vnc_wait_readable(VNC_IO_TIMEOUT_MS) <= 0) return 0;
        received = recv(vnc_socket, (char *)target, (int)length, 0);
        if (received <= 0) return 0;
        target += received;
        length -= (unsigned long)received;
    }
    return 1;
}

static int vnc_skip(unsigned long length)
{
    unsigned long part;
    while (length != 0ul) {
        part = length > sizeof(vnc_in) ? sizeof(vnc_in) : length;
        if (!vnc_recv_all(vnc_in, part)) return 0;
        length -= part;
    }
    return 1;
}

/* Performance counters, logged every 60 s of a session and at its end (perf-* lines in
   VNC.LOG). Times are QueryPerformanceCounter ticks, low 32 bits only:
   intervals are short and a 64-bit division would need a runtime helper. */
static DWORD vnc_perf_frequency_ms;  /* ticks per millisecond, 0 = no counter */
static DWORD vnc_perf_capture;       /* worker: BitBlt and cursor */
static DWORD vnc_perf_diff;
static DWORD vnc_perf_encode;        /* building updates, sends excluded */
static DWORD vnc_perf_send;          /* inside send() */
static DWORD vnc_perf_captures;
static DWORD vnc_perf_updates;
static DWORD vnc_perf_bytes;
static DWORD vnc_perf_logged;

static DWORD vnc_ticks(void)
{
    LARGE_INTEGER now;
    if (vnc_perf_frequency_ms == 0ul || !QueryPerformanceCounter(&now)) return 0ul;
    return now.LowPart;
}

static DWORD vnc_ticks_ms(DWORD ticks)
{
    return vnc_perf_frequency_ms == 0ul ? 0ul : ticks / vnc_perf_frequency_ms;
}

static void vnc_perf_reset(void)
{
    vnc_perf_capture = 0ul;
    vnc_perf_diff = 0ul;
    vnc_perf_encode = 0ul;
    vnc_perf_send = 0ul;
    vnc_perf_captures = 0ul;
    vnc_perf_updates = 0ul;
    vnc_perf_bytes = 0ul;
    vnc_perf_logged = GetTickCount();
}

static void vnc_perf_report(void)
{
    if (vnc_perf_captures == 0ul && vnc_perf_updates == 0ul) {
        vnc_perf_reset();
        return;
    }
    vnc_log("perf-seconds", (GetTickCount() - vnc_perf_logged) / 1000ul);
    vnc_log("perf-captures", vnc_perf_captures);
    vnc_log("perf-capture-ms", vnc_ticks_ms(vnc_perf_capture));
    vnc_log("perf-diff-ms", vnc_ticks_ms(vnc_perf_diff));
    vnc_log("perf-updates", vnc_perf_updates);
    vnc_log("perf-encode-ms", vnc_ticks_ms(vnc_perf_encode));
    vnc_log("perf-send-ms", vnc_ticks_ms(vnc_perf_send));
    vnc_log("perf-kbytes", vnc_perf_bytes / 1024ul);
    vnc_perf_reset();
}

static int vnc_send_all(const unsigned char *data, unsigned long length)
{
    int sent;
    DWORD started = vnc_ticks();
    vnc_perf_bytes += length;
    while (length != 0ul) {
        sent = send(vnc_socket, (const char *)data, (int)length, 0);
        if (sent <= 0) return 0;
        data += sent;
        length -= (unsigned long)sent;
    }
    vnc_perf_send += vnc_ticks() - started;
    return 1;
}

static int vnc_flush(void)
{
    int ok = vnc_send_all(vnc_out, vnc_out_length);
    vnc_out_length = 0ul;
    return ok;
}

/* Make room for `length` more output bytes; returns the write position or 0
   when the flush failed. */
static unsigned char *vnc_reserve(unsigned long length)
{
    unsigned char *at;
    if (vnc_out_length + length > VNC_OUT_SIZE && !vnc_flush()) return 0;
    at = vnc_out + vnc_out_length;
    vnc_out_length += length;
    return at;
}

/* ------------------------------------------------------------ pixel format */

static void vnc_build_tables(void)
{
    unsigned int value;
    for (value = 0u; value < 256u; ++value) {
        vnc_red_table[value] =
            (DWORD)((value * vnc_format.red_max + 127u) / 255u) << vnc_format.red_shift;
        vnc_green_table[value] =
            (DWORD)((value * vnc_format.green_max + 127u) / 255u) << vnc_format.green_shift;
        vnc_blue_table[value] =
            (DWORD)((value * vnc_format.blue_max + 127u) / 255u) << vnc_format.blue_shift;
    }
    vnc_pixel_bytes = (unsigned int)vnc_format.bpp / 8u;
    /* ZRLE's CPIXEL: three bytes when a 32 bpp true-colour pixel's colour
       bits all sit in its low or its high three bytes, else the pixel. */
    vnc_cpixel_bytes = vnc_pixel_bytes;
    vnc_cpixel_skip = 0u;
    if (vnc_format.true_colour && vnc_format.bpp == 32u && vnc_format.depth <= 24u) {
        DWORD used = ((DWORD)vnc_format.red_max << vnc_format.red_shift) |
                     ((DWORD)vnc_format.green_max << vnc_format.green_shift) |
                     ((DWORD)vnc_format.blue_max << vnc_format.blue_shift);
        if ((used & 0xff000000ul) == 0ul) {
            vnc_cpixel_bytes = 3u;
            vnc_cpixel_skip = vnc_format.big_endian ? 1u : 0u;
        } else if ((used & 0xfful) == 0ul) {
            vnc_cpixel_bytes = 3u;
            vnc_cpixel_skip = vnc_format.big_endian ? 0u : 1u;
        }
    }
}

static void vnc_default_format(void)
{
    vnc_format.bpp = 32u;
    vnc_format.depth = 24u;
    vnc_format.big_endian = 0u;
    vnc_format.true_colour = 1u;
    vnc_format.red_max = 255u;
    vnc_format.green_max = 255u;
    vnc_format.blue_max = 255u;
    vnc_format.red_shift = 16u;
    vnc_format.green_shift = 8u;
    vnc_format.blue_shift = 0u;
    vnc_build_tables();
}

static void vnc_write_format(unsigned char *target)
{
    target[0] = vnc_format.bpp;
    target[1] = vnc_format.depth;
    target[2] = vnc_format.big_endian;
    target[3] = vnc_format.true_colour;
    vnc_put16(target + 4, vnc_format.red_max);
    vnc_put16(target + 6, vnc_format.green_max);
    vnc_put16(target + 8, vnc_format.blue_max);
    target[10] = vnc_format.red_shift;
    target[11] = vnc_format.green_shift;
    target[12] = vnc_format.blue_shift;
    target[13] = 0u;
    target[14] = 0u;
    target[15] = 0u;
}

static DWORD vnc_translate(DWORD native)
{
    return vnc_red_table[(native >> 16) & 0xfful] |
           vnc_green_table[(native >> 8) & 0xfful] |
           vnc_blue_table[native & 0xfful];
}

static void vnc_store_pixel(unsigned char *target, DWORD pixel)
{
    if (vnc_pixel_bytes == 1u) {
        target[0] = (unsigned char)pixel;
    } else if (vnc_pixel_bytes == 2u) {
        if (vnc_format.big_endian) {
            target[0] = (unsigned char)(pixel >> 8);
            target[1] = (unsigned char)pixel;
        } else {
            target[0] = (unsigned char)pixel;
            target[1] = (unsigned char)(pixel >> 8);
        }
    } else if (vnc_format.big_endian) {
        vnc_put32(target, pixel);
    } else {
        target[0] = (unsigned char)pixel;
        target[1] = (unsigned char)(pixel >> 8);
        target[2] = (unsigned char)(pixel >> 16);
        target[3] = (unsigned char)(pixel >> 24);
    }
}

/* A colour-map client gets a fixed 3-3-2 palette: index = RRRGGGBB, which is
   exactly an 8 bpp true-colour format, so translation needs no special case. */
static int vnc_send_colour_map(void)
{
    unsigned char *at;
    unsigned int index;
    at = vnc_reserve(6ul + 256ul * 6ul);
    if (at == 0) return 0;
    at[0] = 1u;
    at[1] = 0u;
    vnc_put16(at + 2, 0u);
    vnc_put16(at + 4, 256u);
    at += 6;
    for (index = 0u; index < 256u; ++index) {
        vnc_put16(at, (unsigned int)(((index >> 5) & 7u) * 65535ul / 7ul));
        vnc_put16(at + 2, (unsigned int)(((index >> 2) & 7u) * 65535ul / 7ul));
        vnc_put16(at + 4, (unsigned int)((index & 3u) * 65535ul / 3ul));
        at += 6;
    }
    return vnc_flush();
}

static int vnc_shift_ok(unsigned int max, unsigned int shift, unsigned int bpp)
{
    unsigned int bits = 0u;
    while ((max >> bits) != 0u) ++bits;
    return max != 0u && shift + bits <= bpp;
}

static int vnc_set_pixel_format(const unsigned char *format)
{
    VncFormat wanted;
    wanted.bpp = format[0];
    wanted.depth = format[1];
    wanted.big_endian = (unsigned char)(format[2] != 0u);
    wanted.true_colour = (unsigned char)(format[3] != 0u);
    wanted.red_max = vnc_u16(format + 4);
    wanted.green_max = vnc_u16(format + 6);
    wanted.blue_max = vnc_u16(format + 8);
    wanted.red_shift = format[10];
    wanted.green_shift = format[11];
    wanted.blue_shift = format[12];
    if (wanted.bpp != 8u && wanted.bpp != 16u && wanted.bpp != 32u) return 0;
    if (!wanted.true_colour) {
        if (wanted.bpp != 8u) return 0;
        wanted.red_max = 7u;
        wanted.green_max = 7u;
        wanted.blue_max = 3u;
        wanted.red_shift = 5u;
        wanted.green_shift = 2u;
        wanted.blue_shift = 0u;
    } else if (!vnc_shift_ok(wanted.red_max, wanted.red_shift, wanted.bpp) ||
               !vnc_shift_ok(wanted.green_max, wanted.green_shift, wanted.bpp) ||
               !vnc_shift_ok(wanted.blue_max, wanted.blue_shift, wanted.bpp)) {
        return 0;
    }
    vnc_format = wanted;
    vnc_build_tables();
    vnc_full_wanted = 1;
    vnc_hex_bg_valid = 0;
    vnc_hex_fg_valid = 0;
    vnc_log("pixel-format", (DWORD)wanted.bpp);
    if (!wanted.true_colour) return vnc_send_colour_map();
    return 1;
}

/* ---------------------------------------------------------------- capture */

static void vnc_cursor_free(void);

static void vnc_capture_free(void)
{
    vnc_cursor_free();
    if (vnc_memory_dc != 0 && vnc_old_bitmap != 0) {
        (void)SelectObject(vnc_memory_dc, vnc_old_bitmap);
    }
    if (vnc_dib != 0) (void)DeleteObject(vnc_dib);
    if (vnc_memory_dc != 0) (void)DeleteDC(vnc_memory_dc);
    if (vnc_prev_allocation != 0) (void)GlobalFree(vnc_prev_allocation);
    vnc_memory_dc = 0;
    vnc_dib = 0;
    vnc_old_bitmap = 0;
    vnc_bits = 0;
    vnc_prev = 0;
    vnc_prev_allocation = 0;
    vnc_width = 0;
    vnc_height = 0;
}

/* (Re)create the DIB section for the current screen size. */
static int vnc_capture_init(void)
{
    BITMAPINFO info;
    HDC screen;
    void *bits = 0;
    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);
    vnc_capture_free();
    if (width <= 0 || height <= 0 || width > VNC_MAX_DIM || height > VNC_MAX_DIM) {
        return 0;
    }
    screen = GetDC(0);
    if (screen == 0) return 0;
    vnc_memory_dc = CreateCompatibleDC(screen);
    vnc_zero(&info, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = (LONG)width;
    info.bmiHeader.biHeight = -(LONG)height;
    info.bmiHeader.biPlanes = 1u;
    info.bmiHeader.biBitCount = 32u;
    info.bmiHeader.biCompression = BI_RGB;
    if (vnc_memory_dc != 0) {
        vnc_dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, 0, 0ul);
    }
    (void)ReleaseDC(0, screen);
    if (vnc_dib == 0 || bits == 0) {
        vnc_capture_free();
        return 0;
    }
    vnc_old_bitmap = SelectObject(vnc_memory_dc, vnc_dib);
    vnc_prev_allocation = GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT,
                                      (DWORD)width * (DWORD)height * 4ul);
    if (vnc_old_bitmap == 0 || vnc_old_bitmap == HGDI_ERROR ||
        vnc_prev_allocation == 0) {
        vnc_capture_free();
        return 0;
    }
    vnc_bits = (DWORD *)bits;
    vnc_prev = (DWORD *)vnc_prev_allocation;
    vnc_width = width;
    vnc_height = height;
    vnc_tiles_x = (width + VNC_TILE - 1) / VNC_TILE;
    vnc_tiles_y = (height + VNC_TILE - 1) / VNC_TILE;
    vnc_log("capture-size", (DWORD)width * 10000ul + (DWORD)height);
    {
        HDC probe = GetDC(0);
        if (probe != 0) {
            vnc_log("screen-bpp", (DWORD)(GetDeviceCaps(probe, BITSPIXEL) *
                                          GetDeviceCaps(probe, PLANES)));
            (void)ReleaseDC(0, probe);
        }
    }
    /* New, empty DIB: the next capture reads everything, and the diff
       compares everything; no tile of the old size is "dirty" any more. */
    vnc_capture_full = 1l;
    vnc_cursor_area_valid = 0;
    vnc_zero(vnc_dirty, VNC_MAX_TILES);
    return 1;
}

/* Cursor shape for viewers that take the RichCursor pseudo-encoding: they
   draw the cursor themselves, so it is left out of the frame (moving it then
   dirties no tiles). Written by the worker during a capture, read by the
   network thread once that capture is done. */
static volatile LONG vnc_rich_cursor;    /* set by the network thread */
static volatile LONG vnc_cursor_reset;   /* network asks for a fresh shape */
static HCURSOR vnc_cursor_seen;
static int vnc_cursor_shown;
static unsigned long vnc_cursor_serial;  /* bumps when the shape changes */
static int vnc_cursor_w;
static int vnc_cursor_h;
static int vnc_cursor_hot_x;
static int vnc_cursor_hot_y;
/* Allocated at start, like vnc_out (Watcom keeps BSS in the image). */
static DWORD *vnc_cursor_rgb;
static unsigned char *vnc_cursor_mask;
static DWORD *vnc_cursor_black;
static POINT vnc_cursor_at;
static int vnc_cursor_at_valid;
static HDC vnc_cursor_dc;
static HBITMAP vnc_cursor_dib;
static HGDIOBJ vnc_cursor_old;
static DWORD *vnc_cursor_bits;

static void vnc_cursor_free(void)
{
    if (vnc_cursor_dc != 0 && vnc_cursor_old != 0) {
        (void)SelectObject(vnc_cursor_dc, vnc_cursor_old);
    }
    if (vnc_cursor_dib != 0) (void)DeleteObject(vnc_cursor_dib);
    if (vnc_cursor_dc != 0) (void)DeleteDC(vnc_cursor_dc);
    vnc_cursor_dc = 0;
    vnc_cursor_dib = 0;
    vnc_cursor_old = 0;
    vnc_cursor_bits = 0;
}

static int vnc_cursor_surface(void)
{
    BITMAPINFO info;
    void *bits = 0;
    if (vnc_cursor_bits != 0) return 1;
    vnc_cursor_dc = CreateCompatibleDC(vnc_memory_dc);
    if (vnc_cursor_dc == 0) return 0;
    vnc_zero(&info, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = VNC_CURSOR_MAX;
    info.bmiHeader.biHeight = -VNC_CURSOR_MAX;
    info.bmiHeader.biPlanes = 1u;
    info.bmiHeader.biBitCount = 32u;
    info.bmiHeader.biCompression = BI_RGB;
    vnc_cursor_dib = CreateDIBSection(vnc_cursor_dc, &info, DIB_RGB_COLORS, &bits, 0, 0ul);
    if (vnc_cursor_dib == 0 || bits == 0) { vnc_cursor_free(); return 0; }
    vnc_cursor_old = SelectObject(vnc_cursor_dc, vnc_cursor_dib);
    vnc_cursor_bits = (DWORD *)bits;
    return 1;
}

/* Render the cursor over black and over white: equal results are opaque
   pixels, black/white is transparent, and an inverting (XOR) pixel shows as
   opaque black, as most VNC servers do. */
static void vnc_grab_cursor(void)
{
    VncCursorInfo cursor;
    ICONINFO icon;
    int width;
    int height;
    int index;
    int shown;
    if (vnc_get_cursor_info == 0) return;
    cursor.size = sizeof(cursor);
    if (!vnc_get_cursor_info(&cursor)) return;
    vnc_cursor_at = cursor.position;
    vnc_cursor_at_valid = 1;
    shown = (cursor.flags & 1ul) != 0ul && cursor.cursor != 0;
    if (!InterlockedExchange((LPLONG)&vnc_cursor_reset, 0l) &&
        shown == vnc_cursor_shown && (!shown || cursor.cursor == vnc_cursor_seen)) {
        return;
    }
    vnc_cursor_seen = cursor.cursor;
    vnc_cursor_shown = shown;
    ++vnc_cursor_serial;
    if (!shown || !vnc_cursor_surface()) {
        vnc_cursor_w = 1;
        vnc_cursor_h = 1;
        vnc_cursor_hot_x = 0;
        vnc_cursor_hot_y = 0;
        vnc_cursor_rgb[0] = 0ul;
        vnc_cursor_mask[0] = 0u;
        return;
    }
    width = GetSystemMetrics(SM_CXCURSOR);
    height = GetSystemMetrics(SM_CYCURSOR);
    if (width <= 0 || width > VNC_CURSOR_MAX) width = 32;
    if (height <= 0 || height > VNC_CURSOR_MAX) height = 32;
    vnc_zero(&icon, sizeof(icon));
    if (GetIconInfo((HICON)cursor.cursor, &icon)) {
        if (icon.hbmMask != 0) (void)DeleteObject(icon.hbmMask);
        if (icon.hbmColor != 0) (void)DeleteObject(icon.hbmColor);
    }
    vnc_cursor_hot_x = (int)icon.xHotspot < width ? (int)icon.xHotspot : 0;
    vnc_cursor_hot_y = (int)icon.yHotspot < height ? (int)icon.yHotspot : 0;
    (void)PatBlt(vnc_cursor_dc, 0, 0, VNC_CURSOR_MAX, VNC_CURSOR_MAX, BLACKNESS);
    (void)DrawIconEx(vnc_cursor_dc, 0, 0, (HICON)cursor.cursor, 0, 0, 0u, 0, DI_NORMAL);
    (void)GdiFlush();
    for (index = 0; index < height; ++index) {
        vnc_copy(vnc_cursor_black + index * width,
                 vnc_cursor_bits + index * VNC_CURSOR_MAX, (unsigned long)width * 4ul);
    }
    (void)PatBlt(vnc_cursor_dc, 0, 0, VNC_CURSOR_MAX, VNC_CURSOR_MAX, WHITENESS);
    (void)DrawIconEx(vnc_cursor_dc, 0, 0, (HICON)cursor.cursor, 0, 0, 0u, 0, DI_NORMAL);
    (void)GdiFlush();
    for (index = 0; index < width * height; ++index) {
        DWORD on_black = vnc_cursor_black[index] & 0xfffffful;
        DWORD on_white = vnc_cursor_bits[(index / width) * VNC_CURSOR_MAX + index % width] &
                         0xfffffful;
        if (on_black == on_white) {
            vnc_cursor_rgb[index] = on_black;
            vnc_cursor_mask[index] = 1u;
        } else if (on_black == 0ul && on_white == 0xfffffful) {
            vnc_cursor_rgb[index] = 0ul;
            vnc_cursor_mask[index] = 0u;
        } else {
            vnc_cursor_rgb[index] = 0ul;
            vnc_cursor_mask[index] = 1u;
        }
    }
    vnc_cursor_w = width;
    vnc_cursor_h = height;
}

static void vnc_draw_cursor(void)
{
    VncCursorInfo cursor;
    ICONINFO icon;
    if (vnc_get_cursor_info == 0) return;
    cursor.size = sizeof(cursor);
    if (!vnc_get_cursor_info(&cursor) || (cursor.flags & 1ul) == 0ul ||
        cursor.cursor == 0) return;
    vnc_zero(&icon, sizeof(icon));
    if (!GetIconInfo((HICON)cursor.cursor, &icon)) return;
    (void)DrawIconEx(vnc_memory_dc,
                     (int)(cursor.position.x - (LONG)icon.xHotspot),
                     (int)(cursor.position.y - (LONG)icon.yHotspot),
                     (HICON)cursor.cursor, 0, 0, 0u, 0, DI_NORMAL);
    if (icon.hbmMask != 0) (void)DeleteObject(icon.hbmMask);
    if (icon.hbmColor != 0) (void)DeleteObject(icon.hbmColor);
}

/* Grab the screen. Returns 1, 0 on failure, or -1 when the mode changed
   size (the caller resizes). */
/* Sampled capture. Reading the screen back is by far the most expensive
   step (on an emulated S3 ViRGE a full 1024x768 BitBlt took ~780 ms), so a
   poll reads one pixel row per 16-row band of tiles, 1/16 of the screen,
   and reads in full only the tiles whose sampled row changed, the tiles
   that changed last frame (motion tends to continue), and the area around
   the cursor when it is drawn into the frame. The sampled row rotates
   through all 16 rows of a band in bit-reversed order, so a change that no
   sample touched is still found within 16 polls. A full read happens when
   the viewer asks for a full frame and after (re)initialising.
   vnc_read_mask accumulates the tiles read until the next diff, which
   compares only those (unread tiles hold what vnc_prev holds). */
static unsigned int vnc_sample_step;
static const unsigned char vnc_sample_order[16] = {
    0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15
};

static void vnc_suspect_rect(LONG left, LONG top, LONG right, LONG bottom)
{
    int tile_x;
    int tile_y;
    if (left < 0l) left = 0l;
    if (top < 0l) top = 0l;
    if (right > (LONG)vnc_width) right = (LONG)vnc_width;
    if (bottom > (LONG)vnc_height) bottom = (LONG)vnc_height;
    if (left >= right || top >= bottom) return;
    for (tile_y = (int)top / VNC_TILE; tile_y <= (int)(bottom - 1l) / VNC_TILE; ++tile_y) {
        for (tile_x = (int)left / VNC_TILE; tile_x <= (int)(right - 1l) / VNC_TILE; ++tile_x) {
            vnc_suspect[tile_y * vnc_tiles_x + tile_x] = 1u;
        }
    }
}

static int vnc_capture_sampled(HDC screen)
{
    int band;
    int tile_x;
    int phase = (int)vnc_sample_order[vnc_sample_step++ & 15u];
    VncCursorInfo cursor;
    vnc_zero(vnc_suspect, (unsigned long)vnc_tiles_x * (unsigned long)vnc_tiles_y);
    for (band = 0; band < vnc_tiles_y; ++band) {
        int row = band * VNC_TILE + phase;
        if (row >= vnc_height) row = vnc_height - 1;
        if (!BitBlt(vnc_memory_dc, 0, row, vnc_width, 1, screen, 0, row, SRCCOPY)) return 0;
    }
    (void)GdiFlush();
    for (band = 0; band < vnc_tiles_y; ++band) {
        int row = band * VNC_TILE + phase;
        const DWORD *now;
        const DWORD *before;
        if (row >= vnc_height) row = vnc_height - 1;
        now = vnc_bits + (long)row * vnc_width;
        before = vnc_prev + (long)row * vnc_width;
        for (tile_x = 0; tile_x < vnc_tiles_x; ++tile_x) {
            int x0 = tile_x * VNC_TILE;
            int x1 = x0 + VNC_TILE > vnc_width ? vnc_width : x0 + VNC_TILE;
            int index = band * vnc_tiles_x + tile_x;
            int x;
            if (vnc_dirty[index]) { vnc_suspect[index] = 1u; continue; }
            for (x = x0; x < x1; ++x) {
                if (now[x] != before[x]) { vnc_suspect[index] = 1u; break; }
            }
        }
    }
    /* A cursor drawn into the frame: re-read where it was (to erase it) and
       where it is now; 32 pixels either side covers any hotspot. */
    if (!vnc_rich_cursor) {
        if (vnc_cursor_area_valid) {
            vnc_suspect_rect(vnc_cursor_area.left, vnc_cursor_area.top,
                             vnc_cursor_area.right, vnc_cursor_area.bottom);
        }
        cursor.size = sizeof(cursor);
        if (vnc_get_cursor_info != 0 && vnc_get_cursor_info(&cursor)) {
            vnc_cursor_area.left = cursor.position.x - 32l;
            vnc_cursor_area.top = cursor.position.y - 32l;
            vnc_cursor_area.right = cursor.position.x + 32l;
            vnc_cursor_area.bottom = cursor.position.y + 32l;
            vnc_cursor_area_valid = 1;
            vnc_suspect_rect(vnc_cursor_area.left, vnc_cursor_area.top,
                             vnc_cursor_area.right, vnc_cursor_area.bottom);
        }
    }
    /* Read each horizontal run of suspect tiles with one BitBlt. */
    for (band = 0; band < vnc_tiles_y; ++band) {
        int y0 = band * VNC_TILE;
        int height = y0 + VNC_TILE > vnc_height ? vnc_height - y0 : VNC_TILE;
        int run;
        for (tile_x = 0; tile_x < vnc_tiles_x; tile_x += run) {
            int x0;
            int width;
            int index;
            run = 1;
            if (!vnc_suspect[band * vnc_tiles_x + tile_x]) continue;
            while (tile_x + run < vnc_tiles_x &&
                   vnc_suspect[band * vnc_tiles_x + tile_x + run]) ++run;
            x0 = tile_x * VNC_TILE;
            width = (tile_x + run) * VNC_TILE > vnc_width ? vnc_width - x0 : run * VNC_TILE;
            if (!BitBlt(vnc_memory_dc, x0, y0, width, height, screen, x0, y0, SRCCOPY)) {
                return 0;
            }
            for (index = 0; index < run; ++index) {
                vnc_read_mask[band * vnc_tiles_x + tile_x + index] = 1u;
            }
        }
    }
    return 1;
}

static int vnc_capture(void)
{
    HDC screen;
    BOOL copied = TRUE;
    if (GetSystemMetrics(SM_CXSCREEN) != vnc_width ||
        GetSystemMetrics(SM_CYSCREEN) != vnc_height) return -1;
    screen = GetDC(0);
    if (screen == 0) return 0;
    if (InterlockedExchange((LPLONG)&vnc_capture_full, 0l)) {
        copied = BitBlt(vnc_memory_dc, 0, 0, vnc_width, vnc_height, screen, 0, 0,
                        SRCCOPY);
        if (copied) {
            int index;
            for (index = 0; index < vnc_tiles_x * vnc_tiles_y; ++index) vnc_read_mask[index] = 1u;
        }
    } else {
        copied = vnc_capture_sampled(screen);
    }
    (void)ReleaseDC(0, screen);
    if (!copied) {
        vnc_capture_full = 1l;
        return 0;
    }
    if (vnc_rich_cursor) {
        vnc_cursor_area_valid = 0;
        vnc_grab_cursor();
    } else {
        vnc_draw_cursor();
    }
    (void)GdiFlush();
    return 1;
}

/* Compare the capture against the previous frame tile by tile, copying each
   changed tile into the previous frame. Returns the dirty tile count. */
static unsigned long vnc_diff(int all)
{
    int tile_x;
    int tile_y;
    int x0;
    int y0;
    int width;
    int height;
    int row;
    int column;
    unsigned long dirty = 0ul;
    for (tile_y = 0; tile_y < vnc_tiles_y; ++tile_y) {
        y0 = tile_y * VNC_TILE;
        height = vnc_height - y0 < VNC_TILE ? vnc_height - y0 : VNC_TILE;
        for (tile_x = 0; tile_x < vnc_tiles_x; ++tile_x) {
            unsigned char changed = (unsigned char)(all != 0);
            int index = tile_y * vnc_tiles_x + tile_x;
            /* Unread tiles still hold what vnc_prev holds: skip them. */
            if (!all && !vnc_read_mask[index]) {
                vnc_dirty[index] = 0u;
                continue;
            }
            vnc_read_mask[index] = 0u;
            x0 = tile_x * VNC_TILE;
            width = vnc_width - x0 < VNC_TILE ? vnc_width - x0 : VNC_TILE;
            for (row = 0; row < height && !changed; ++row) {
                const DWORD *now = vnc_bits + (long)(y0 + row) * vnc_width + x0;
                const DWORD *before = vnc_prev + (long)(y0 + row) * vnc_width + x0;
                for (column = 0; column < width; ++column) {
                    if (now[column] != before[column]) { changed = 1u; break; }
                }
            }
            if (changed) {
                for (row = 0; row < height; ++row) {
                    long offset = (long)(y0 + row) * vnc_width + x0;
                    vnc_copy(vnc_prev + offset, vnc_bits + offset,
                             (unsigned long)width * 4ul);
                }
                ++dirty;
            }
            vnc_dirty[tile_y * vnc_tiles_x + tile_x] = changed;
        }
    }
    return dirty;
}

/* --------------------------------------------------------------- encoding */

static int vnc_rect_header(int x, int y, int width, int height, long encoding)
{
    unsigned char *at = vnc_reserve(12ul);
    if (at == 0) return 0;
    vnc_put16(at, (unsigned int)x);
    vnc_put16(at + 2, (unsigned int)y);
    vnc_put16(at + 4, (unsigned int)width);
    vnc_put16(at + 6, (unsigned int)height);
    vnc_put32(at + 8, (DWORD)encoding);
    return 1;
}

static int vnc_encode_raw(int x, int y, int width, int height)
{
    int row;
    int column;
    unsigned char *at;
    for (row = 0; row < height; ++row) {
        const DWORD *source = vnc_prev + (long)(y + row) * vnc_width + x;
        at = vnc_reserve((unsigned long)width * vnc_pixel_bytes);
        if (at == 0) return 0;
        for (column = 0; column < width; ++column) {
            vnc_store_pixel(at, vnc_translate(source[column]));
            at += vnc_pixel_bytes;
        }
    }
    return 1;
}

/* Cover the tile's non-background pixels with solid rectangles, greedily
   taking the larger of the row-first and column-first growth at each start
   pixel. Returns the subrect count, or -1 once encoding would not beat Raw. */
static int vnc_hextile_subrects(int width, int height, DWORD bg, int mono,
                                unsigned long *bytes)
{
    unsigned long limit = (unsigned long)width * (unsigned long)height *
                          vnc_pixel_bytes;
    unsigned long per = mono ? 2ul : 2ul + vnc_pixel_bytes;
    unsigned long used = 0ul;
    int count = 0;
    int x;
    int y;
    int i;
    int j;
    vnc_zero(vnc_covered, sizeof(vnc_covered));
    for (y = 0; y < height; ++y) {
        for (x = 0; x < width; ++x) {
            DWORD colour = vnc_tile[y * width + x];
            int run_w;
            int run_h;
            int alt_w;
            int alt_h;
            int ok;
            if (colour == bg || vnc_covered[y * width + x]) continue;
            for (run_w = 1; x + run_w < width; ++run_w) {
                if (vnc_tile[y * width + x + run_w] != colour ||
                    vnc_covered[y * width + x + run_w]) break;
            }
            for (run_h = 1; y + run_h < height; ++run_h) {
                ok = 1;
                for (i = 0; i < run_w; ++i) {
                    if (vnc_tile[(y + run_h) * width + x + i] != colour ||
                        vnc_covered[(y + run_h) * width + x + i]) { ok = 0; break; }
                }
                if (!ok) break;
            }
            for (alt_h = 1; y + alt_h < height; ++alt_h) {
                if (vnc_tile[(y + alt_h) * width + x] != colour ||
                    vnc_covered[(y + alt_h) * width + x]) break;
            }
            for (alt_w = 1; x + alt_w < width; ++alt_w) {
                ok = 1;
                for (j = 0; j < alt_h; ++j) {
                    if (vnc_tile[(y + j) * width + x + alt_w] != colour ||
                        vnc_covered[(y + j) * width + x + alt_w]) { ok = 0; break; }
                }
                if (!ok) break;
            }
            if (alt_w * alt_h > run_w * run_h) {
                run_w = alt_w;
                run_h = alt_h;
            }
            for (j = 0; j < run_h; ++j) {
                for (i = 0; i < run_w; ++i) {
                    vnc_covered[(y + j) * width + x + i] = 1u;
                }
            }
            used += per;
            if (count == 255 || used >= limit) return -1;
            if (!mono) {
                vnc_store_pixel(vnc_subrects + used - per, colour);
            }
            vnc_subrects[used - 2ul] = (unsigned char)((x << 4) | y);
            vnc_subrects[used - 1ul] =
                (unsigned char)(((run_w - 1) << 4) | (run_h - 1));
            ++count;
        }
    }
    *bytes = used;
    return count;
}

static int vnc_encode_hextile_tile(int x0, int y0, int width, int height)
{
    DWORD first;
    DWORD second = 0ul;
    unsigned long first_count = 0ul;
    unsigned long second_count = 0ul;
    int colours = 1;
    int count;
    int mono;
    int index;
    int row;
    int column;
    unsigned long bytes = 0ul;
    unsigned char flags;
    unsigned char *at;
    DWORD bg;
    DWORD fg = 0ul;

    for (row = 0; row < height; ++row) {
        const DWORD *source = vnc_prev + (long)(y0 + row) * vnc_width + x0;
        for (column = 0; column < width; ++column) {
            vnc_tile[row * width + column] = vnc_translate(source[column]);
        }
    }
    first = vnc_tile[0];
    for (index = 0; index < width * height; ++index) {
        DWORD pixel = vnc_tile[index];
        if (pixel == first) {
            ++first_count;
        } else if (colours == 1) {
            second = pixel;
            second_count = 1ul;
            colours = 2;
        } else if (pixel == second) {
            ++second_count;
        } else {
            colours = 3;
        }
    }
    bg = (colours >= 2 && second_count > first_count) ? second : first;
    flags = 0u;
    if (!vnc_hex_bg_valid || vnc_hex_bg != bg) flags |= VNC_HEX_BG;

    if (colours == 1) {
        at = vnc_reserve(1ul + ((flags & VNC_HEX_BG) ? vnc_pixel_bytes : 0ul));
        if (at == 0) return 0;
        at[0] = flags;
        if (flags & VNC_HEX_BG) vnc_store_pixel(at + 1, bg);
        vnc_hex_bg = bg;
        vnc_hex_bg_valid = 1;
        return 1;
    }

    mono = colours == 2;
    count = vnc_hextile_subrects(width, height, bg, mono, &bytes);
    if (count < 0) {
        at = vnc_reserve(1ul + (unsigned long)width * (unsigned long)height *
                         vnc_pixel_bytes);
        if (at == 0) return 0;
        *at++ = VNC_HEX_RAW;
        for (index = 0; index < width * height; ++index) {
            vnc_store_pixel(at, vnc_tile[index]);
            at += vnc_pixel_bytes;
        }
        vnc_hex_bg_valid = 0;
        vnc_hex_fg_valid = 0;
        return 1;
    }
    flags |= VNC_HEX_ANY;
    if (mono) {
        fg = bg == first ? second : first;
        if (!vnc_hex_fg_valid || vnc_hex_fg != fg) flags |= VNC_HEX_FG;
    } else {
        flags |= VNC_HEX_COLOURED;
    }
    at = vnc_reserve(1ul + ((flags & VNC_HEX_BG) ? vnc_pixel_bytes : 0ul) +
                     ((flags & VNC_HEX_FG) ? vnc_pixel_bytes : 0ul) + 1ul + bytes);
    if (at == 0) return 0;
    *at++ = flags;
    if (flags & VNC_HEX_BG) { vnc_store_pixel(at, bg); at += vnc_pixel_bytes; }
    if (flags & VNC_HEX_FG) { vnc_store_pixel(at, fg); at += vnc_pixel_bytes; }
    *at++ = (unsigned char)count;
    vnc_copy(at, vnc_subrects, bytes);
    vnc_hex_bg = bg;
    vnc_hex_bg_valid = 1;
    if (mono) {
        vnc_hex_fg = fg;
        vnc_hex_fg_valid = 1;
    } else {
        vnc_hex_fg_valid = 0;
    }
    return 1;
}

/* ZRLE (RFC 6143 7.7.6): 64x64 tiles, each raw, solid, packed palette,
   plain RLE or palette RLE, whichever is smallest, all through one zlib
   stream per connection. Our rectangles are runs of 16-pixel tile rows, so
   ZRLE tiles here are at most 64x16. Buffers are allocated the first time a
   viewer asks for ZRLE, sized for the widest screen. */
#define VNC_ENC_ZRLE 16l
#define VNC_ZRLE_TILE 64
#define VNC_ZRLE_RAW_CAP ((unsigned long)VNC_MAX_DIM * VNC_TILE * 4ul + 1024ul)

static int vnc_use_zrle;
static int vnc_zstream_started;
static VncDeflate vnc_zstream;
static unsigned long *vnc_zhead;
static unsigned char *vnc_zraw;
static unsigned long vnc_zraw_length;
static unsigned char *vnc_zout;
static unsigned long vnc_zout_capacity;
static DWORD vnc_ztile[VNC_ZRLE_TILE * VNC_TILE];
static DWORD vnc_zpalette[128];
static unsigned int vnc_zpalette_count;
static DWORD vnc_zkeys[256];
static unsigned char vnc_zslots[256];      /* palette index + 1, 0 = empty */

static int vnc_zrle_buffers(void)
{
    if (vnc_zraw != 0) return 1;
    vnc_zout_capacity = vnc_deflate_bound(VNC_ZRLE_RAW_CAP);
    vnc_zhead = (unsigned long *)GlobalAlloc(GMEM_FIXED, VNC_DEFLATE_HASH_SIZE * 4ul);
    vnc_zraw = (unsigned char *)GlobalAlloc(GMEM_FIXED, VNC_ZRLE_RAW_CAP);
    vnc_zout = (unsigned char *)GlobalAlloc(GMEM_FIXED, vnc_zout_capacity);
    if (vnc_zhead == 0 || vnc_zraw == 0 || vnc_zout == 0) {
        if (vnc_zhead != 0) (void)GlobalFree((HGLOBAL)vnc_zhead);
        if (vnc_zraw != 0) (void)GlobalFree((HGLOBAL)vnc_zraw);
        if (vnc_zout != 0) (void)GlobalFree((HGLOBAL)vnc_zout);
        vnc_zhead = 0;
        vnc_zraw = 0;
        vnc_zout = 0;
        return 0;
    }
    return 1;
}

static void vnc_zput(unsigned char value)
{
    vnc_zraw[vnc_zraw_length++] = value;
}

static void vnc_zput_cpixel(DWORD pixel)
{
    unsigned char wire[4];
    unsigned int index;
    vnc_store_pixel(wire, pixel);
    for (index = 0u; index < vnc_cpixel_bytes; ++index) {
        vnc_zput(wire[vnc_cpixel_skip + index]);
    }
}

static void vnc_zput_run_length(unsigned long length)
{
    unsigned long value = length - 1ul;
    while (value >= 255ul) {
        vnc_zput(255u);
        value -= 255ul;
    }
    vnc_zput((unsigned char)value);
}

static unsigned long vnc_zrun_bytes(unsigned long length)
{
    return (length - 1ul) / 255ul + 1ul;
}

/* Palette index of a pixel, adding it while there is room (127 is the most
   palette RLE can address). Returns -1 when the palette is full. */
static int vnc_zpalette_index(DWORD pixel)
{
    unsigned int slot = (unsigned int)((pixel ^ (pixel >> 8) ^ (pixel >> 16) ^
                                        (pixel >> 24)) & 0xfful);
    for (;;) {
        if (vnc_zslots[slot] == 0u) {
            if (vnc_zpalette_count >= 127u) return -1;
            vnc_zpalette[vnc_zpalette_count] = pixel;
            vnc_zkeys[slot] = pixel;
            vnc_zslots[slot] = (unsigned char)(++vnc_zpalette_count);
            return (int)vnc_zpalette_count - 1;
        }
        if (vnc_zkeys[slot] == pixel) return (int)vnc_zslots[slot] - 1;
        slot = (slot + 1u) & 0xffu;
    }
}

static void vnc_encode_zrle_tile(int x0, int y0, int width, int height)
{
    unsigned long count = (unsigned long)width * (unsigned long)height;
    unsigned long index;
    unsigned long raw_size = count * vnc_cpixel_bytes;
    unsigned long plain_size = 0ul;
    unsigned long palette_rle_size = 0ul;
    unsigned long packed_size = ~0ul;
    unsigned long best;
    int too_many = 0;
    int row;
    int column;
    unsigned int bits = 0u;
    unsigned long run;

    for (row = 0; row < height; ++row) {
        const DWORD *source = vnc_prev + (long)(y0 + row) * vnc_width + x0;
        for (column = 0; column < width; ++column) {
            vnc_ztile[row * width + column] = vnc_translate(source[column]);
        }
    }
    vnc_zero(vnc_zslots, sizeof(vnc_zslots));
    vnc_zpalette_count = 0u;
    for (index = 0ul; index < count; index += run) {
        run = 1ul;
        while (index + run < count && vnc_ztile[index + run] == vnc_ztile[index]) ++run;
        if (!too_many && vnc_zpalette_index(vnc_ztile[index]) < 0) too_many = 1;
        plain_size += vnc_cpixel_bytes + vnc_zrun_bytes(run);
        palette_rle_size += run == 1ul ? 1ul : 1ul + vnc_zrun_bytes(run);
    }
    if (!too_many && vnc_zpalette_count == 1u) {
        vnc_zput(1u);
        vnc_zput_cpixel(vnc_zpalette[0]);
        return;
    }
    if (!too_many && vnc_zpalette_count <= 16u) {
        bits = vnc_zpalette_count <= 2u ? 1u : vnc_zpalette_count <= 4u ? 2u : 4u;
        packed_size = vnc_zpalette_count * vnc_cpixel_bytes +
                      (unsigned long)height * (((unsigned long)width * bits + 7ul) / 8ul);
    }
    if (too_many) palette_rle_size = ~0ul;
    else palette_rle_size += vnc_zpalette_count * vnc_cpixel_bytes;
    best = raw_size;
    if (plain_size < best) best = plain_size;
    if (palette_rle_size < best) best = palette_rle_size;
    if (packed_size < best) best = packed_size;

    if (best == packed_size) {
        vnc_zput((unsigned char)vnc_zpalette_count);
        for (index = 0ul; index < vnc_zpalette_count; ++index) vnc_zput_cpixel(vnc_zpalette[index]);
        for (row = 0; row < height; ++row) {
            unsigned int byte = 0u;
            unsigned int filled = 0u;
            for (column = 0; column < width; ++column) {
                byte = (byte << bits) |
                       (unsigned int)vnc_zpalette_index(vnc_ztile[row * width + column]);
                filled += bits;
                if (filled == 8u) {
                    vnc_zput((unsigned char)byte);
                    byte = 0u;
                    filled = 0u;
                }
            }
            if (filled != 0u) vnc_zput((unsigned char)(byte << (8u - filled)));
        }
    } else if (best == palette_rle_size) {
        vnc_zput((unsigned char)(128u + vnc_zpalette_count));
        for (index = 0ul; index < vnc_zpalette_count; ++index) vnc_zput_cpixel(vnc_zpalette[index]);
        for (index = 0ul; index < count; index += run) {
            unsigned char entry = (unsigned char)vnc_zpalette_index(vnc_ztile[index]);
            run = 1ul;
            while (index + run < count && vnc_ztile[index + run] == vnc_ztile[index]) ++run;
            if (run == 1ul) {
                vnc_zput(entry);
            } else {
                vnc_zput((unsigned char)(entry | 128u));
                vnc_zput_run_length(run);
            }
        }
    } else if (best == plain_size) {
        vnc_zput(128u);
        for (index = 0ul; index < count; index += run) {
            run = 1ul;
            while (index + run < count && vnc_ztile[index + run] == vnc_ztile[index]) ++run;
            vnc_zput_cpixel(vnc_ztile[index]);
            vnc_zput_run_length(run);
        }
    } else {
        vnc_zput(0u);
        for (index = 0ul; index < count; ++index) vnc_zput_cpixel(vnc_ztile[index]);
    }
}

static int vnc_encode_zrle(int x, int y, int width, int height)
{
    int tile_x;
    int tile_y;
    unsigned long packed;
    unsigned long sent = 0ul;
    unsigned char *at;
    vnc_zraw_length = 0ul;
    for (tile_y = y; tile_y < y + height; tile_y += VNC_ZRLE_TILE) {
        for (tile_x = x; tile_x < x + width; tile_x += VNC_ZRLE_TILE) {
            int tile_w = x + width - tile_x < VNC_ZRLE_TILE ? x + width - tile_x : VNC_ZRLE_TILE;
            int tile_h = y + height - tile_y < VNC_ZRLE_TILE ? y + height - tile_y : VNC_ZRLE_TILE;
            vnc_encode_zrle_tile(tile_x, tile_y, tile_w, tile_h);
        }
    }
    packed = vnc_deflate_sync(&vnc_zstream, vnc_zraw, vnc_zraw_length, vnc_zout,
                              vnc_zout_capacity);
    if (packed == 0ul) {
        vnc_log("zrle-overflow", vnc_zraw_length);
        return 0;
    }
    at = vnc_reserve(4ul);
    if (at == 0) return 0;
    vnc_put32(at, packed);
    while (sent < packed) {
        unsigned long part = packed - sent;
        if (part > 32768ul) part = 32768ul;
        at = vnc_reserve(part);
        if (at == 0) return 0;
        vnc_copy(at, vnc_zout + sent, part);
        sent += part;
    }
    return 1;
}

static int vnc_encode_hextile(int x, int y, int width, int height)
{
    int tile_x;
    int tile_y;
    vnc_hex_bg_valid = 0;
    vnc_hex_fg_valid = 0;
    for (tile_y = y; tile_y < y + height; tile_y += VNC_TILE) {
        for (tile_x = x; tile_x < x + width; tile_x += VNC_TILE) {
            int tile_w = x + width - tile_x < VNC_TILE ? x + width - tile_x : VNC_TILE;
            int tile_h = y + height - tile_y < VNC_TILE ? y + height - tile_y : VNC_TILE;
            if (!vnc_encode_hextile_tile(tile_x, tile_y, tile_w, tile_h)) return 0;
        }
    }
    return 1;
}

/* CopyRect for a moved window. The worker records the foreground window and
   its rectangle with every capture; when the same window turns up at the
   same size somewhere else, the viewer is told to copy the pixels it
   already has instead of being sent them again. The copy is applied to
   vnc_prev too (it mirrors the viewer's framebuffer), and the tile diff
   that follows sends whatever the copy did not get right. */
#define VNC_ENC_COPYRECT 1l
#define VNC_COPY_MIN 32

static HWND vnc_foreground;              /* worker, during a capture */
static RECT vnc_foreground_rect;
static HWND vnc_last_foreground;         /* as of the last frame sent */
static RECT vnc_last_foreground_rect;
static int vnc_use_copyrect;
static int vnc_copy_pending;
static int vnc_copy_x;
static int vnc_copy_y;
static int vnc_copy_w;
static int vnc_copy_h;
static int vnc_copy_src_x;
static int vnc_copy_src_y;

static void vnc_move_bytes(unsigned char *target, const unsigned char *source,
                           unsigned long length)
{
    if (target < source) {
        while (length-- != 0ul) *target++ = *source++;
    } else {
        target += length;
        source += length;
        while (length-- != 0ul) *--target = *--source;
    }
}

static void vnc_detect_move(void)
{
    const RECT *was = &vnc_last_foreground_rect;
    const RECT *now = &vnc_foreground_rect;
    int dx;
    int dy;
    int x0;
    int y0;
    int x1;
    int y1;
    int row;
    HWND window = vnc_foreground;
    vnc_copy_pending = 0;
    if (vnc_use_copyrect && window != 0 && window == vnc_last_foreground &&
        now->right - now->left == was->right - was->left &&
        now->bottom - now->top == was->bottom - was->top &&
        (now->left != was->left || now->top != was->top)) {
        dx = (int)(now->left - was->left);
        dy = (int)(now->top - was->top);
        /* Destination inside the screen, and its source too. */
        x0 = (int)now->left;
        if (x0 < 0) x0 = 0;
        if (x0 < dx) x0 = dx;
        y0 = (int)now->top;
        if (y0 < 0) y0 = 0;
        if (y0 < dy) y0 = dy;
        x1 = (int)now->right;
        if (x1 > vnc_width) x1 = vnc_width;
        if (x1 > vnc_width + dx) x1 = vnc_width + dx;
        y1 = (int)now->bottom;
        if (y1 > vnc_height) y1 = vnc_height;
        if (y1 > vnc_height + dy) y1 = vnc_height + dy;
        if (x1 - x0 >= VNC_COPY_MIN && y1 - y0 >= VNC_COPY_MIN) {
            vnc_copy_x = x0;
            vnc_copy_y = y0;
            vnc_copy_w = x1 - x0;
            vnc_copy_h = y1 - y0;
            vnc_copy_src_x = x0 - dx;
            vnc_copy_src_y = y0 - dy;
            for (row = 0; row < vnc_copy_h; ++row) {
                /* Moving down copies bottom rows first, so no source row is
                   overwritten before it is read. */
                int r = dy > 0 ? vnc_copy_h - 1 - row : row;
                vnc_move_bytes((unsigned char *)(vnc_prev + (long)(vnc_copy_y + r) * vnc_width +
                                                 vnc_copy_x),
                               (const unsigned char *)(vnc_prev +
                                                       (long)(vnc_copy_src_y + r) * vnc_width +
                                                       vnc_copy_src_x),
                               (unsigned long)vnc_copy_w * 4ul);
            }
            vnc_copy_pending = 1;
        }
    }
    vnc_last_foreground = window;
    vnc_last_foreground_rect = *now;
}

static int vnc_use_pointer_pos;
static unsigned long vnc_cursor_sent_serial;
static POINT vnc_pos_sent;

/* Whether the next update must carry the cursor shape (RichCursor) or the
   guest's own cursor position (PointerPos). The position is sent only when
   it moved other than to where the viewer itself last put the pointer. */
static int vnc_cursor_shape_due(void)
{
    return vnc_rich_cursor && vnc_cursor_serial != vnc_cursor_sent_serial;
}

static int vnc_cursor_pos_due(void)
{
    if (!vnc_use_pointer_pos || !vnc_cursor_at_valid) return 0;
    if (vnc_cursor_at.x == vnc_pos_sent.x && vnc_cursor_at.y == vnc_pos_sent.y) return 0;
    if (vnc_cursor_at.x == vnc_pointer_x && vnc_cursor_at.y == vnc_pointer_y) {
        vnc_pos_sent = vnc_cursor_at;
        return 0;
    }
    return 1;
}

static int vnc_send_cursor_shape(void)
{
    unsigned long row_bytes = ((unsigned long)vnc_cursor_w + 7ul) / 8ul;
    unsigned char *at;
    int index;
    int row;
    int column;
    if (!vnc_rect_header(vnc_cursor_hot_x, vnc_cursor_hot_y, vnc_cursor_w, vnc_cursor_h,
                         VNC_ENC_RICH_CURSOR)) return 0;
    at = vnc_reserve((unsigned long)vnc_cursor_w * (unsigned long)vnc_cursor_h *
                     vnc_pixel_bytes + row_bytes * (unsigned long)vnc_cursor_h);
    if (at == 0) return 0;
    for (index = 0; index < vnc_cursor_w * vnc_cursor_h; ++index) {
        vnc_store_pixel(at, vnc_translate(vnc_cursor_rgb[index]));
        at += vnc_pixel_bytes;
    }
    for (row = 0; row < vnc_cursor_h; ++row) {
        vnc_zero(at, row_bytes);
        for (column = 0; column < vnc_cursor_w; ++column) {
            if (vnc_cursor_mask[row * vnc_cursor_w + column]) {
                at[column / 8] |= (unsigned char)(0x80u >> (column % 8));
            }
        }
        at += row_bytes;
    }
    vnc_cursor_sent_serial = vnc_cursor_serial;
    return 1;
}

/* Send every dirty tile, one rectangle per horizontal run of dirty tiles,
   plus the cursor shape and position when they are due. */
static int vnc_send_update(void)
{
    unsigned long rects = 0ul;
    unsigned char *at;
    int tile_x;
    int tile_y;
    int run;
    int shape = vnc_cursor_shape_due();
    int position = vnc_cursor_pos_due();
    for (tile_y = 0; tile_y < vnc_tiles_y; ++tile_y) {
        for (tile_x = 0; tile_x < vnc_tiles_x; ++tile_x) {
            if (vnc_dirty[tile_y * vnc_tiles_x + tile_x] &&
                (tile_x == 0 || !vnc_dirty[tile_y * vnc_tiles_x + tile_x - 1])) {
                ++rects;
            }
        }
    }
    rects += (unsigned long)shape + (unsigned long)position +
             (unsigned long)vnc_copy_pending;
    if (rects == 0ul) return 1;
    at = vnc_reserve(4ul);
    if (at == 0) return 0;
    at[0] = 0u;
    at[1] = 0u;
    vnc_put16(at + 2, (unsigned int)rects);
    /* CopyRect first: it copies from the viewer's framebuffer as it was
       before this update, which is what vnc_prev held before the copy. */
    if (vnc_copy_pending) {
        if (!vnc_rect_header(vnc_copy_x, vnc_copy_y, vnc_copy_w, vnc_copy_h,
                             VNC_ENC_COPYRECT)) return 0;
        at = vnc_reserve(4ul);
        if (at == 0) return 0;
        vnc_put16(at, (unsigned int)vnc_copy_src_x);
        vnc_put16(at + 2, (unsigned int)vnc_copy_src_y);
        vnc_copy_pending = 0;
    }
    if (shape && !vnc_send_cursor_shape()) return 0;
    if (position) {
        if (!vnc_rect_header((int)vnc_cursor_at.x, (int)vnc_cursor_at.y, 0, 0,
                             VNC_ENC_POINTER_POS)) return 0;
        vnc_pos_sent = vnc_cursor_at;
    }
    for (tile_y = 0; tile_y < vnc_tiles_y; ++tile_y) {
        for (tile_x = 0; tile_x < vnc_tiles_x; tile_x += run) {
            int x;
            int y;
            int width;
            int height;
            run = 1;
            if (!vnc_dirty[tile_y * vnc_tiles_x + tile_x]) continue;
            while (tile_x + run < vnc_tiles_x &&
                   vnc_dirty[tile_y * vnc_tiles_x + tile_x + run]) ++run;
            x = tile_x * VNC_TILE;
            y = tile_y * VNC_TILE;
            width = (tile_x + run) * VNC_TILE > vnc_width ? vnc_width - x : run * VNC_TILE;
            height = y + VNC_TILE > vnc_height ? vnc_height - y : VNC_TILE;
            if (vnc_use_zrle) {
                if (!vnc_rect_header(x, y, width, height, VNC_ENC_ZRLE) ||
                    !vnc_encode_zrle(x, y, width, height)) return 0;
            } else if (vnc_use_hextile) {
                if (!vnc_rect_header(x, y, width, height, VNC_ENC_HEXTILE) ||
                    !vnc_encode_hextile(x, y, width, height)) return 0;
            } else {
                if (!vnc_rect_header(x, y, width, height, VNC_ENC_RAW) ||
                    !vnc_encode_raw(x, y, width, height)) return 0;
            }
        }
    }
    return vnc_flush();
}

static int vnc_send_desktop_size(void)
{
    unsigned char *at = vnc_reserve(4ul);
    if (at == 0) return 0;
    at[0] = 0u;
    at[1] = 0u;
    vnc_put16(at + 2, 1u);
    if (!vnc_rect_header(0, 0, vnc_width, vnc_height, VNC_ENC_DESKTOP_SIZE)) return 0;
    return vnc_flush();
}

/* ------------------------------------------------------------------ input */

typedef struct {
    DWORD keysym;
    unsigned char vk;
    unsigned char extended;
} VncKeyMap;

static const VncKeyMap vnc_key_map[] = {
    { 0xff08ul, VK_BACK, 0u }, { 0xff09ul, VK_TAB, 0u },
    { 0xfe20ul, VK_TAB, 0u }, { 0xff0dul, VK_RETURN, 0u },
    { 0xff13ul, VK_PAUSE, 0u }, { 0xff14ul, VK_SCROLL, 0u },
    { 0xff1bul, VK_ESCAPE, 0u }, { 0xfffful, VK_DELETE, 1u },
    { 0xff50ul, VK_HOME, 1u }, { 0xff51ul, VK_LEFT, 1u },
    { 0xff52ul, VK_UP, 1u }, { 0xff53ul, VK_RIGHT, 1u },
    { 0xff54ul, VK_DOWN, 1u }, { 0xff55ul, VK_PRIOR, 1u },
    { 0xff56ul, VK_NEXT, 1u }, { 0xff57ul, VK_END, 1u },
    { 0xff61ul, VK_SNAPSHOT, 1u }, { 0xff63ul, VK_INSERT, 1u },
    { 0xff67ul, VK_APPS, 1u }, { 0xff7ful, VK_NUMLOCK, 0u },
    { 0xff8dul, VK_RETURN, 1u },
    { 0xff95ul, VK_HOME, 0u }, { 0xff96ul, VK_LEFT, 0u },
    { 0xff97ul, VK_UP, 0u }, { 0xff98ul, VK_RIGHT, 0u },
    { 0xff99ul, VK_DOWN, 0u }, { 0xff9aul, VK_PRIOR, 0u },
    { 0xff9bul, VK_NEXT, 0u }, { 0xff9cul, VK_END, 0u },
    { 0xff9dul, VK_CLEAR, 0u }, { 0xff9eul, VK_INSERT, 0u },
    { 0xff9ful, VK_DELETE, 0u },
    { 0xffaaul, VK_MULTIPLY, 0u }, { 0xffabul, VK_ADD, 0u },
    { 0xffacul, VK_SEPARATOR, 0u }, { 0xffadul, VK_SUBTRACT, 0u },
    { 0xffaeul, VK_DECIMAL, 0u }, { 0xffaful, VK_DIVIDE, 1u },
    { 0xffe1ul, VK_SHIFT, 0u }, { 0xffe2ul, VK_SHIFT, 0u },
    { 0xffe3ul, VK_CONTROL, 0u }, { 0xffe4ul, VK_CONTROL, 1u },
    { 0xffe5ul, VK_CAPITAL, 0u },
    { 0xffe7ul, VK_MENU, 0u }, { 0xffe8ul, VK_MENU, 1u },
    { 0xffe9ul, VK_MENU, 0u }, { 0xffeaul, VK_MENU, 1u },
    { 0xffebul, VK_LWIN, 1u }, { 0xffecul, VK_RWIN, 1u }
};

static void vnc_key(unsigned char vk, unsigned char extended, int down)
{
    UINT scan = MapVirtualKeyA((UINT)vk, 0u);
    DWORD flags = down ? 0ul : KEYEVENTF_KEYUP;
    if (extended) flags |= KEYEVENTF_EXTENDEDKEY;
    keybd_event(vk, (BYTE)scan, flags, 0ul);
    vnc_keys_down[vk] = (unsigned char)(down != 0);
    vnc_keys_extended[vk] = extended;
}

static void vnc_key_event(int down, DWORD keysym)
{
    unsigned long index;
    unsigned char vk = 0u;
    unsigned char extended = 0u;
    SHORT scan;
    int need_shift;
    int shift_held;
    for (index = 0ul; index < sizeof(vnc_key_map) / sizeof(vnc_key_map[0]); ++index) {
        if (vnc_key_map[index].keysym == keysym) {
            vk = vnc_key_map[index].vk;
            extended = vnc_key_map[index].extended;
            break;
        }
    }
    if (vk == 0u && keysym >= 0xffb0ul && keysym <= 0xffb9ul) {
        vk = (unsigned char)(VK_NUMPAD0 + (int)(keysym - 0xffb0ul));
    } else if (vk == 0u && keysym >= 0xffbeul && keysym <= 0xffc9ul) {
        vk = (unsigned char)(VK_F1 + (int)(keysym - 0xffbeul));
    }
    if (vk != 0u) {
        if (down || vnc_keys_down[vk]) vnc_key(vk, extended, down);
        return;
    }
    if (keysym < 0x20ul || keysym > 0xfful) return;
    /* A Latin-1 character: the layout picks the key. The viewer already sent
       its own Shift; add one only when the layout needs it and none is held
       (a viewer that sends '!' without Shift). */
    scan = VkKeyScanA((char)(unsigned char)keysym);
    if (scan == -1) return;
    vk = (unsigned char)(scan & 0xff);
    if (!down) {
        if (vnc_keys_down[vk]) vnc_key(vk, 0u, 0);
        return;
    }
    need_shift = (scan & 0x100) != 0;
    shift_held = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (need_shift && !shift_held) {
        keybd_event(VK_SHIFT, (BYTE)MapVirtualKeyA(VK_SHIFT, 0u), 0ul, 0ul);
        vnc_key(vk, 0u, 1);
        keybd_event(VK_SHIFT, (BYTE)MapVirtualKeyA(VK_SHIFT, 0u), KEYEVENTF_KEYUP, 0ul);
    } else {
        vnc_key(vk, 0u, 1);
    }
}

static void vnc_button(unsigned char mask, unsigned char bit, DWORD down_flag,
                       DWORD up_flag)
{
    if ((mask & bit) != (vnc_buttons & bit)) {
        mouse_event((mask & bit) ? down_flag : up_flag, 0ul, 0ul, 0ul, 0ul);
    }
}

static void vnc_pointer_event(unsigned char mask, int x, int y)
{
    if (x >= vnc_width) x = vnc_width - 1;
    if (y >= vnc_height) y = vnc_height - 1;
    if (x != vnc_pointer_x || y != vnc_pointer_y) {
        (void)SetCursorPos(x, y);
        vnc_pointer_x = x;
        vnc_pointer_y = y;
    }
    vnc_button(mask, 1u, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP);
    vnc_button(mask, 2u, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP);
    vnc_button(mask, 4u, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP);
    if ((mask & 8u) && !(vnc_buttons & 8u)) {
        mouse_event(MOUSEEVENTF_WHEEL, 0ul, 0ul, (DWORD)WHEEL_DELTA, 0ul);
    }
    if ((mask & 16u) && !(vnc_buttons & 16u)) {
        mouse_event(MOUSEEVENTF_WHEEL, 0ul, 0ul, (DWORD)-WHEEL_DELTA, 0ul);
    }
    vnc_buttons = mask;
}

/* Never leave a key or button stuck down after the viewer goes away. */
static void vnc_release_all(void)
{
    unsigned int vk;
    for (vk = 1u; vk < 256u; ++vk) {
        if (vnc_keys_down[vk]) vnc_key((unsigned char)vk, vnc_keys_extended[vk], 0);
    }
    vnc_pointer_event(0u, vnc_pointer_x < 0 ? 0 : vnc_pointer_x,
                      vnc_pointer_y < 0 ? 0 : vnc_pointer_y);
}

/* -------------------------------------------------------------- clipboard */

/* Text clipboard both ways (RFB cut text is Latin-1 with LF line ends; the
   Windows side is CF_TEXT with CRLF). Every clipboard call runs on its own
   thread, which owns the hidden clipboard window: opening or reading the
   clipboard can block on another program that owns it, and then only the
   clipboard stops. (It cannot be the GDI worker: on Windows 2000 that
   thread follows the input desktop, and SetThreadDesktop refuses a thread
   that owns a window.) Viewer text arrives on the network thread into
   vnc_clip_recv and is swapped into vnc_clip_in only while the clipboard
   thread is not using it (vnc_clip_in_ready 0), so no buffer is ever
   touched by two threads at once. Changes on the Windows side are found
   by polling (no clipboard chain: that needs a message loop every viewer of
   the chain depends on) and compared by hash, which also keeps text that
   came from the viewer from being echoed back to it. */

#define VNC_CLIP_MAX 262144ul
#define VNC_CLIP_POLL_MS 500ul

static HWND vnc_clip_window;
static unsigned char *vnc_clip_recv;         /* network thread */
static unsigned long vnc_clip_recv_length;
static int vnc_clip_recv_ready;
static unsigned char *vnc_clip_in;           /* clipboard thread while ready */
static unsigned long vnc_clip_in_length;
static volatile LONG vnc_clip_in_ready;
static unsigned char *vnc_clip_out;          /* clipboard thread writes, network sends */
static unsigned long vnc_clip_out_length;
static volatile LONG vnc_clip_out_ready;
static volatile LONG vnc_clip_reset;
static DWORD vnc_clip_hash;
static int vnc_clip_primed;
static DWORD vnc_clip_polled;

/* FNV-1a over the text without CR and NUL, so CRLF and LF forms match. */
static DWORD vnc_clip_digest(const unsigned char *text, unsigned long length)
{
    DWORD hash = 2166136261ul;
    unsigned long index;
    for (index = 0ul; index < length; ++index) {
        if (text[index] == '\r' || text[index] == 0u) continue;
        hash = (hash ^ (DWORD)text[index]) * 16777619ul;
    }
    return hash;
}

static void vnc_clip_apply_in(void)
{
    unsigned long index;
    unsigned long size = 1ul;
    HGLOBAL memory;
    unsigned char *target;
    if (!vnc_clip_in_ready) return;
    for (index = 0ul; index < vnc_clip_in_length; ++index) {
        unsigned char c = vnc_clip_in[index];
        if (c == '\r' || c == 0u) continue;
        size += c == '\n' ? 2ul : 1ul;
    }
    memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, size);
    if (memory == 0) { vnc_clip_in_ready = 0l; return; }
    target = (unsigned char *)GlobalLock(memory);
    if (target == 0) { (void)GlobalFree(memory); vnc_clip_in_ready = 0l; return; }
    for (index = 0ul; index < vnc_clip_in_length; ++index) {
        unsigned char c = vnc_clip_in[index];
        if (c == '\r' || c == 0u) continue;
        if (c == '\n') *target++ = '\r';
        *target++ = c;
    }
    *target = 0u;
    (void)GlobalUnlock(memory);
    if (!OpenClipboard(vnc_clip_window)) {
        (void)GlobalFree(memory);
        vnc_log("clipboard-busy", 0ul);
        vnc_clip_in_ready = 0l;
        return;
    }
    (void)EmptyClipboard();
    if (SetClipboardData(CF_TEXT, memory) == 0) (void)GlobalFree(memory);
    (void)CloseClipboard();
    vnc_clip_hash = vnc_clip_digest(vnc_clip_in, vnc_clip_in_length);
    vnc_clip_primed = 1;
    vnc_log("clipboard-from-viewer", vnc_clip_in_length);
    vnc_clip_in_ready = 0l;  /* the buffer is the network thread's again */
}

/* Look at the Windows clipboard at most every VNC_CLIP_POLL_MS. The first
   look of a session only records it, so connecting does not push whatever
   happens to be on the clipboard. */
static void vnc_clip_poll_out(void)
{
    HANDLE data;
    const unsigned char *source;
    unsigned long limit;
    unsigned long length = 0ul;
    DWORD hash;
    if (InterlockedExchange((LPLONG)&vnc_clip_reset, 0l)) vnc_clip_primed = 0;
    if (vnc_clip_primed && GetTickCount() - vnc_clip_polled < VNC_CLIP_POLL_MS) return;
    vnc_clip_polled = GetTickCount();
    if (vnc_clip_out_ready || vnc_clip_out == 0) return;
    if (!IsClipboardFormatAvailable(CF_TEXT)) {
        hash = vnc_clip_digest(vnc_clip_out, 0ul);
    } else {
        if (!OpenClipboard(vnc_clip_window)) return;
        data = GetClipboardData(CF_TEXT);
        if (data != 0) {
            source = (const unsigned char *)GlobalLock(data);
            if (source != 0) {
                limit = GlobalSize(data);
                while (limit-- != 0ul && *source != 0u && length < VNC_CLIP_MAX) {
                    if (*source != '\r') vnc_clip_out[length++] = *source;
                    ++source;
                }
                (void)GlobalUnlock(data);
            }
        }
        (void)CloseClipboard();
        hash = vnc_clip_digest(vnc_clip_out, length);
    }
    if (!vnc_clip_primed) {
        vnc_clip_hash = hash;
        vnc_clip_primed = 1;
        return;
    }
    if (hash == vnc_clip_hash) return;
    vnc_clip_hash = hash;
    if (length == 0ul) return; /* cleared, or not text: nothing to send */
    vnc_clip_out_length = length;
    vnc_clip_out_ready = 1l;
}

static int vnc_send_cut_text(void)
{
    unsigned char *at = vnc_reserve(8ul);
    unsigned long sent = 0ul;
    if (at == 0) return 0;
    at[0] = 3u;
    at[1] = 0u;
    at[2] = 0u;
    at[3] = 0u;
    vnc_put32(at + 4, vnc_clip_out_length);
    while (sent < vnc_clip_out_length) {
        unsigned long part = vnc_clip_out_length - sent;
        if (part > 32768ul) part = 32768ul;
        at = vnc_reserve(part);
        if (at == 0) return 0;
        vnc_copy(at, vnc_clip_out + sent, part);
        sent += part;
    }
    vnc_log("clipboard-to-viewer", vnc_clip_out_length);
    return vnc_flush();
}

static HANDLE vnc_clip_event;
static volatile LONG vnc_clip_session;   /* poll only while a viewer is on */

static DWORD WINAPI vnc_clip_thread(LPVOID parameter)
{
    MSG message;
    (void)parameter;
    /* The hidden window owns what this server puts on the clipboard, and
       must answer messages sent to it (another program emptying the
       clipboard sends WM_DESTROYCLIPBOARD and waits), so this thread waits
       for work and messages together. */
    vnc_clip_window = CreateWindowExA(0ul, "STATIC", "V9XVNC", 0ul, 0, 0, 0, 0,
                                      0, 0, GetModuleHandleA(0), 0);
    for (;;) {
        DWORD woke = MsgWaitForMultipleObjects(1ul, &vnc_clip_event, FALSE,
                                               VNC_CLIP_POLL_MS, QS_ALLINPUT);
        if (woke == WAIT_OBJECT_0 + 1ul) {
            while (PeekMessageA(&message, 0, 0u, 0u, PM_REMOVE)) {
                (void)DispatchMessageA(&message);
            }
        }
        if (!vnc_clip_session) continue;
        vnc_clip_apply_in();
        vnc_clip_poll_out();
    }
}

/* Network thread: hand received viewer text over when the clipboard thread
   is idle with its buffer. */
static void vnc_clip_hand_over(void)
{
    unsigned char *swap;
    if (!vnc_clip_recv_ready || vnc_clip_in_ready) return;
    swap = vnc_clip_in;
    vnc_clip_in = vnc_clip_recv;
    vnc_clip_in_length = vnc_clip_recv_length;
    vnc_clip_recv = swap;
    vnc_clip_recv_ready = 0;
    vnc_clip_in_ready = 1l;
    (void)SetEvent(vnc_clip_event);
}

/* --------------------------------------------------------------- protocol */

/* [vnc] password: optional, at most 8 characters (all VNC authentication
   uses). It protects screen and input only; the agent itself, which can
   read AGENT.INI, stays unauthenticated. */
static char vnc_password[9];
static unsigned long vnc_challenge_counter;

/* 16 unpredictable-enough bytes without a C runtime: timers, process state
   and a counter, whitened through DES. */
static void vnc_make_challenge(unsigned char challenge[16])
{
    unsigned char seed[16];
    LARGE_INTEGER counter;
    DWORD values[4];
    int index;
    counter.LowPart = 0ul;
    counter.HighPart = 0l;
    (void)QueryPerformanceCounter(&counter);
    values[0] = GetTickCount();
    values[1] = counter.LowPart;
    values[2] = (DWORD)counter.HighPart ^ GetCurrentProcessId();
    values[3] = ++vnc_challenge_counter ^ (DWORD)(unsigned long)&counter;
    for (index = 0; index < 16; ++index) {
        seed[index] = (unsigned char)(values[index / 4] >> ((index % 4) * 8));
    }
    vnc_des_encrypt(seed, seed + 8, challenge);
    for (index = 0; index < 8; ++index) seed[index] ^= challenge[index];
    vnc_des_encrypt(challenge, seed, challenge + 8);
}

static int vnc_authenticate(int minor)
{
    static const char reason[] = "Authentication failed";
    unsigned char challenge[16];
    unsigned char response[16];
    unsigned char expected[16];
    unsigned char reply[4 + 4 + sizeof(reason)];
    unsigned char difference = 0u;
    int index;
    vnc_make_challenge(challenge);
    if (!vnc_send_all(challenge, 16ul) || !vnc_recv_all(response, 16ul)) return 0;
    vnc_auth_response(vnc_password, challenge, expected);
    for (index = 0; index < 16; ++index) difference |= (unsigned char)(response[index] ^ expected[index]);
    if (difference == 0u) {
        vnc_put32(reply, 0ul);
        vnc_log("auth-ok", 0ul);
        return vnc_send_all(reply, 4ul);
    }
    vnc_log("auth-failed", 0ul);
    Sleep(2000ul); /* slows guessing; one viewer at a time anyway */
    vnc_put32(reply, 1ul);
    if (minor >= 8) {
        vnc_put32(reply + 4, (DWORD)(sizeof(reason) - 1u));
        vnc_copy(reply + 8, reason, sizeof(reason) - 1u);
        (void)vnc_send_all(reply, 8ul + sizeof(reason) - 1u);
    } else {
        (void)vnc_send_all(reply, 4ul);
    }
    return 0;
}

static int vnc_handshake(void)
{
    static const char version[] = "RFB 003.008\n";
    unsigned char reply[24];
    unsigned char init[24 + 64];
    char name[64];
    DWORD name_length;
    int minor;

    if (!vnc_send_all((const unsigned char *)version, 12ul)) return 0;
    if (!vnc_recv_all(reply, 12ul)) return 0;
    if (reply[0] != 'R' || reply[1] != 'F' || reply[2] != 'B' || reply[3] != ' ' ||
        reply[4] != '0' || reply[5] != '0' || reply[6] != '3' || reply[7] != '.') {
        return 0;
    }
    minor = (reply[8] - '0') * 100 + (reply[9] - '0') * 10 + (reply[10] - '0');
    vnc_log("client-version", (DWORD)minor);
    /* Security type 2 (VNC authentication) when [vnc] password is set, else
       1 (None). RFB 3.7 sends a SecurityResult only after VNC auth; 3.8
       always does, with a reason on failure; 3.3 has the server pick. */
    if (minor < 7) {
        vnc_put32(reply, vnc_password[0] != '\0' ? 2ul : 1ul);
        if (!vnc_send_all(reply, 4ul)) return 0;
    } else {
        reply[0] = 1u;
        reply[1] = vnc_password[0] != '\0' ? 2u : 1u;
        if (!vnc_send_all(reply, 2ul) || !vnc_recv_all(reply + 2, 1ul)) return 0;
        if (reply[2] != reply[1]) return 0;
    }
    if (vnc_password[0] != '\0') {
        if (!vnc_authenticate(minor)) return 0;
    } else if (minor >= 8) {
        vnc_put32(reply, 0ul);
        if (!vnc_send_all(reply, 4ul)) return 0;
    }
    if (!vnc_recv_all(reply, 1ul)) return 0; /* ClientInit shared flag */

    vnc_copy(name, "V9x ", 4ul);
    name_length = sizeof(name) - 4ul;
    if (!GetComputerNameA(name + 4, &name_length)) name_length = 0ul;
    name_length += 4ul;
    vnc_put16(init, (unsigned int)vnc_width);
    vnc_put16(init + 2, (unsigned int)vnc_height);
    vnc_write_format(init + 4);
    vnc_put32(init + 20, name_length);
    vnc_copy(init + 24, name, name_length);
    return vnc_send_all(init, 24ul + name_length);
}

/* Read and act on one client message. Returns 0 to drop the connection. */
static int vnc_handle_message(void)
{
    unsigned long count;
    unsigned long index;
    if (!vnc_recv_all(vnc_in, 1ul)) return 0;
    switch (vnc_in[0]) {
    case 0: /* SetPixelFormat */
        if (!vnc_recv_all(vnc_in, 19ul)) return 0;
        if (!vnc_set_pixel_format(vnc_in + 3)) {
            vnc_log("bad-pixel-format", (DWORD)vnc_in[3]);
            return 0;
        }
        return 1;
    case 2: /* SetEncodings */
        if (!vnc_recv_all(vnc_in, 3ul)) return 0;
        count = (unsigned long)vnc_u16(vnc_in + 1);
        {
            LONG rich = 0l;
            long chosen = VNC_ENC_RAW; /* the viewer's first of ZRLE/Hextile */
            vnc_use_hextile = 0;
            vnc_use_zrle = 0;
            vnc_use_desktop_size = 0;
            vnc_use_pointer_pos = 0;
            vnc_use_copyrect = 0;
            for (index = 0ul; index < count; ++index) {
                long encoding;
                if (!vnc_recv_all(vnc_in, 4ul)) return 0;
                encoding = (long)vnc_u32(vnc_in);
                if (chosen == VNC_ENC_RAW &&
                    (encoding == VNC_ENC_HEXTILE ||
                     (encoding == VNC_ENC_ZRLE && vnc_zrle_buffers()))) {
                    chosen = encoding;
                }
                if (encoding == VNC_ENC_DESKTOP_SIZE) vnc_use_desktop_size = 1;
                if (encoding == VNC_ENC_RICH_CURSOR && vnc_get_cursor_info != 0) rich = 1l;
                if (encoding == VNC_ENC_POINTER_POS) vnc_use_pointer_pos = 1;
                if (encoding == VNC_ENC_COPYRECT) vnc_use_copyrect = 1;
            }
            /* Switching who draws the cursor changes every frame: resend the
               whole screen, and the shape. */
            if (rich != vnc_rich_cursor) {
                vnc_rich_cursor = rich;
                vnc_full_wanted = 1;
            }
            vnc_use_hextile = chosen == VNC_ENC_HEXTILE;
            vnc_use_zrle = chosen == VNC_ENC_ZRLE;
            /* One zlib stream for the whole connection, started once. */
            if (vnc_use_zrle && !vnc_zstream_started) {
                vnc_deflate_init(&vnc_zstream, vnc_zhead);
                vnc_zstream_started = 1;
            }
            vnc_cursor_reset = 1l;
            vnc_log("encoding", (DWORD)chosen);
            vnc_log("encodings-rich-cursor", (DWORD)rich);
        }
        return 1;
    case 3: /* FramebufferUpdateRequest */
        if (!vnc_recv_all(vnc_in, 9ul)) return 0;
        vnc_update_wanted = 1;
        if (vnc_in[0] == 0u) vnc_full_wanted = 1;
        return 1;
    case 4: /* KeyEvent */
        if (!vnc_recv_all(vnc_in, 7ul)) return 0;
        (void)vnc_follow_input_desktop(&vnc_input_desktop);
        vnc_key_event(vnc_in[0] != 0u, vnc_u32(vnc_in + 3));
        return 1;
    case 5: /* PointerEvent */
        if (!vnc_recv_all(vnc_in, 5ul)) return 0;
        (void)vnc_follow_input_desktop(&vnc_input_desktop);
        vnc_pointer_event(vnc_in[0], (int)vnc_u16(vnc_in + 1),
                          (int)vnc_u16(vnc_in + 3));
        return 1;
    case 6: /* ClientCutText: onto the Windows clipboard via the worker */
        if (!vnc_recv_all(vnc_in, 7ul)) return 0;
        count = vnc_u32(vnc_in + 3);
        if (count > VNC_MAX_CUT_TEXT) return 0;
        if (count > VNC_CLIP_MAX || vnc_clip_recv == 0) {
            vnc_log("clipboard-too-large", count);
            return vnc_skip(count);
        }
        if (!vnc_recv_all(vnc_clip_recv, count)) return 0;
        vnc_clip_recv_length = count;
        vnc_clip_recv_ready = 1;
        return 1;
    default:
        vnc_log("unknown-message", (DWORD)vnc_in[0]);
        return 0;
    }
}

/* ------------------------------------------------------- GDI worker thread */

/* Every GDI call (DIB setup, BitBlt, cursor, teardown) runs on this one
   thread, so a display driver that wedges inside GDI stalls only the
   worker. The network thread hands it one job at a time and never blocks
   on it: while a capture is overdue it keeps reading input and simply sends
   no frames, and it resumes when the capture comes back. Setup and teardown
   must finish, so those wait a bounded time and give up on the process. */

#define VNC_JOB_CAPTURE 1
#define VNC_JOB_INIT 2
#define VNC_JOB_FREE 3
#define VNC_STALL_MS 5000ul
#define VNC_GDI_TIMEOUT_MS 15000ul
#define VNC_EXIT_GDI_WEDGED 10ul

static HANDLE vnc_job_event;
static HANDLE vnc_done_event;
static volatile LONG vnc_job;
static volatile LONG vnc_job_result;
static int vnc_job_pending;
static DWORD vnc_job_started;
static int vnc_stalled;

static DWORD WINAPI vnc_worker(LPVOID parameter)
{
    (void)parameter;
    /* All GDI work, and nothing else: the clipboard has its own thread. */
    for (;;) {
        if (WaitForSingleObject(vnc_job_event, INFINITE) != WAIT_OBJECT_0) return 0ul;
        /* Test hook: while TEMP\VNCSTALL.DAT exists the worker behaves like
           one stuck in a wedged display driver, so the watchdog can be
           exercised on a healthy machine. */
        while (GetFileAttributesA(vnc_stall_path) != 0xfffffffful) Sleep(100ul);
        switch (vnc_job) {
        case VNC_JOB_CAPTURE:
            /* Follow a desktop switch (NT; a no-op on 9x and for a user
               process that cannot open Winlogon). A switch makes the whole
               screen new, so read it in full. */
            if (vnc_follow_input_desktop(&vnc_worker_desktop)) {
                vnc_capture_full = 1l;
                vnc_log("desktop-switch", 0ul);
            }
            {
                DWORD started = vnc_ticks();
                vnc_job_result = (LONG)vnc_capture();
                vnc_perf_capture += vnc_ticks() - started;
                ++vnc_perf_captures;
            }
            vnc_foreground = GetForegroundWindow();
            if (vnc_foreground == 0 || !GetWindowRect(vnc_foreground, &vnc_foreground_rect)) {
                vnc_foreground = 0;
            }
            break;
        case VNC_JOB_INIT:
            (void)vnc_follow_input_desktop(&vnc_worker_desktop);
            vnc_job_result = (LONG)vnc_capture_init();
            if (!vnc_job_result) vnc_log("capture-init-failed", GetLastError());
            break;
        case VNC_JOB_FREE: vnc_capture_free(); vnc_job_result = 1l; break;
        default: vnc_job_result = 0l; break;
        }
        (void)SetEvent(vnc_done_event);
    }
}

static void vnc_job_start(LONG job)
{
    vnc_job = job;
    vnc_job_pending = 1;
    vnc_job_started = GetTickCount();
    (void)SetEvent(vnc_job_event);
}

/* 1 when the pending job finished (its result is in vnc_job_result). */
static int vnc_job_poll(DWORD milliseconds)
{
    if (!vnc_job_pending) return 1;
    if (WaitForSingleObject(vnc_done_event, milliseconds) != WAIT_OBJECT_0) return 0;
    vnc_job_pending = 0;
    if (vnc_stalled) {
        vnc_stalled = 0;
        vnc_log("capture-resumed", GetTickCount() - vnc_job_started);
    }
    return 1;
}

/* Run a job to completion. A worker that is still stuck after the timeout
   holds GDI objects this thread can no longer touch, so the process exits
   and the agent (or a person) starts a fresh one. */
static LONG vnc_job_run(LONG job)
{
    int done = vnc_job_poll(VNC_GDI_TIMEOUT_MS); /* a capture still running */
    if (done) {
        vnc_job_start(job);
        done = vnc_job_poll(VNC_GDI_TIMEOUT_MS);
    }
    if (!done) {
        vnc_log("gdi-wedged-exit", (DWORD)job);
        if (vnc_socket != INVALID_SOCKET) {
            vnc_release_all();
            closesocket(vnc_socket);
        }
        ExitProcess(VNC_EXIT_GDI_WEDGED);
    }
    return vnc_job_result;
}

/* ------------------------------------------------------------------ session */

static void vnc_serve(DWORD poll_ms)
{
    DWORD last_capture = 0ul;
    int ready;
    LONG captured;
    HANDLE session;
    vnc_default_format();
    vnc_use_hextile = 0;
    vnc_use_desktop_size = 0;
    vnc_use_pointer_pos = 0;
    vnc_rich_cursor = 0l;
    vnc_cursor_sent_serial = vnc_cursor_serial;
    vnc_pos_sent.x = -1l;
    vnc_pos_sent.y = -1l;
    vnc_use_copyrect = 0;
    vnc_use_zrle = 0;
    vnc_zstream_started = 0;
    vnc_copy_pending = 0;
    vnc_last_foreground = 0;
    vnc_clip_recv_ready = 0;
    vnc_clip_reset = 1l;
    vnc_clip_out_ready = 0l;
    vnc_clip_session = 1l;
    vnc_update_wanted = 0;
    vnc_full_wanted = 1;
    vnc_out_length = 0ul;
    vnc_buttons = 0u;
    vnc_pointer_x = -1;
    vnc_pointer_y = -1;
    vnc_zero(vnc_keys_down, sizeof(vnc_keys_down));
    if (!vnc_job_run(VNC_JOB_INIT)) return;
    if (!vnc_handshake()) {
        vnc_log("handshake-failed", 0ul);
        return;
    }
    vnc_log("session-start", 0ul);
    session = CreateEventA(0, TRUE, TRUE, vnc_session_name);
    vnc_perf_reset();
    while (!vnc_stop_requested()) {
        DWORD wait = 500ul;
        if (GetTickCount() - vnc_perf_logged >= 60000ul) vnc_perf_report();
        if (vnc_job_pending) {
            wait = 10ul;
        } else if (vnc_update_wanted) {
            DWORD elapsed = GetTickCount() - last_capture;
            wait = elapsed >= poll_ms ? 0ul : poll_ms - elapsed;
        }
        ready = vnc_wait_session(wait);
        if (ready < 0) break;
        if ((ready & 2) != 0) vnc_turn_away();
        if ((ready & 1) != 0 && !vnc_handle_message()) break;
        vnc_clip_hand_over();
        if (vnc_clip_out_ready) {
            if (!vnc_send_cut_text()) break;
            vnc_clip_out_ready = 0l;
        }

        if (vnc_job_pending) {
            if (!vnc_job_poll(0ul)) {
                if (!vnc_stalled && GetTickCount() - vnc_job_started > VNC_STALL_MS) {
                    vnc_stalled = 1;
                    vnc_log("capture-stalled", VNC_STALL_MS);
                }
                continue;
            }
            captured = vnc_job_result;
            if (captured < 0) {
                if (!vnc_use_desktop_size) {
                    vnc_log("mode-change-disconnect", 0ul);
                    break;
                }
                if (!vnc_job_run(VNC_JOB_INIT)) break;
                /* DesktopSize answers the outstanding request; the viewer
                   asks again at the new size and gets a full frame. */
                if (!vnc_send_desktop_size()) break;
                vnc_update_wanted = 0;
                vnc_full_wanted = 1;
                continue;
            }
            if (captured == 0l || !vnc_update_wanted) continue;
            if (vnc_full_wanted) {
                vnc_last_foreground = vnc_foreground;
                vnc_last_foreground_rect = vnc_foreground_rect;
                vnc_copy_pending = 0;
            } else {
                vnc_detect_move();
            }
            {
                DWORD started = vnc_ticks();
                unsigned long dirty = vnc_diff(vnc_full_wanted);
                DWORD send_before;
                vnc_perf_diff += vnc_ticks() - started;
                if (dirty == 0ul && !vnc_copy_pending &&
                    !vnc_cursor_shape_due() && !vnc_cursor_pos_due()) continue;
                vnc_full_wanted = 0;
                started = vnc_ticks();
                send_before = vnc_perf_send;
                if (!vnc_send_update()) break;
                vnc_perf_encode += (vnc_ticks() - started) - (vnc_perf_send - send_before);
                ++vnc_perf_updates;
            }
            vnc_update_wanted = 0;
            continue;
        }
        /* Capture on schedule even while input keeps arriving. */
        if (!vnc_update_wanted || GetTickCount() - last_capture < poll_ms) continue;
        last_capture = GetTickCount();
        if (vnc_full_wanted) vnc_capture_full = 1l; /* a full frame needs a full read */
        vnc_job_start(VNC_JOB_CAPTURE);
    }
    vnc_clip_session = 0l;
    vnc_release_all();
    vnc_perf_report();
    if (session != 0) CloseHandle(session);
    vnc_log("session-end", 0ul);
}

/* ------------------------------------------------------------------- main */

static const char *vnc_skip_spaces(const char *text)
{
    while (*text == ' ' || *text == '\t') ++text;
    return text;
}

/* Skip the program name (quoted or not) in GetCommandLineA's result. */
static const char *vnc_arguments(void)
{
    const char *text = GetCommandLineA();
    if (*text == '"') {
        ++text;
        while (*text != '\0' && *text != '"') ++text;
        if (*text == '"') ++text;
    } else {
        while (*text != '\0' && *text != ' ' && *text != '\t') ++text;
    }
    return vnc_skip_spaces(text);
}

static int vnc_word(const char *text, const char *word)
{
    while (*word != '\0') {
        char c = *text++;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != *word++) return 0;
    }
    return *text == '\0' || *text == ' ' || *text == '\t';
}

/* The server proper: config, worker/clipboard threads, listener, accept
   loop. Shared by the console/9x path and the NT service, so it takes its
   port override through a global rather than the command line. Returns the
   process exit code. */
static UINT vnc_port_override;

DWORD vnc_service_main(void)
{
    char bind_address[32];
    char allowed_text[32];
    unsigned long allowed = 0ul;
    int restrict_client;
    UINT port;
    UINT poll_ms;
    HANDLE mutex;
    WSADATA data;
    SOCKET listener;
    struct sockaddr_in address;
    int address_length;
    int yes = 1;

    /* Off unless the machine's owner opted in (SETUP.EXE's VNC option). */
    if (GetPrivateProfileIntA("vnc", "enabled", 0, vnc_config_path) != 1) {
        vnc_log("disabled", 0ul);
        return VNC_EXIT_DISABLED;
    }
    port = vnc_port_override != 0u ? vnc_port_override :
           GetPrivateProfileIntA("vnc", "port", 5900, vnc_config_path);
    if (port == 0u || port > 65535u) return 2ul;
    (void)GetPrivateProfileStringA("vnc", "password", "", vnc_password,
                                   sizeof(vnc_password), vnc_config_path);
    vnc_log(vnc_password[0] != '\0' ? "auth-password" : "auth-none", 0ul);
    poll_ms = GetPrivateProfileIntA("vnc", "poll_ms", 100, vnc_config_path);
    if (poll_ms < 20u) poll_ms = 20u;
    if (poll_ms > 2000u) poll_ms = 2000u;
    (void)GetPrivateProfileStringA("agent", "bind_address", "0.0.0.0", bind_address,
                                   sizeof(bind_address), vnc_config_path);
    (void)GetPrivateProfileStringA("agent", "allowed_client", "", allowed_text,
                                   sizeof(allowed_text), vnc_config_path);

    mutex = CreateMutexA(0, FALSE, vnc_mutex_name);
    if (mutex == 0 || GetLastError() == ERROR_ALREADY_EXISTS) return 3ul;
    vnc_stop_event = CreateEventA(0, TRUE, FALSE, vnc_stop_name);
    if (vnc_stop_event == 0) return 4ul;
    {
        LARGE_INTEGER frequency;
        if (QueryPerformanceFrequency(&frequency) && frequency.HighPart == 0l) {
            vnc_perf_frequency_ms = frequency.LowPart / 1000ul;
        }
    }
    vnc_get_cursor_info = (VncGetCursorInfo)GetProcAddress(
        GetModuleHandleA("USER32.DLL"), "GetCursorInfo");

    vnc_dirty = (unsigned char *)GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT, VNC_MAX_TILES);
    vnc_read_mask = (unsigned char *)GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT, VNC_MAX_TILES);
    vnc_suspect = (unsigned char *)GlobalAlloc(GMEM_FIXED, VNC_MAX_TILES);
    if (vnc_read_mask == 0 || vnc_suspect == 0) return 4ul;
    vnc_out = (unsigned char *)GlobalAlloc(GMEM_FIXED, VNC_OUT_SIZE);
    vnc_cursor_rgb = (DWORD *)GlobalAlloc(GMEM_FIXED, VNC_CURSOR_MAX * VNC_CURSOR_MAX * 4);
    vnc_cursor_black = (DWORD *)GlobalAlloc(GMEM_FIXED, VNC_CURSOR_MAX * VNC_CURSOR_MAX * 4);
    vnc_cursor_mask = (unsigned char *)GlobalAlloc(GMEM_FIXED, VNC_CURSOR_MAX * VNC_CURSOR_MAX);
    vnc_clip_recv = (unsigned char *)GlobalAlloc(GMEM_FIXED, VNC_CLIP_MAX);
    vnc_clip_in = (unsigned char *)GlobalAlloc(GMEM_FIXED, VNC_CLIP_MAX);
    vnc_clip_out = (unsigned char *)GlobalAlloc(GMEM_FIXED, VNC_CLIP_MAX);
    if (vnc_dirty == 0 || vnc_out == 0 || vnc_cursor_rgb == 0 ||
        vnc_cursor_black == 0 || vnc_cursor_mask == 0 || vnc_clip_recv == 0 ||
        vnc_clip_in == 0 || vnc_clip_out == 0) return 4ul;
    vnc_job_event = CreateEventA(0, FALSE, FALSE, 0);
    vnc_done_event = CreateEventA(0, FALSE, FALSE, 0);
    if (vnc_job_event == 0 || vnc_done_event == 0) return 4ul;
    {
        DWORD thread_id;
        HANDLE worker = CreateThread(0, 0ul, vnc_worker, 0, 0ul, &thread_id);
        HANDLE clipboard;
        if (worker == 0) return 4ul;
        CloseHandle(worker);
        vnc_clip_event = CreateEventA(0, FALSE, FALSE, 0);
        if (vnc_clip_event == 0) return 4ul;
        clipboard = CreateThread(0, 0ul, vnc_clip_thread, 0, 0ul, &thread_id);
        if (clipboard == 0) return 4ul;
        CloseHandle(clipboard);
    }
    if (WSAStartup(MAKEWORD(1, 1), &data) != 0) return 5ul;
    restrict_client = allowed_text[0] != '\0';
    if (restrict_client) {
        allowed = inet_addr(allowed_text);
        if (allowed == INADDR_NONE) {
            vnc_log("allowed-client-invalid", 0ul);
            return 6ul;
        }
    }
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) return 7ul;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes,
                     sizeof(yes));
    vnc_zero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((u_short)port);
    address.sin_addr.s_addr = inet_addr(bind_address);
    if (address.sin_addr.s_addr == INADDR_NONE ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR) {
        vnc_log("listen-failed", (DWORD)WSAGetLastError());
        return 8ul;
    }
    vnc_log("listening", (DWORD)port);
    vnc_listener = listener;
    vnc_restrict_client = restrict_client;
    vnc_allowed_client = allowed;

    while (!vnc_stop_requested() && !vnc_service_stopping()) {
        fd_set readable;
        struct timeval wait;
        SOCKET client;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        wait.tv_sec = 0l;
        wait.tv_usec = 500000l;
        if (select(0, &readable, 0, 0, &wait) <= 0) continue;
        address_length = sizeof(address);
        client = accept(listener, (struct sockaddr *)&address, &address_length);
        if (client == INVALID_SOCKET) continue;
        if (restrict_client && address.sin_addr.s_addr != allowed) {
            vnc_log("client-rejected", 0ul);
            closesocket(client);
            continue;
        }
        (void)setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes,
                         sizeof(yes));
        vnc_socket = client;
        vnc_serve((DWORD)poll_ms);
        closesocket(client);
        vnc_socket = INVALID_SOCKET;
    }
    closesocket(listener);
    (void)vnc_job_run(VNC_JOB_FREE);
    vnc_log("stopped", 0ul);
    return 0ul;
}

/* This module's own path (vnc.c has no access to GetModuleFileName result
   elsewhere): used for the service command line. */
static void vnc_module_path(char *path)
{
    DWORD length = GetModuleFileNameA(0, path, MAX_PATH);
    if (length == 0ul || length >= (DWORD)MAX_PATH) path[0] = '\0';
}

static DWORD vnc_main(void)
{
    const char *arguments = vnc_arguments();
    char module[MAX_PATH];

    if (vnc_word(arguments, "-status")) {
        HANDLE stop = OpenEventA(EVENT_MODIFY_STATE, FALSE, vnc_stop_name);
        /* The service's event exists but is not ours to open: that is a
           running server too. Without this, -status said "not running" for
           every server started as the Windows 2000 service. */
        if (stop == 0) return GetLastError() == ERROR_ACCESS_DENIED ? 0ul : 1ul;
        CloseHandle(stop);
        return 0ul;
    }
    if (vnc_word(arguments, "-stop")) {
        int service = v9x_service_control(0);
        /* Then the event too, for a console instance started by hand. */
        HANDLE stop = OpenEventA(EVENT_MODIFY_STATE, FALSE, vnc_stop_name);
        if (stop != 0) {
            (void)SetEvent(stop);
            CloseHandle(stop);
            return 0ul;
        }
        return service >= 0 ? (DWORD)service : 1ul;
    }
    if (vnc_word(arguments, "-install")) {
        vnc_module_path(module);
        return (DWORD)v9x_service_install(module);
    }
    if (vnc_word(arguments, "-uninstall")) {
        return (DWORD)v9x_service_uninstall();
    }
    if (vnc_word(arguments, "-port")) {
        const char *digits = vnc_skip_spaces(arguments + 5);
        vnc_port_override = 0u;
        while (*digits >= '0' && *digits <= '9' && vnc_port_override < 65536u) {
            vnc_port_override = vnc_port_override * 10u + (UINT)(*digits++ - '0');
        }
    }
    if (vnc_word(arguments, "-service")) {
        /* Hand off to the SCM. If this was not launched as a service (run by
           hand), v9x_service_run returns non-zero and we run on the console. */
        if (v9x_service_run() == 0) return vnc_service_exit_code();
    } else if (!vnc_word(arguments, "-port")) {
        /* A plain start on 2000 where the service is installed (v9xctl vnc
           start, update's restart) starts the service, not a second,
           user-session server that could not reach the logon desktop. */
        int service = v9x_service_control(1);
        if (service >= 0) return (DWORD)service;
    }
    return vnc_service_main();
}

void WINAPI V9xVncEntry(void)
{
    (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                       SEM_NOOPENFILEERRORBOX);
    ExitProcess(vnc_main());
}
