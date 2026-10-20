#include "agent.h"
#include "platform_win.h"
#include "http.h"
#include "v9xremote/crc32.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"

#define V9X_DL_MAX_URL 1024ul
#define V9X_DL_MAX_DEST 230ul
#define V9X_DL_HEADER_MAX 8192ul
#define V9X_DL_RECV_CHUNK 2048ul
#define V9X_DL_PROGRESS_STEP 65536ul
/* A server silent for this long ends the download. */
#define V9X_DL_TIMEOUT_MS 60000ul

static unsigned long v9x_dl_num(char *target, unsigned long offset,
                                unsigned long value)
{
    char reverse[10];
    unsigned long count = 0ul;
    unsigned long index;
    do {
        reverse[count++] = (char)('0' + (value % 10ul));
        value /= 10ul;
    } while (value != 0ul && count < sizeof(reverse));
    for (index = 0ul; index < count; ++index) {
        target[offset + index] = reverse[count - index - 1ul];
    }
    return offset + count;
}

static void v9x_dl_log_complete(const char *dest, unsigned long size,
                                unsigned long crc, unsigned long status)
{
    char detail[300];
    unsigned long at = 0ul;
    unsigned long index = 0ul;
    while (dest[index] != '\0' && index < 200ul) {
        unsigned char ch = (unsigned char)dest[index];
        if (ch < 0x20u || ch > 0x7eu) ch = '?';
        detail[at++] = (char)ch;
        ++index;
    }
    detail[at++] = ' ';
    detail[at++] = 's'; detail[at++] = 't'; detail[at++] = '=';
    at = v9x_dl_num(detail, at, status);
    detail[at++] = ' ';
    detail[at++] = 's'; detail[at++] = 'z'; detail[at++] = '=';
    at = v9x_dl_num(detail, at, size);
    detail[at++] = ' ';
    detail[at++] = 'c'; detail[at++] = 'r'; detail[at++] = 'c'; detail[at++] = '=';
    at = v9x_dl_num(detail, at, crc);
    detail[at] = '\0';
    v9x_log_event("download-complete", detail);
}

static int v9x_dl_error(V9xConnection *conn, DWORD request_id, DWORD status,
                        DWORD native_error, const char *detail)
{
    unsigned long offset = 8ul;
    v9x_write_u32(conn->file_response, status);
    v9x_write_u32(conn->file_response + 4, native_error);
    if (!v9x_append_string(conn->file_response, sizeof(conn->file_response),
                           &offset, detail)) return 0;
    return v9x_send_frame(conn, V9X_MSG_ERROR_RESPONSE, request_id,
                          conn->file_response, offset);
}

static int v9x_dl_send_request(SOCKET handle, const char *host,
                               const char *path)
{
    char request[V9X_HTTP_MAX_PATH + 512ul];
    unsigned long offset = 0ul;
    const char *parts[5];
    unsigned long part;
    parts[0] = "GET ";
    parts[1] = path;
    parts[2] = " HTTP/1.0\r\nHost: ";
    parts[3] = host;
    parts[4] = "\r\nUser-Agent: v9x-agent\r\nConnection: close\r\n\r\n";
    for (part = 0ul; part < 5ul; ++part) {
        const char *text = parts[part];
        while (*text != '\0') {
            if (offset + 1ul >= sizeof(request)) return 0;
            request[offset++] = *text++;
        }
    }
    return v9x_http_send_all(handle, request, offset, V9X_DL_TIMEOUT_MS);
}

int v9x_handle_download(V9xConnection *conn, unsigned long request_id,
                        const unsigned char *payload, unsigned long length)
{
    char url[V9X_DL_MAX_URL + 1ul];
    char host[V9X_HTTP_MAX_HOST];
    char path[V9X_HTTP_MAX_PATH];
    char dest[260];
    char temp[260];
    unsigned char header[V9X_DL_HEADER_MAX];
    unsigned char chunk[V9X_DL_RECV_CHUNK];
    unsigned long offset = 0ul;
    unsigned long url_length;
    unsigned long dest_length;
    unsigned long header_length = 0ul;
    unsigned long body_start;
    unsigned long body_total = 0ul;
    unsigned long next_progress = V9X_DL_PROGRESS_STEP;
    unsigned long index;
    unsigned short port = 80u;
    unsigned long http_status;
    unsigned long running_crc;
    unsigned long actual_crc;
    unsigned long promote_status;
    DWORD native_error = 0ul;
    DWORD written;
    SOCKET handle;
    HANDLE file;
    int is_https = 0;
    int received;
    int failed = 0;

    if (length < 4ul) {
        return v9x_dl_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                            ERROR_INVALID_PARAMETER, "download payload too short");
    }
    url_length = (unsigned long)v9x_read_u16(payload);
    if (url_length == 0ul || url_length > V9X_DL_MAX_URL ||
        2ul + url_length + 2ul > length) {
        return v9x_dl_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                            ERROR_INVALID_PARAMETER, "invalid download url");
    }
    for (index = 0ul; index < url_length; ++index) {
        unsigned char ch = payload[2ul + index];
        if (ch <= 0x20u || ch > 0x7eu) {
            return v9x_dl_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                                ERROR_INVALID_PARAMETER, "invalid download url");
        }
        url[index] = (char)ch;
    }
    url[url_length] = '\0';
    offset = 2ul + url_length;
    dest_length = (unsigned long)v9x_read_u16(payload + offset);
    offset += 2ul;
    if (dest_length == 0ul || dest_length > V9X_DL_MAX_DEST ||
        offset + dest_length != length) {
        return v9x_dl_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                            ERROR_INVALID_PARAMETER, "invalid download dest");
    }
    for (index = 0ul; index < dest_length; ++index) {
        unsigned char ch = payload[offset + index];
        if (ch == 0u || ch > 0x7fu) {
            return v9x_dl_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                                ERROR_INVALID_PARAMETER, "invalid download dest");
        }
        dest[index] = (char)ch;
    }
    dest[dest_length] = '\0';

    if (!v9x_http_parse_url(url, host, &port, path, &is_https)) {
        return v9x_dl_error(conn, request_id,
                            is_https ? V9X_STATUS_UNSUPPORTED_OPERATION :
                                       V9X_STATUS_INVALID_PAYLOAD,
                            ERROR_INVALID_PARAMETER,
                            is_https ? "https is not supported (http only)" :
                                       "malformed http url");
    }

    /* temp = dest + "." + slot index + ".DLPART" (unique per connection). */
    offset = 0ul;
    for (index = 0ul; index < dest_length; ++index) temp[offset++] = dest[index];
    temp[offset++] = '.';
    {
        unsigned long slot = (unsigned long)conn->index;
        char digits[10];
        unsigned long count = 0ul;
        do { digits[count++] = (char)('0' + (slot % 10ul)); slot /= 10ul; }
        while (slot != 0ul && count < sizeof(digits));
        while (count != 0ul) temp[offset++] = digits[--count];
    }
    temp[offset++] = '.'; temp[offset++] = 'D'; temp[offset++] = 'L';
    temp[offset++] = 'P'; temp[offset++] = 'A'; temp[offset++] = 'R';
    temp[offset++] = 'T'; temp[offset] = '\0';

    handle = v9x_http_connect(host, port, V9X_DL_TIMEOUT_MS, &native_error);
    if (handle == INVALID_SOCKET) {
        return v9x_dl_error(conn, request_id, V9X_STATUS_IO_FAILED,
                            native_error, "download connect failed");
    }
    if (!v9x_dl_send_request(handle, host, path)) {
        closesocket(handle);
        return v9x_dl_error(conn, request_id, V9X_STATUS_IO_FAILED,
                            WSAGetLastError(), "download request send failed");
    }

    /* Accumulate the response header until the CRLF CRLF terminator. */
    body_start = 0ul;
    while (header_length < sizeof(header)) {
        received = v9x_http_recv(handle, header + header_length,
                                 sizeof(header) - header_length,
                                 V9X_DL_TIMEOUT_MS);
        if (received <= 0) break;
        header_length += (unsigned long)received;
        body_start = v9x_http_find_body(header, header_length);
        if (body_start != 0ul) break;
    }
    if (body_start == 0ul) {
        closesocket(handle);
        return v9x_dl_error(conn, request_id, V9X_STATUS_IO_FAILED,
                            ERROR_INVALID_DATA, "download response header invalid");
    }
    http_status = v9x_http_status_code(header, header_length);
    if (http_status != 200ul) {
        closesocket(handle);
        return v9x_dl_error(conn, request_id, V9X_STATUS_IO_FAILED, http_status,
                            "download http status not 200");
    }

    file = CreateFileA(temp, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        native_error = GetLastError();
        closesocket(handle);
        return v9x_dl_error(conn, request_id, V9X_STATUS_IO_FAILED,
                            native_error, "download temp create failed");
    }

    v9x_log_event("download-start", url);
    running_crc = v9x_crc32_begin();

    /* Bytes already read past the header terminator are the first body bytes. */
    if (header_length > body_start) {
        unsigned long initial = header_length - body_start;
        body_total = initial;
        running_crc = v9x_crc32_update(running_crc, header + body_start, initial);
        if (body_total > V9X_MAX_DOWNLOAD_SIZE ||
            !WriteFile(file, header + body_start, initial, &written, 0) ||
            written != initial) {
            native_error = GetLastError();
            failed = 1;
        }
    }

    while (!failed) {
        received = v9x_http_recv(handle, chunk, sizeof(chunk),
                                 V9X_DL_TIMEOUT_MS);
        if (received < 0) {
            native_error = WSAETIMEDOUT;
            failed = 1;
            break;
        }
        if (received == 0) break;
        if (body_total + (unsigned long)received > V9X_MAX_DOWNLOAD_SIZE) {
            native_error = ERROR_FILE_TOO_LARGE;
            failed = 1;
            break;
        }
        running_crc = v9x_crc32_update(running_crc, chunk, (unsigned long)received);
        if (!WriteFile(file, chunk, (DWORD)received, &written, 0) ||
            written != (DWORD)received) {
            native_error = GetLastError();
            failed = 1;
            break;
        }
        body_total += (unsigned long)received;
        if (body_total >= next_progress) {
            v9x_write_u32(conn->response, body_total);
            (void)v9x_send_frame(conn, V9X_MSG_DOWNLOAD_PROGRESS, request_id,
                                 conn->response, 4ul);
            next_progress += V9X_DL_PROGRESS_STEP;
        }
    }
    closesocket(handle);
    if (!FlushFileBuffers(file) && !failed) {
        native_error = GetLastError();
        failed = 1;
    }
    CloseHandle(file);
    if (failed) {
        (void)DeleteFileA(temp);
        return v9x_dl_error(conn, request_id,
                            native_error == ERROR_FILE_TOO_LARGE ?
                                V9X_STATUS_LIMIT_EXCEEDED : V9X_STATUS_IO_FAILED,
                            native_error, "download body transfer failed");
    }

    actual_crc = v9x_crc32_end(running_crc);
    promote_status = v9x_promote_temp(dest, temp, &native_error);
    if (promote_status != V9X_STATUS_OK) {
        return v9x_dl_error(conn, request_id, promote_status, native_error,
                            "download final rename failed");
    }
    v9x_dl_log_complete(dest, body_total, actual_crc, http_status);
    v9x_write_u32(conn->file_response, http_status);
    v9x_write_u32(conn->file_response + 4, body_total);
    v9x_write_u32(conn->file_response + 8, actual_crc);
    return v9x_send_frame(conn, V9X_MSG_DOWNLOAD_COMPLETE, request_id,
                          conn->file_response, 12ul);
}
