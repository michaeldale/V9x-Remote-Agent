/* File transfer for the DOS agent.

   Ported almost verbatim from src\guest\files.c: the wire frames, the chunked
   offset-validated upload, the running CRC, the transactional temp -> verify ->
   backup -> rename commit, and the chunked download-read are all unchanged, so
   v9xctl.ps1 and the MCP server transfer files to a DOS box exactly as they do
   to a Win9x one. Only the storage layer differs - Win32 file calls
   (CreateFileA/ReadFile/WriteFile/FindFirstFileA/MoveFileA/...) are replaced by
   the C library (open/read/write/_dos_findfirst/rename/...).

   Two DOS-specific changes from the Win9x version:
   - 8.3 staging: the Win9x agent staged uploads as FILE.EXE.PART and backed up
     as FILE.EXE.V9X.BAK, which are illegal on FAT. Here the temp and backup use
     fixed 8.3 names (V9XUP.TMP / V9XUP.BAK) created in the destination's own
     directory, so the final rename stays on one volume.
   - Attribute bits: DOS _A_* attribute bits (subdir 0x10, readonly 0x01, hidden
     0x02, system 0x04, archive 0x20) are numerically identical to the low Win32
     FILE_ATTRIBUTE_* bits, so they travel on the wire unchanged. */

#include <dos.h>
#include <direct.h>
#include <errno.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

#include "dosagent.h"
#include "v9xremote/crc32.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"

static void v9x_reset_write_state(V9xDosWriteState *write_state)
{
    write_state->handle = -1;
    write_state->expected_size = 0ul;
    write_state->expected_crc = 0ul;
    write_state->received = 0ul;
    write_state->running_crc = 0ul;
    write_state->final_path[0] = '\0';
    write_state->temp_path[0] = '\0';
}

static unsigned long v9x_file_status(int err)
{
    if (err == ENOENT) return V9X_STATUS_NOT_FOUND;
    if (err == EACCES) return V9X_STATUS_ACCESS_DENIED;
    return V9X_STATUS_IO_FAILED;
}

static int v9x_file_error(V9xDos *dos, unsigned long request_id,
                          unsigned long status, unsigned long native_error,
                          const char *detail)
{
    unsigned long offset = 8ul;
    v9x_write_u32(dos->file_response, status);
    v9x_write_u32(dos->file_response + 4, native_error);
    if (!v9x_append_string(dos->file_response, sizeof(dos->file_response),
                           &offset, detail)) return 0;
    return v9x_send_frame(dos, V9X_MSG_ERROR_RESPONSE, request_id,
                          dos->file_response, offset);
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

/* Copy the directory prefix of `path` (up to and including the last path
   separator) into `target`. No separator means the current directory (""). */
static void v9x_dir_prefix(const char *path, char *target)
{
    unsigned long length = 0ul;
    unsigned long last = 0ul;
    unsigned long index;
    while (path[length] != '\0') {
        if (path[length] == '\\' || path[length] == '/') last = length + 1ul;
        ++length;
    }
    for (index = 0ul; index < last; ++index) target[index] = path[index];
    target[last] = '\0';
}

/* Build a staging path (temp or backup) in the destination's own directory. */
static int v9x_make_sibling(char *target, const char *final_path,
                            const char *name)
{
    char dir[260];
    unsigned long length;
    unsigned long index;
    v9x_dir_prefix(final_path, dir);
    length = 0ul;
    while (dir[length] != '\0') ++length;
    if (length + 16ul >= 260ul) return 0;
    for (index = 0ul; dir[index] != '\0'; ++index) target[index] = dir[index];
    for (index = 0ul; name[index] != '\0'; ++index) {
        target[length + index] = name[index];
    }
    target[length + index] = '\0';
    return 1;
}

/* Promote a fully written, CRC-verified temp file to its final path, backing up
   any existing destination first so a failure rolls back cleanly. DOS version of
   v9x_promote_temp from src\guest\files.c (rename/remove instead of MoveFileA;
   temp and backup are siblings of the destination so rename stays on volume). */
static unsigned long v9x_promote_temp(const char *final_path,
                                      const char *temp_path,
                                      unsigned long *native_error)
{
    unsigned attributes;
    char backup_path[260];
    int backed_up = 0;
    *native_error = 0ul;
    if (_dos_getfileattr(final_path, &attributes) == 0) {
        if ((attributes & _A_SUBDIR) != 0u ||
            !v9x_make_sibling(backup_path, final_path, V9X_DOS_BAK_NAME)) {
            (void)remove(temp_path);
            *native_error = (unsigned long)EACCES;
            return V9X_STATUS_ACCESS_DENIED;
        }
        (void)remove(backup_path);
        if (rename(final_path, backup_path) != 0) {
            *native_error = (unsigned long)errno;
            (void)remove(temp_path);
            return v9x_file_status(errno);
        }
        backed_up = 1;
    }
    if (rename(temp_path, final_path) != 0) {
        *native_error = (unsigned long)errno;
        if (backed_up) (void)rename(backup_path, final_path);
        (void)remove(temp_path);
        return V9X_STATUS_IO_FAILED;
    }
    if (backed_up) (void)remove(backup_path);
    return V9X_STATUS_OK;
}

static int v9x_handle_stat(V9xDos *dos, unsigned long request_id,
                           const unsigned char *payload, unsigned long length)
{
    char path[260];
    unsigned attributes;
    unsigned long size = 0ul;
    int fd;
    long file_size;
    if (!v9x_read_path(payload, length, 0ul, path, 259ul)) {
        return v9x_file_error(dos, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              (unsigned long)EINVAL, "invalid stat path");
    }
    if (_dos_getfileattr(path, &attributes) != 0) {
        if (errno != ENOENT) {
            return v9x_file_error(dos, request_id, v9x_file_status(errno),
                                  (unsigned long)errno, "stat attributes failed");
        }
        dos->file_response[0] = 0u;
        dos->file_response[1] = 0u;
        v9x_write_u16(dos->file_response + 2, 0u);
        v9x_write_u32(dos->file_response + 4, 0ul);
        v9x_write_u32(dos->file_response + 8, 0ul);
        v9x_write_u32(dos->file_response + 12, (unsigned long)ENOENT);
        return v9x_send_frame(dos, V9X_MSG_FILE_STAT_RESPONSE, request_id,
                              dos->file_response, 16ul);
    }
    if ((attributes & _A_SUBDIR) == 0u) {
        fd = open(path, O_RDONLY | O_BINARY);
        if (fd < 0) {
            return v9x_file_error(dos, request_id, v9x_file_status(errno),
                                  (unsigned long)errno, "stat open failed");
        }
        file_size = filelength(fd);
        close(fd);
        if (file_size < 0l) {
            return v9x_file_error(dos, request_id, V9X_STATUS_IO_FAILED,
                                  (unsigned long)errno, "stat size failed");
        }
        if ((unsigned long)file_size > V9X_MAX_FILE_SIZE) {
            return v9x_file_error(dos, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                                  0ul, "file exceeds v1 size");
        }
        size = (unsigned long)file_size;
    }
    dos->file_response[0] = 1u;
    dos->file_response[1] = (attributes & _A_SUBDIR) != 0u ? 1u : 0u;
    v9x_write_u16(dos->file_response + 2, 0u);
    v9x_write_u32(dos->file_response + 4, size);
    v9x_write_u32(dos->file_response + 8, (unsigned long)attributes);
    v9x_write_u32(dos->file_response + 12, 0ul);
    return v9x_send_frame(dos, V9X_MSG_FILE_STAT_RESPONSE, request_id,
                          dos->file_response, 16ul);
}

static int v9x_handle_list(V9xDos *dos, unsigned long request_id,
                           const unsigned char *payload, unsigned long length)
{
    char path[260];
    char pattern[268];
    struct find_t find;
    unsigned rc;
    unsigned long path_length;
    unsigned long offset = 4ul;
    unsigned long count = 0ul;
    unsigned long index;
    unsigned long name_length;
    if (!v9x_read_path(payload, length, 0ul, path, 255ul)) {
        return v9x_file_error(dos, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              (unsigned long)EINVAL, "invalid list path");
    }
    path_length = v9x_bounded_length(path, 256ul);
    for (index = 0ul; index < path_length; ++index) pattern[index] = path[index];
    if (path_length != 0ul && pattern[path_length - 1ul] != '\\' &&
        pattern[path_length - 1ul] != '/') pattern[path_length++] = '\\';
    pattern[path_length++] = '*'; pattern[path_length++] = '.';
    pattern[path_length++] = '*'; pattern[path_length] = '\0';
    rc = _dos_findfirst(pattern, _A_HIDDEN | _A_SYSTEM | _A_SUBDIR, &find);
    if (rc != 0u) {
        /* An empty or missing directory: report a zero-entry listing rather than
           an error when the path itself exists as a directory. */
        unsigned dir_attr;
        if (_dos_getfileattr(path, &dir_attr) == 0 &&
            (dir_attr & _A_SUBDIR) != 0u) {
            v9x_write_u32(dos->file_response, 0ul);
            return v9x_send_frame(dos, V9X_MSG_FILE_LIST_RESPONSE, request_id,
                                  dos->file_response, 4ul);
        }
        return v9x_file_error(dos, request_id, v9x_file_status(errno),
                              (unsigned long)errno, "directory list failed");
    }
    do {
        if ((find.name[0] == '.' && find.name[1] == '\0') ||
            (find.name[0] == '.' && find.name[1] == '.' &&
             find.name[2] == '\0')) continue;
        name_length = v9x_bounded_length(find.name, sizeof(find.name));
        if (name_length >= sizeof(find.name) ||
            offset + 10ul + name_length > sizeof(dos->file_response)) {
            return v9x_file_error(dos, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                                  0ul, "directory listing too large");
        }
        for (index = 0ul; index < name_length; ++index) {
            if ((unsigned char)find.name[index] > 0x7fu) {
                return v9x_file_error(dos, request_id, V9X_STATUS_INVALID_PAYLOAD,
                                      0ul, "non-ASCII directory entry");
            }
        }
        v9x_write_u32(dos->file_response + offset,
                      (unsigned long)(unsigned char)find.attrib);
        v9x_write_u32(dos->file_response + offset + 4, find.size);
        offset += 8ul;
        if (!v9x_append_string(dos->file_response, sizeof(dos->file_response),
                               &offset, find.name)) {
            return v9x_file_error(dos, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                                  0ul, "directory listing overflow");
        }
        ++count;
    } while (_dos_findnext(&find) == 0u);
    v9x_write_u32(dos->file_response, count);
    return v9x_send_frame(dos, V9X_MSG_FILE_LIST_RESPONSE, request_id,
                          dos->file_response, offset);
}

static int v9x_handle_mkdir(V9xDos *dos, unsigned long request_id,
                            const unsigned char *payload, unsigned long length)
{
    char path[260];
    unsigned attributes;
    unsigned long created = 1ul;
    if (!v9x_read_path(payload, length, 0ul, path, 259ul)) {
        return v9x_file_error(dos, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              (unsigned long)EINVAL, "invalid mkdir path");
    }
    if (mkdir(path) != 0) {
        int err = errno;
        if (err != EEXIST || _dos_getfileattr(path, &attributes) != 0 ||
            (attributes & _A_SUBDIR) == 0u) {
            return v9x_file_error(dos, request_id, v9x_file_status(err),
                                  (unsigned long)err, "mkdir failed");
        }
        created = 0ul;
    }
    v9x_write_u32(dos->file_response, created);
    return v9x_send_frame(dos, V9X_MSG_FILE_MKDIR_RESPONSE, request_id,
                          dos->file_response, 4ul);
}

static int v9x_handle_open_write(V9xDos *dos, unsigned long request_id,
                                 const unsigned char *payload,
                                 unsigned long length)
{
    V9xDosWriteState *write_state = &dos->write_state;
    if (write_state->handle >= 0) {
        return v9x_file_error(dos, request_id, V9X_STATUS_BUSY, 0ul,
                              "upload already active");
    }
    if (length < 10ul ||
        !v9x_read_path(payload, length, 8ul, write_state->final_path,
                       V9X_DOS_FILE_PATH_MAX)) {
        return v9x_file_error(dos, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              (unsigned long)EINVAL, "invalid upload request");
    }
    write_state->expected_size = v9x_read_u32(payload);
    write_state->expected_crc = v9x_read_u32(payload + 4);
    if (write_state->expected_size > V9X_MAX_FILE_SIZE ||
        !v9x_make_sibling(write_state->temp_path, write_state->final_path,
                          V9X_DOS_PART_NAME)) {
        v9x_reset_write_state(write_state);
        return v9x_file_error(dos, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                              0ul, "upload exceeds limits");
    }
    write_state->handle = open(write_state->temp_path,
                               O_WRONLY | O_CREAT | O_TRUNC | O_BINARY,
                               S_IREAD | S_IWRITE);
    if (write_state->handle < 0) {
        int err = errno;
        v9x_reset_write_state(write_state);
        return v9x_file_error(dos, request_id, v9x_file_status(err),
                              (unsigned long)err, "upload part create failed");
    }
    write_state->received = 0ul;
    write_state->running_crc = v9x_crc32_begin();
    v9x_write_u32(dos->file_response, write_state->expected_size);
    v9x_write_u32(dos->file_response + 4, write_state->expected_crc);
    return v9x_send_frame(dos, V9X_MSG_FILE_WRITE_READY, request_id,
                          dos->file_response, 8ul);
}

static int v9x_handle_write_chunk(V9xDos *dos, unsigned long request_id,
                                  const unsigned char *payload,
                                  unsigned long length)
{
    V9xDosWriteState *write_state = &dos->write_state;
    unsigned long chunk_length;
    if (write_state->handle < 0) {
        return v9x_file_error(dos, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              (unsigned long)EBADF, "no upload active");
    }
    if (length < 4ul || length > V9X_FILE_CHUNK_SIZE + 4ul ||
        v9x_read_u32(payload) != write_state->received) {
        return v9x_file_error(dos, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              (unsigned long)EINVAL, "upload offset mismatch");
    }
    chunk_length = length - 4ul;
    if (chunk_length > write_state->expected_size - write_state->received) {
        return v9x_file_error(dos, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                              0ul, "upload exceeds declared size");
    }
    if (chunk_length != 0ul &&
        (unsigned long)write(write_state->handle, payload + 4,
                             (unsigned)chunk_length) != chunk_length) {
        int err = errno;
        return v9x_file_error(dos, request_id, V9X_STATUS_IO_FAILED,
                              (unsigned long)err, "upload write failed");
    }
    write_state->running_crc =
        v9x_crc32_update(write_state->running_crc, payload + 4, chunk_length);
    write_state->received += chunk_length;
    v9x_write_u32(dos->file_response, write_state->received);
    return v9x_send_frame(dos, V9X_MSG_FILE_WRITE_ACK, request_id,
                          dos->file_response, 4ul);
}

static int v9x_handle_commit(V9xDos *dos, unsigned long request_id,
                             unsigned long length)
{
    V9xDosWriteState *write_state = &dos->write_state;
    unsigned long actual_crc;
    unsigned long native_error = 0ul;
    unsigned long promote_status;
    if (length != 0ul || write_state->handle < 0) {
        return v9x_file_error(dos, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              (unsigned long)EBADF, "no upload to commit");
    }
    close(write_state->handle);
    write_state->handle = -1;
    actual_crc = v9x_crc32_end(write_state->running_crc);
    if (write_state->received != write_state->expected_size ||
        actual_crc != write_state->expected_crc) {
        (void)remove(write_state->temp_path);
        v9x_reset_write_state(write_state);
        return v9x_file_error(dos, request_id, V9X_STATUS_CRC_MISMATCH,
                              0ul, "upload size or CRC mismatch");
    }
    promote_status = v9x_promote_temp(write_state->final_path,
                                      write_state->temp_path, &native_error);
    if (promote_status != V9X_STATUS_OK) {
        v9x_reset_write_state(write_state);
        return v9x_file_error(dos, request_id, promote_status, native_error,
                              "upload final rename failed");
    }
    v9x_write_u32(dos->file_response, write_state->received);
    v9x_write_u32(dos->file_response + 4, actual_crc);
    v9x_reset_write_state(write_state);
    return v9x_send_frame(dos, V9X_MSG_FILE_WRITE_COMPLETE, request_id,
                          dos->file_response, 8ul);
}

static int v9x_handle_read(V9xDos *dos, unsigned long request_id,
                           const unsigned char *payload, unsigned long length)
{
    char path[260];
    int fd;
    long file_size;
    unsigned long size;
    unsigned long offset = 0ul;
    int read_count;
    unsigned long running_crc = v9x_crc32_begin();
    unsigned long actual_crc;
    if (!v9x_read_path(payload, length, 0ul, path, 259ul)) {
        return v9x_file_error(dos, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              (unsigned long)EINVAL, "invalid read path");
    }
    fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) {
        int err = errno;
        return v9x_file_error(dos, request_id, v9x_file_status(err),
                              (unsigned long)err, "download open failed");
    }
    file_size = filelength(fd);
    if (file_size < 0l || (unsigned long)file_size > V9X_MAX_FILE_SIZE) {
        close(fd);
        return v9x_file_error(dos, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                              (unsigned long)errno, "download exceeds limits");
    }
    size = (unsigned long)file_size;
    while (offset < size) {
        unsigned long wanted = size - offset;
        if (wanted > V9X_FILE_CHUNK_SIZE) wanted = V9X_FILE_CHUNK_SIZE;
        read_count = read(fd, dos->file_buffer + 4, (unsigned)wanted);
        if (read_count <= 0) {
            int err = errno;
            close(fd);
            return v9x_file_error(dos, request_id, V9X_STATUS_IO_FAILED,
                                  (unsigned long)err, "download read failed");
        }
        running_crc = v9x_crc32_update(running_crc, dos->file_buffer + 4,
                                       (unsigned long)read_count);
        v9x_write_u32(dos->file_buffer, offset);
        if (!v9x_send_frame(dos, V9X_MSG_FILE_READ_CHUNK, request_id,
                            dos->file_buffer,
                            (unsigned long)read_count + 4ul)) {
            close(fd);
            return 0;
        }
        offset += (unsigned long)read_count;
    }
    close(fd);
    actual_crc = v9x_crc32_end(running_crc);
    v9x_write_u32(dos->file_response, size);
    v9x_write_u32(dos->file_response + 4, actual_crc);
    return v9x_send_frame(dos, V9X_MSG_FILE_READ_COMPLETE, request_id,
                          dos->file_response, 8ul);
}

int v9x_is_file_message(unsigned short type)
{
    return type >= V9X_MSG_FILE_STAT_REQUEST &&
           type <= V9X_MSG_FILE_OPEN_READ;
}

int v9x_handle_file_message(V9xDos *dos, unsigned short type,
                            unsigned long request_id,
                            const unsigned char *payload, unsigned long length)
{
    if (type == V9X_MSG_FILE_STAT_REQUEST)
        return v9x_handle_stat(dos, request_id, payload, length);
    if (type == V9X_MSG_FILE_LIST_REQUEST)
        return v9x_handle_list(dos, request_id, payload, length);
    if (type == V9X_MSG_FILE_MKDIR_REQUEST)
        return v9x_handle_mkdir(dos, request_id, payload, length);
    if (type == V9X_MSG_FILE_OPEN_WRITE)
        return v9x_handle_open_write(dos, request_id, payload, length);
    if (type == V9X_MSG_FILE_WRITE_CHUNK)
        return v9x_handle_write_chunk(dos, request_id, payload, length);
    if (type == V9X_MSG_FILE_COMMIT)
        return v9x_handle_commit(dos, request_id, length);
    if (type == V9X_MSG_FILE_OPEN_READ)
        return v9x_handle_read(dos, request_id, payload, length);
    return v9x_file_error(dos, request_id, V9X_STATUS_UNSUPPORTED_OPERATION,
                          0ul, "unsupported file operation");
}

void v9x_files_disconnect(V9xDos *dos)
{
    V9xDosWriteState *write_state = &dos->write_state;
    if (write_state->handle >= 0) {
        close(write_state->handle);
        (void)remove(write_state->temp_path);
    }
    v9x_reset_write_state(write_state);
}
