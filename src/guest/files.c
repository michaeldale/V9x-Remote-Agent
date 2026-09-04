#include "agent.h"
#include "v9xremote/crc32.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"

#define V9X_FILE_PATH_MAX 230ul

static void v9x_file_zero(void *target, unsigned long length)
{
    unsigned char *bytes = (unsigned char *)target;
    unsigned long index;
    for (index = 0ul; index < length; ++index) bytes[index] = 0u;
}

static unsigned long v9x_file_status(DWORD native_error)
{
    if (native_error == ERROR_FILE_NOT_FOUND || native_error == ERROR_PATH_NOT_FOUND) {
        return V9X_STATUS_NOT_FOUND;
    }
    if (native_error == ERROR_ACCESS_DENIED || native_error == ERROR_SHARING_VIOLATION) {
        return V9X_STATUS_ACCESS_DENIED;
    }
    return V9X_STATUS_IO_FAILED;
}

static int v9x_file_error(V9xConnection *conn, DWORD request_id,
                          DWORD status, DWORD native_error, const char *detail)
{
    unsigned long offset = 8ul;
    v9x_write_u32(conn->file_response, status);
    v9x_write_u32(conn->file_response + 4, native_error);
    if (!v9x_append_string(conn->file_response, sizeof(conn->file_response),
                           &offset, detail)) return 0;
    return v9x_send_frame(conn, V9X_MSG_ERROR_RESPONSE, request_id,
                          conn->file_response, offset);
}

static int v9x_read_path(const unsigned char *payload, unsigned long length,
                         unsigned long offset, char *path,
                         unsigned long maximum)
{
    unsigned long path_length;
    unsigned long index;
    if (offset > length || length - offset < 2ul) return 0;
    path_length = (unsigned long)v9x_read_u16(payload + offset);
    offset += 2ul;
    if (path_length == 0ul || path_length > maximum ||
        path_length != length - offset) return 0;
    for (index = 0ul; index < path_length; ++index) {
        if (payload[offset + index] == 0u || payload[offset + index] > 0x7fu) {
            return 0;
        }
        path[index] = (char)payload[offset + index];
    }
    path[path_length] = '\0';
    return 1;
}

static unsigned long v9x_append_number(char *target, unsigned long offset,
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

static unsigned long v9x_file_text(char *target, unsigned long offset,
                                   const char *text, unsigned long maximum)
{
    unsigned long index = 0ul;
    while (text[index] != '\0' && index < maximum) {
        unsigned char ch = (unsigned char)text[index];
        if (ch < 0x20u || ch > 0x7eu) ch = '?';
        target[offset++] = (char)ch;
        ++index;
    }
    return offset;
}

static void v9x_file_log(const char *event, const char *path,
                         int with_totals, DWORD size, DWORD crc)
{
    char detail[300];
    unsigned long at = 0ul;
    at = v9x_file_text(detail, at, path, 200ul);
    if (with_totals) {
        detail[at++] = ' ';
        detail[at++] = 's'; detail[at++] = 'z'; detail[at++] = '=';
        at = v9x_append_number(detail, at, size);
        detail[at++] = ' ';
        detail[at++] = 'c'; detail[at++] = 'r'; detail[at++] = 'c'; detail[at++] = '=';
        at = v9x_append_number(detail, at, crc);
    }
    detail[at] = '\0';
    v9x_log_event(event, detail);
}

/* The `.PART` staging name carries both the owning connection index and the
   request id so two connections uploading to the same destination cannot pick
   the same temp path (request ids are unique only per connection). */
static int v9x_make_part_path(char *target, const char *path, DWORD slot_index,
                              DWORD request_id)
{
    unsigned long length = v9x_bounded_length(path, V9X_FILE_PATH_MAX + 1ul);
    unsigned long index;
    if (length > V9X_FILE_PATH_MAX) return 0;
    for (index = 0ul; index < length; ++index) target[index] = path[index];
    target[length++] = '.';
    length = v9x_append_number(target, length, slot_index);
    target[length++] = '.';
    length = v9x_append_number(target, length, request_id);
    target[length++] = '.';
    target[length++] = 'P'; target[length++] = 'A';
    target[length++] = 'R'; target[length++] = 'T';
    target[length] = '\0';
    return 1;
}

static int v9x_make_backup_path(char *target, const char *path)
{
    unsigned long length = v9x_bounded_length(path, V9X_FILE_PATH_MAX + 1ul);
    unsigned long index;
    static const char suffix[] = ".V9X.BAK";
    if (length > V9X_FILE_PATH_MAX) return 0;
    for (index = 0ul; index < length; ++index) target[index] = path[index];
    for (index = 0ul; index < sizeof(suffix); ++index) {
        target[length + index] = suffix[index];
    }
    return 1;
}

/* Promote a fully written, CRC-verified temp file to its final path, backing up
   any existing destination first so a failure rolls back cleanly. Returns a
   V9X_STATUS_* code (V9X_STATUS_OK on success) and sets *native_error. The temp
   file is always consumed (moved or deleted). Shared by upload-commit and the
   URL-download handler in download.c. */
unsigned long v9x_promote_temp(const char *final_path, const char *temp_path,
                               DWORD *native_error)
{
    DWORD attributes;
    char backup_path[260];
    int backed_up = 0;
    *native_error = 0ul;
    attributes = GetFileAttributesA(final_path);
    if (attributes != 0xfffffffful) {
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0ul ||
            !v9x_make_backup_path(backup_path, final_path)) {
            (void)DeleteFileA(temp_path);
            *native_error = ERROR_ACCESS_DENIED;
            return V9X_STATUS_ACCESS_DENIED;
        }
        (void)DeleteFileA(backup_path);
        if (!MoveFileA(final_path, backup_path)) {
            *native_error = GetLastError();
            (void)DeleteFileA(temp_path);
            return v9x_file_status(*native_error);
        }
        backed_up = 1;
    }
    if (!MoveFileA(temp_path, final_path)) {
        *native_error = GetLastError();
        if (backed_up) (void)MoveFileA(backup_path, final_path);
        (void)DeleteFileA(temp_path);
        return V9X_STATUS_IO_FAILED;
    }
    if (backed_up) (void)DeleteFileA(backup_path);
    return V9X_STATUS_OK;
}

static int v9x_handle_stat(V9xConnection *conn, DWORD request_id,
                           const unsigned char *payload, DWORD length)
{
    char path[260];
    DWORD attributes;
    DWORD native_error = 0ul;
    DWORD size = 0ul;
    DWORD high = 0ul;
    HANDLE file;
    if (!v9x_read_path(payload, length, 0ul, path, 259ul)) {
        return v9x_file_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              ERROR_INVALID_PARAMETER, "invalid stat path");
    }
    attributes = GetFileAttributesA(path);
    if (attributes == 0xfffffffful) {
        native_error = GetLastError();
        if (native_error != ERROR_FILE_NOT_FOUND &&
            native_error != ERROR_PATH_NOT_FOUND) {
            return v9x_file_error(conn, request_id,
                                  v9x_file_status(native_error), native_error,
                                  "stat attributes failed");
        }
        conn->file_response[0] = 0u;
        conn->file_response[1] = 0u;
        v9x_write_u16(conn->file_response + 2, 0u);
        v9x_write_u32(conn->file_response + 4, 0ul);
        v9x_write_u32(conn->file_response + 8, 0ul);
        v9x_write_u32(conn->file_response + 12, native_error);
        return v9x_send_frame(conn, V9X_MSG_FILE_STAT_RESPONSE, request_id,
                              conn->file_response, 16ul);
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0ul) {
        file = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
        if (file == INVALID_HANDLE_VALUE) {
            native_error = GetLastError();
            return v9x_file_error(conn, request_id,
                                  v9x_file_status(native_error), native_error,
                                  "stat open failed");
        }
        size = GetFileSize(file, &high);
        native_error = GetLastError();
        CloseHandle(file);
        if (size == 0xfffffffful) {
            return v9x_file_error(conn, request_id, V9X_STATUS_IO_FAILED,
                                  native_error, "stat size failed");
        }
        if (high != 0ul) {
            return v9x_file_error(conn, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                                  ERROR_FILE_TOO_LARGE, "file exceeds v1 size");
        }
    }
    conn->file_response[0] = 1u;
    conn->file_response[1] =
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0ul ? 1u : 0u;
    v9x_write_u16(conn->file_response + 2, 0u);
    v9x_write_u32(conn->file_response + 4, size);
    v9x_write_u32(conn->file_response + 8, attributes);
    v9x_write_u32(conn->file_response + 12, 0ul);
    return v9x_send_frame(conn, V9X_MSG_FILE_STAT_RESPONSE, request_id,
                          conn->file_response, 16ul);
}

static int v9x_handle_list(V9xConnection *conn, DWORD request_id,
                           const unsigned char *payload, DWORD length)
{
    char path[260];
    char pattern[264];
    WIN32_FIND_DATAA data;
    HANDLE search;
    DWORD native_error;
    unsigned long path_length;
    unsigned long offset = 4ul;
    unsigned long count = 0ul;
    unsigned long index;
    unsigned long name_length;
    if (!v9x_read_path(payload, length, 0ul, path, 255ul)) {
        return v9x_file_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              ERROR_INVALID_PARAMETER, "invalid list path");
    }
    path_length = v9x_bounded_length(path, 256ul);
    for (index = 0ul; index < path_length; ++index) pattern[index] = path[index];
    if (path_length != 0ul && pattern[path_length - 1ul] != '\\' &&
        pattern[path_length - 1ul] != '/') pattern[path_length++] = '\\';
    pattern[path_length++] = '*'; pattern[path_length] = '\0';
    search = FindFirstFileA(pattern, &data);
    if (search == INVALID_HANDLE_VALUE) {
        native_error = GetLastError();
        return v9x_file_error(conn, request_id, v9x_file_status(native_error),
                              native_error, "directory list failed");
    }
    do {
        if ((data.cFileName[0] == '.' && data.cFileName[1] == '\0') ||
            (data.cFileName[0] == '.' && data.cFileName[1] == '.' &&
             data.cFileName[2] == '\0')) continue;
        if (data.nFileSizeHigh != 0ul) {
            FindClose(search);
            return v9x_file_error(conn, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                                  ERROR_FILE_TOO_LARGE, "listed file too large");
        }
        name_length = v9x_bounded_length(data.cFileName, MAX_PATH);
        if (name_length >= MAX_PATH || offset + 10ul + name_length >
                                      sizeof(conn->file_response)) {
            FindClose(search);
            return v9x_file_error(conn, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                                  ERROR_INSUFFICIENT_BUFFER,
                                  "directory listing too large");
        }
        for (index = 0ul; index < name_length; ++index) {
            if ((unsigned char)data.cFileName[index] > 0x7fu) {
                FindClose(search);
                return v9x_file_error(conn, request_id,
                                      V9X_STATUS_INVALID_PAYLOAD,
                                      ERROR_INVALID_NAME,
                                      "non-ASCII directory entry");
            }
        }
        v9x_write_u32(conn->file_response + offset, data.dwFileAttributes);
        v9x_write_u32(conn->file_response + offset + 4, data.nFileSizeLow);
        offset += 8ul;
        if (!v9x_append_string(conn->file_response, sizeof(conn->file_response),
                               &offset, data.cFileName)) {
            FindClose(search);
            return v9x_file_error(conn, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                                  ERROR_INSUFFICIENT_BUFFER,
                                  "directory listing overflow");
        }
        ++count;
    } while (FindNextFileA(search, &data));
    native_error = GetLastError();
    FindClose(search);
    if (native_error != ERROR_NO_MORE_FILES) {
        return v9x_file_error(conn, request_id, V9X_STATUS_IO_FAILED,
                              native_error, "directory enumeration failed");
    }
    v9x_write_u32(conn->file_response, count);
    return v9x_send_frame(conn, V9X_MSG_FILE_LIST_RESPONSE, request_id,
                          conn->file_response, offset);
}

static int v9x_handle_mkdir(V9xConnection *conn, DWORD request_id,
                            const unsigned char *payload, DWORD length)
{
    char path[260];
    DWORD native_error;
    DWORD attributes;
    DWORD created = 1ul;
    if (!v9x_read_path(payload, length, 0ul, path, 259ul)) {
        return v9x_file_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              ERROR_INVALID_PARAMETER, "invalid mkdir path");
    }
    if (!CreateDirectoryA(path, 0)) {
        native_error = GetLastError();
        attributes = GetFileAttributesA(path);
        if (native_error != ERROR_ALREADY_EXISTS || attributes == 0xfffffffful ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0ul) {
            return v9x_file_error(conn, request_id,
                                  v9x_file_status(native_error), native_error,
                                  "mkdir failed");
        }
        created = 0ul;
    }
    v9x_write_u32(conn->file_response, created);
    return v9x_send_frame(conn, V9X_MSG_FILE_MKDIR_RESPONSE, request_id,
                          conn->file_response, 4ul);
}

static int v9x_handle_open_write(V9xConnection *conn, DWORD request_id,
                                 const unsigned char *payload, DWORD length)
{
    V9xWriteState *write_state = &conn->write_state;
    DWORD native_error;
    if (write_state->handle != 0) {
        return v9x_file_error(conn, request_id, V9X_STATUS_BUSY, 0ul,
                              "upload already active");
    }
    if (length < 10ul ||
        !v9x_read_path(payload, length, 8ul, write_state->final_path,
                       V9X_FILE_PATH_MAX)) {
        return v9x_file_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              ERROR_INVALID_PARAMETER, "invalid upload request");
    }
    write_state->expected_size = v9x_read_u32(payload);
    write_state->expected_crc = v9x_read_u32(payload + 4);
    if (write_state->expected_size > V9X_MAX_FILE_SIZE ||
        !v9x_make_part_path(write_state->temp_path, write_state->final_path,
                            (DWORD)conn->index, request_id)) {
        v9x_file_zero(write_state, sizeof(*write_state));
        return v9x_file_error(conn, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                              ERROR_FILE_TOO_LARGE, "upload exceeds limits");
    }
    write_state->handle = CreateFileA(write_state->temp_path,
                                      GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, 0);
    if (write_state->handle == INVALID_HANDLE_VALUE) {
        native_error = GetLastError();
        v9x_file_zero(write_state, sizeof(*write_state));
        return v9x_file_error(conn, request_id,
                              v9x_file_status(native_error), native_error,
                              "upload part create failed");
    }
    write_state->received = 0ul;
    write_state->running_crc = v9x_crc32_begin();
    v9x_write_u32(conn->file_response, write_state->expected_size);
    v9x_write_u32(conn->file_response + 4, write_state->expected_crc);
    v9x_file_log("file-write-open", write_state->final_path, 0, 0ul, 0ul);
    return v9x_send_frame(conn, V9X_MSG_FILE_WRITE_READY, request_id,
                          conn->file_response, 8ul);
}

static int v9x_handle_write_chunk(V9xConnection *conn, DWORD request_id,
                                  const unsigned char *payload, DWORD length)
{
    V9xWriteState *write_state = &conn->write_state;
    DWORD chunk_length;
    DWORD written = 0ul;
    DWORD native_error;
    if (write_state->handle == 0 || write_state->handle == INVALID_HANDLE_VALUE) {
        return v9x_file_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              ERROR_INVALID_HANDLE, "no upload active");
    }
    if (length < 4ul || length > V9X_FILE_CHUNK_SIZE + 4ul ||
        v9x_read_u32(payload) != write_state->received) {
        return v9x_file_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              ERROR_INVALID_PARAMETER, "upload offset mismatch");
    }
    chunk_length = length - 4ul;
    if (chunk_length > write_state->expected_size - write_state->received) {
        return v9x_file_error(conn, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                              ERROR_FILE_TOO_LARGE, "upload exceeds declared size");
    }
    if (chunk_length != 0ul &&
        (!WriteFile(write_state->handle, payload + 4, chunk_length,
                    &written, 0) || written != chunk_length)) {
        native_error = GetLastError();
        return v9x_file_error(conn, request_id, V9X_STATUS_IO_FAILED,
                              native_error, "upload write failed");
    }
    write_state->running_crc =
        v9x_crc32_update(write_state->running_crc, payload + 4, chunk_length);
    write_state->received += chunk_length;
    v9x_write_u32(conn->file_response, write_state->received);
    return v9x_send_frame(conn, V9X_MSG_FILE_WRITE_ACK, request_id,
                          conn->file_response, 4ul);
}

static int v9x_handle_commit(V9xConnection *conn, DWORD request_id,
                             DWORD length)
{
    V9xWriteState *write_state = &conn->write_state;
    DWORD actual_crc;
    DWORD native_error = 0ul;
    unsigned long promote_status;
    if (length != 0ul || write_state->handle == 0 ||
        write_state->handle == INVALID_HANDLE_VALUE) {
        return v9x_file_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              ERROR_INVALID_HANDLE, "no upload to commit");
    }
    if (!FlushFileBuffers(write_state->handle)) {
        native_error = GetLastError();
        CloseHandle(write_state->handle);
        write_state->handle = 0;
        (void)DeleteFileA(write_state->temp_path);
        v9x_file_zero(write_state, sizeof(*write_state));
        return v9x_file_error(conn, request_id, V9X_STATUS_IO_FAILED,
                              native_error, "upload flush failed");
    }
    CloseHandle(write_state->handle);
    write_state->handle = 0;
    actual_crc = v9x_crc32_end(write_state->running_crc);
    if (write_state->received != write_state->expected_size ||
        actual_crc != write_state->expected_crc) {
        (void)DeleteFileA(write_state->temp_path);
        v9x_file_zero(write_state, sizeof(*write_state));
        return v9x_file_error(conn, request_id, V9X_STATUS_CRC_MISMATCH,
                              ERROR_CRC, "upload size or CRC mismatch");
    }
    promote_status = v9x_promote_temp(write_state->final_path,
                                      write_state->temp_path, &native_error);
    if (promote_status != V9X_STATUS_OK) {
        v9x_file_zero(write_state, sizeof(*write_state));
        return v9x_file_error(conn, request_id, promote_status, native_error,
                              "upload final rename failed");
    }
    v9x_write_u32(conn->file_response, write_state->received);
    v9x_write_u32(conn->file_response + 4, actual_crc);
    v9x_file_log("file-write-complete", write_state->final_path, 1,
                 write_state->received, actual_crc);
    v9x_file_zero(write_state, sizeof(*write_state));
    return v9x_send_frame(conn, V9X_MSG_FILE_WRITE_COMPLETE, request_id,
                          conn->file_response, 8ul);
}

static int v9x_handle_read(V9xConnection *conn, DWORD request_id,
                           const unsigned char *payload, DWORD length)
{
    char path[260];
    HANDLE file;
    DWORD high = 0ul;
    DWORD size;
    DWORD offset = 0ul;
    DWORD read_count;
    DWORD native_error;
    DWORD running_crc = v9x_crc32_begin();
    DWORD actual_crc;
    if (!v9x_read_path(payload, length, 0ul, path, 259ul)) {
        return v9x_file_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              ERROR_INVALID_PARAMETER, "invalid read path");
    }
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        native_error = GetLastError();
        return v9x_file_error(conn, request_id, v9x_file_status(native_error),
                              native_error, "download open failed");
    }
    size = GetFileSize(file, &high);
    native_error = GetLastError();
    if (size == 0xfffffffful || high != 0ul ||
        size > V9X_MAX_FILE_SIZE) {
        CloseHandle(file);
        return v9x_file_error(conn, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                              native_error, "download exceeds limits");
    }
    v9x_file_log("file-read-open", path, 0, 0ul, 0ul);
    while (offset < size) {
        DWORD wanted = size - offset;
        if (wanted > V9X_FILE_CHUNK_SIZE) wanted = V9X_FILE_CHUNK_SIZE;
        if (!ReadFile(file, conn->file_buffer + 4, wanted, &read_count, 0) ||
            read_count == 0ul) {
            native_error = GetLastError();
            CloseHandle(file);
            return v9x_file_error(conn, request_id, V9X_STATUS_IO_FAILED,
                                  native_error, "download read failed");
        }
        running_crc = v9x_crc32_update(running_crc, conn->file_buffer + 4,
                                       read_count);
        v9x_write_u32(conn->file_buffer, offset);
        if (!v9x_send_frame(conn, V9X_MSG_FILE_READ_CHUNK, request_id,
                            conn->file_buffer, read_count + 4ul)) {
            CloseHandle(file);
            return 0;
        }
        offset += read_count;
    }
    CloseHandle(file);
    actual_crc = v9x_crc32_end(running_crc);
    v9x_write_u32(conn->file_response, size);
    v9x_write_u32(conn->file_response + 4, actual_crc);
    v9x_file_log("file-read-complete", path, 1, size, actual_crc);
    return v9x_send_frame(conn, V9X_MSG_FILE_READ_COMPLETE, request_id,
                          conn->file_response, 8ul);
}

int v9x_is_file_message(unsigned short type)
{
    return type >= V9X_MSG_FILE_STAT_REQUEST &&
           type <= V9X_MSG_FILE_OPEN_READ;
}

int v9x_handle_file_message(V9xConnection *conn, unsigned short type,
                            unsigned long request_id,
                            const unsigned char *payload,
                            unsigned long length)
{
    if (type == V9X_MSG_FILE_STAT_REQUEST)
        return v9x_handle_stat(conn, request_id, payload, length);
    if (type == V9X_MSG_FILE_LIST_REQUEST)
        return v9x_handle_list(conn, request_id, payload, length);
    if (type == V9X_MSG_FILE_MKDIR_REQUEST)
        return v9x_handle_mkdir(conn, request_id, payload, length);
    if (type == V9X_MSG_FILE_OPEN_WRITE)
        return v9x_handle_open_write(conn, request_id, payload, length);
    if (type == V9X_MSG_FILE_WRITE_CHUNK)
        return v9x_handle_write_chunk(conn, request_id, payload, length);
    if (type == V9X_MSG_FILE_COMMIT)
        return v9x_handle_commit(conn, request_id, length);
    if (type == V9X_MSG_FILE_OPEN_READ)
        return v9x_handle_read(conn, request_id, payload, length);
    return v9x_file_error(conn, request_id, V9X_STATUS_UNSUPPORTED_OPERATION,
                          ERROR_INVALID_FUNCTION, "unsupported file operation");
}

void v9x_files_disconnect(V9xConnection *conn)
{
    V9xWriteState *write_state = &conn->write_state;
    if (write_state->handle != 0 &&
        write_state->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(write_state->handle);
        (void)DeleteFileA(write_state->temp_path);
    }
    v9x_file_zero(write_state, sizeof(*write_state));
}
