#include "agent.h"
#include <tlhelp32.h>
#include "platform_win.h"
#include "v9xremote/crc32.h"
#include "v9xremote/protocol.h"
#include "v9xremote/sha256.h"
#include "v9xremote/status.h"
#include "v9xremote/textscan.h"

/* The verbs agents kept improvising with exec + get (agent 0.13.0+):
   FILE_DELETE, FILE_HASH, FILE_READ_TEXT (`cat`), PROCESS_LIST, PROCESS_KILL,
   a read-only REG_QUERY and WINDOW_LIST. See docs\protocol.md. */

#define V9X_DELETE_DEPTH_MAX 24
/* READ_TEXT answers in conn->payload (the request is copied out first),
   less its 24-byte header. */
#define V9X_TEXT_HEADER 24ul
#define V9X_TEXT_CAPACITY (V9X_MAX_PAYLOAD - V9X_TEXT_HEADER)

typedef HANDLE (WINAPI *V9xSnapshotFn)(DWORD, DWORD);
typedef BOOL (WINAPI *V9xProcessWalkFn)(HANDLE, LPPROCESSENTRY32);

static int v9x_tools_error(V9xConnection *conn, DWORD request_id, DWORD status,
                           DWORD native_error, const char *detail)
{
    unsigned long offset = 8ul;
    v9x_write_u32(conn->response, status);
    v9x_write_u32(conn->response + 4, native_error);
    if (!v9x_append_string(conn->response, sizeof(conn->response), &offset,
                           detail)) return 0;
    return v9x_send_frame(conn, V9X_MSG_ERROR_RESPONSE, request_id,
                          conn->response, offset);
}

static DWORD v9x_tools_status(DWORD native_error)
{
    if (native_error == ERROR_FILE_NOT_FOUND || native_error == ERROR_PATH_NOT_FOUND) {
        return V9X_STATUS_NOT_FOUND;
    }
    if (native_error == ERROR_ACCESS_DENIED || native_error == ERROR_SHARING_VIOLATION) {
        return V9X_STATUS_ACCESS_DENIED;
    }
    return V9X_STATUS_IO_FAILED;
}

/* A length-prefixed ASCII string at *offset (1..maximum bytes, or 0 bytes
   when allow_empty). */
static int v9x_tools_string(const unsigned char *payload, unsigned long length,
                            unsigned long *offset, char *target,
                            unsigned long maximum, int allow_empty)
{
    unsigned long count;
    unsigned long index;
    if (*offset > length || length - *offset < 2ul) return 0;
    count = (unsigned long)v9x_read_u16(payload + *offset);
    *offset += 2ul;
    if ((count == 0ul && !allow_empty) || count > maximum ||
        count > length - *offset) return 0;
    for (index = 0ul; index < count; ++index) {
        unsigned char ch = payload[*offset + index];
        if (ch == 0u || ch > 0x7fu) return 0;
        target[index] = (char)ch;
    }
    target[count] = '\0';
    *offset += count;
    return 1;
}

static char v9x_tools_upper(char ch)
{
    return (ch >= 'a' && ch <= 'z') ? (char)(ch - ('a' - 'A')) : ch;
}

static int v9x_tools_same(const char *left, const char *right)
{
    while (*left != '\0' && v9x_tools_upper(*left) == v9x_tools_upper(*right)) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

/* True if `outer` is `inner` or one of its parent directories. */
static int v9x_tools_covers(const char *outer, const char *inner)
{
    unsigned long index = 0ul;
    while (outer[index] != '\0' &&
           v9x_tools_upper(outer[index]) == v9x_tools_upper(inner[index])) ++index;
    if (outer[index] != '\0') return 0;
    if (index > 0ul && outer[index - 1ul] == '\\') return 1;
    return inner[index] == '\0' || inner[index] == '\\';
}

/* Windows 9x keeps the last INI file written with WritePrivateProfileString
   in memory and writes it to disk lazily, so a raw read returns the old
   contents (a 43-byte C:\V9XSYNC.INI that TYPE, which starts a DOS VM and so
   forces a flush, showed as five lines). All-NULL arguments but the file
   name flush it. Harmless for a file that is not cached, and a no-op on NT. */
void v9x_flush_ini_cache(const char *path)
{
    if (!v9x_platform_is_nt()) (void)WritePrivateProfileStringA(0, 0, 0, path);
}

/* ---- FILE_DELETE ---------------------------------------------------- */

typedef struct V9xDeleteCount {
    DWORD files;
    DWORD directories;
    DWORD native_error;
} V9xDeleteCount;

static int v9x_delete_file(const char *path, DWORD attributes, int force,
                           V9xDeleteCount *count)
{
    if (force && (attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                                FILE_ATTRIBUTE_SYSTEM)) != 0ul) {
        (void)SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    }
    if (!DeleteFileA(path)) {
        count->native_error = GetLastError();
        return 0;
    }
    ++count->files;
    return 1;
}

static int v9x_delete_tree(char *path, unsigned long length, int depth, int force,
                           V9xDeleteCount *count)
{
    WIN32_FIND_DATAA data;
    HANDLE search;
    unsigned long index;
    if (depth > V9X_DELETE_DEPTH_MAX || length + 2ul >= MAX_PATH) {
        count->native_error = ERROR_FILENAME_EXCED_RANGE;
        return 0;
    }
    path[length] = '\\';
    path[length + 1ul] = '*';
    path[length + 2ul] = '\0';
    search = FindFirstFileA(path, &data);
    path[length] = '\0';
    if (search == INVALID_HANDLE_VALUE) {
        count->native_error = GetLastError();
        if (count->native_error != ERROR_FILE_NOT_FOUND &&
            count->native_error != ERROR_NO_MORE_FILES) return 0;
    } else {
        do {
            unsigned long name_length = 0ul;
            if (data.cFileName[0] == '.' &&
                (data.cFileName[1] == '\0' ||
                 (data.cFileName[1] == '.' && data.cFileName[2] == '\0'))) continue;
            while (data.cFileName[name_length] != '\0') ++name_length;
            if (length + 1ul + name_length >= MAX_PATH) {
                FindClose(search);
                count->native_error = ERROR_FILENAME_EXCED_RANGE;
                return 0;
            }
            path[length] = '\\';
            for (index = 0ul; index <= name_length; ++index) {
                path[length + 1ul + index] = data.cFileName[index];
            }
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0ul) {
                if (!v9x_delete_tree(path, length + 1ul + name_length, depth + 1,
                                     force, count)) {
                    FindClose(search);
                    return 0;
                }
            } else if (!v9x_delete_file(path, data.dwFileAttributes, force, count)) {
                FindClose(search);
                path[length] = '\0';
                return 0;
            }
            path[length] = '\0';
        } while (FindNextFileA(search, &data));
        FindClose(search);
    }
    if (force) (void)SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    if (!RemoveDirectoryA(path)) {
        count->native_error = GetLastError();
        return 0;
    }
    ++count->directories;
    return 1;
}

static int v9x_handle_delete(V9xConnection *conn, DWORD request_id,
                             const unsigned char *payload, unsigned long length)
{
    char requested[MAX_PATH];
    char full[MAX_PATH];
    char windows_dir[MAX_PATH];
    char system_dir[MAX_PATH];
    char agent_dir[MAX_PATH];
    char *file_part;
    unsigned long offset = 4ul;
    unsigned long full_length;
    DWORD flags;
    DWORD attributes;
    V9xDeleteCount count;
    int ok;
    if (length < 4ul ||
        !v9x_tools_string(payload, length, &offset, requested, MAX_PATH - 1ul, 0) ||
        offset != length) {
        return v9x_tools_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                               ERROR_INVALID_PARAMETER, "invalid delete request");
    }
    flags = v9x_read_u32(payload);
    full_length = GetFullPathNameA(requested, MAX_PATH, full, &file_part);
    if (full_length == 0ul || full_length >= MAX_PATH) {
        return v9x_tools_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                               GetLastError(), "delete path could not be resolved");
    }
    while (full_length > 3ul && full[full_length - 1ul] == '\\') full[--full_length] = '\0';
    windows_dir[0] = '\0';
    system_dir[0] = '\0';
    (void)GetWindowsDirectoryA(windows_dir, sizeof(windows_dir));
    (void)GetSystemDirectoryA(system_dir, sizeof(system_dir));
    lstrcpynA(agent_dir, "C:\\V9XREMOTE", sizeof(agent_dir));
    /* Never a drive or share root, the Windows or system directory, the
       agent's own folder, or anything that contains one of them. Single
       files inside them (a driver being replaced) are fine. */
    if (full_length <= 3ul || (full[0] == '\\' && full[1] == '\\') ||
        (windows_dir[0] != '\0' && v9x_tools_covers(full, windows_dir)) ||
        (system_dir[0] != '\0' && v9x_tools_covers(full, system_dir)) ||
        v9x_tools_covers(full, agent_dir)) {
        return v9x_tools_error(conn, request_id, V9X_STATUS_ACCESS_DENIED,
                               ERROR_ACCESS_DENIED,
                               "refused: a root, the Windows or system directory, "
                               "the agent folder, or a folder containing one");
    }
    attributes = GetFileAttributesA(full);
    if (attributes == 0xfffffffful) {
        DWORD native_error = GetLastError();
        return v9x_tools_error(conn, request_id, v9x_tools_status(native_error),
                               native_error, "delete target not found");
    }
    count.files = 0ul;
    count.directories = 0ul;
    count.native_error = 0ul;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0ul) {
        ok = v9x_delete_file(full, attributes, (flags & V9X_DELETE_FORCE) != 0ul, &count);
    } else if ((flags & V9X_DELETE_RECURSIVE) != 0ul) {
        ok = v9x_delete_tree(full, full_length, 0,
                             (flags & V9X_DELETE_FORCE) != 0ul, &count);
    } else {
        if ((flags & V9X_DELETE_FORCE) != 0ul) {
            (void)SetFileAttributesA(full, FILE_ATTRIBUTE_NORMAL);
        }
        ok = RemoveDirectoryA(full) != FALSE;
        if (ok) ++count.directories;
        else count.native_error = GetLastError();
    }
    v9x_log_event(ok ? "file-delete" : "file-delete-failed", full);
    if (!ok) {
        /* Report what was already removed in the detail: a recursive delete
           is not transactional. */
        return v9x_tools_error(conn, request_id, v9x_tools_status(count.native_error),
                               count.native_error,
                               count.files + count.directories != 0ul ?
                                   "delete stopped part way (some entries removed)" :
                               (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0ul &&
                               (flags & V9X_DELETE_RECURSIVE) == 0ul ?
                                   "directory not empty or in use (use -Recurse)" :
                                   "delete failed (read-only or in use? -Force clears attributes)");
    }
    v9x_write_u32(conn->response, count.files);
    v9x_write_u32(conn->response + 4, count.directories);
    return v9x_send_frame(conn, V9X_MSG_FILE_DELETE_RESPONSE, request_id,
                          conn->response, 8ul);
}

/* ---- FILE_HASH ------------------------------------------------------ */

static int v9x_handle_hash(V9xConnection *conn, DWORD request_id,
                           const unsigned char *payload, unsigned long length)
{
    char path[MAX_PATH];
    unsigned long offset = 0ul;
    struct v9x_sha256 sha;
    DWORD crc = v9x_crc32_begin();
    DWORD size;
    DWORD high = 0ul;
    DWORD total = 0ul;
    DWORD read_count;
    DWORD native_error;
    HANDLE file;
    if (!v9x_tools_string(payload, length, &offset, path, MAX_PATH - 1ul, 0) ||
        offset != length) {
        return v9x_tools_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                               ERROR_INVALID_PARAMETER, "invalid hash path");
    }
    v9x_flush_ini_cache(path);
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        native_error = GetLastError();
        return v9x_tools_error(conn, request_id, v9x_tools_status(native_error),
                               native_error, "hash open failed");
    }
    size = GetFileSize(file, &high);
    if (size == 0xfffffffful || high != 0ul || size > V9X_MAX_FILE_SIZE) {
        native_error = GetLastError();
        CloseHandle(file);
        return v9x_tools_error(conn, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                               native_error, "hash exceeds limits");
    }
    v9x_sha256_init(&sha);
    while (total < size) {
        DWORD wanted = size - total;
        if (wanted > V9X_FILE_CHUNK_SIZE) wanted = V9X_FILE_CHUNK_SIZE;
        if (!ReadFile(file, conn->file_buffer, wanted, &read_count, 0) ||
            read_count == 0ul) {
            native_error = GetLastError();
            CloseHandle(file);
            return v9x_tools_error(conn, request_id, V9X_STATUS_IO_FAILED,
                                   native_error, "hash read failed");
        }
        crc = v9x_crc32_update(crc, conn->file_buffer, read_count);
        v9x_sha256_update(&sha, conn->file_buffer, read_count);
        total += read_count;
    }
    CloseHandle(file);
    v9x_write_u32(conn->response, size);
    v9x_write_u32(conn->response + 4, v9x_crc32_end(crc));
    v9x_sha256_final(&sha, conn->response + 8);
    return v9x_send_frame(conn, V9X_MSG_FILE_HASH_RESPONSE, request_id,
                          conn->response, 40ul);
}

/* ---- FILE_READ_TEXT ------------------------------------------------- */

static V9xTextScan v9x_text_scans[V9X_MAX_CONNECTIONS];

static int v9x_text_pass(V9xConnection *conn, const char *path, V9xTextScan *scan,
                         DWORD *size, DWORD *native_error)
{
    DWORD high = 0ul;
    DWORD total = 0ul;
    DWORD read_count;
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        *native_error = GetLastError();
        return 0;
    }
    *size = GetFileSize(file, &high);
    if (*size == 0xfffffffful || high != 0ul || *size > V9X_MAX_FILE_SIZE) {
        *native_error = ERROR_FILE_TOO_LARGE;
        CloseHandle(file);
        return 0;
    }
    while (total < *size) {
        DWORD wanted = *size - total;
        if (wanted > V9X_FILE_CHUNK_SIZE) wanted = V9X_FILE_CHUNK_SIZE;
        if (!ReadFile(file, conn->file_buffer, wanted, &read_count, 0) ||
            read_count == 0ul) {
            *native_error = GetLastError();
            CloseHandle(file);
            return 0;
        }
        v9x_text_feed(scan, conn->file_buffer, read_count);
        total += read_count;
    }
    CloseHandle(file);
    v9x_text_end(scan);
    return 1;
}

static int v9x_handle_read_text(V9xConnection *conn, DWORD request_id,
                                const unsigned char *payload, unsigned long length)
{
    char path[MAX_PATH];
    char match[V9X_TEXT_MATCH_MAX + 1ul];
    unsigned long offset = 8ul;
    unsigned long match_length = 0ul;
    DWORD tail;
    DWORD capacity;
    DWORD size = 0ul;
    DWORD native_error = 0ul;
    DWORD skip = 0ul;
    V9xTextScan *scan = &v9x_text_scans[conn->index];
    if (length < 8ul ||
        !v9x_tools_string(payload, length, &offset, match, V9X_TEXT_MATCH_MAX, 1) ||
        !v9x_tools_string(payload, length, &offset, path, MAX_PATH - 1ul, 0) ||
        offset != length) {
        return v9x_tools_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                               ERROR_INVALID_PARAMETER, "invalid read-text request");
    }
    tail = v9x_read_u32(payload);
    capacity = v9x_read_u32(payload + 4);
    if (capacity == 0ul || capacity > V9X_TEXT_CAPACITY) capacity = V9X_TEXT_CAPACITY;
    while (match[match_length] != '\0') ++match_length;
    v9x_flush_ini_cache(path);
    /* The request has been copied out; conn->payload holds the answer. */
    if (tail != 0ul) {
        v9x_text_begin(scan, 0, 0ul, match, match_length, 0, 0ul);
        if (!v9x_text_pass(conn, path, scan, &size, &native_error)) {
            return v9x_tools_error(conn, request_id, v9x_tools_status(native_error),
                                   native_error, "read-text failed");
        }
        if (scan->matched > tail) skip = scan->matched - tail;
    }
    v9x_text_begin(scan, 1, skip, match, match_length,
                   conn->payload + V9X_TEXT_HEADER, capacity);
    if (!v9x_text_pass(conn, path, scan, &size, &native_error)) {
        return v9x_tools_error(conn, request_id, v9x_tools_status(native_error),
                               native_error, "read-text failed");
    }
    v9x_write_u32(conn->payload, size);
    v9x_write_u32(conn->payload + 4, scan->lines);
    v9x_write_u32(conn->payload + 8, scan->matched);
    v9x_write_u32(conn->payload + 12, scan->emitted);
    v9x_write_u32(conn->payload + 16, scan->flags);
    v9x_write_u32(conn->payload + 20, scan->used);
    return v9x_send_frame(conn, V9X_MSG_FILE_READ_TEXT_RESPONSE, request_id,
                          conn->payload, V9X_TEXT_HEADER + scan->used);
}

/* ---- PROCESS_LIST / PROCESS_KILL ------------------------------------ */

static int v9x_toolhelp(V9xSnapshotFn *snapshot, V9xProcessWalkFn *first,
                        V9xProcessWalkFn *next)
{
    HMODULE kernel = GetModuleHandleA("KERNEL32.DLL");
    if (kernel == 0) return 0;
    *snapshot = (V9xSnapshotFn)GetProcAddress(kernel, "CreateToolhelp32Snapshot");
    *first = (V9xProcessWalkFn)GetProcAddress(kernel, "Process32First");
    *next = (V9xProcessWalkFn)GetProcAddress(kernel, "Process32Next");
    return *snapshot != 0 && *first != 0 && *next != 0;
}

/* Toolhelp exists on 95/98/ME and 2000, not NT4. Resolved at run time so
   an NT4 agent still loads (it just withholds V9X_CAP_PROCESS). */
int v9x_process_supported(void)
{
    V9xSnapshotFn snapshot;
    V9xProcessWalkFn first;
    V9xProcessWalkFn next;
    return v9x_toolhelp(&snapshot, &first, &next);
}

static const char *v9x_base_name(const char *path)
{
    const char *base = path;
    for (; *path != '\0'; ++path) {
        if (*path == '\\' || *path == '/' || *path == ':') base = path + 1;
    }
    return base;
}

static int v9x_agent_job(V9xAgentState *machine, DWORD pid, DWORD *request_id)
{
    int index;
    for (index = 0; index < V9X_EXEC_POOL_SIZE; ++index) {
        V9xExecSlot *slot = &machine->exec_pool[index];
        if (pid != 0ul && slot->process_id == pid && v9x_flag_read(&slot->active) != 0l) {
            *request_id = slot->request_id;
            return 1;
        }
    }
    return 0;
}

/* Name of `pid` from a fresh snapshot, "" if it is gone. */
static int v9x_process_name(DWORD pid, char *name, unsigned long capacity)
{
    V9xSnapshotFn snapshot;
    V9xProcessWalkFn first;
    V9xProcessWalkFn next;
    PROCESSENTRY32 entry;
    HANDLE list;
    int found = 0;
    name[0] = '\0';
    if (!v9x_toolhelp(&snapshot, &first, &next)) return 0;
    list = snapshot(TH32CS_SNAPPROCESS, 0ul);
    if (list == INVALID_HANDLE_VALUE) return 0;
    entry.dwSize = sizeof(entry);
    if (first(list, &entry)) {
        do {
            if (entry.th32ProcessID == pid) {
                lstrcpynA(name, v9x_base_name(entry.szExeFile), (int)capacity);
                found = 1;
                break;
            }
            entry.dwSize = sizeof(entry);
        } while (next(list, &entry));
    }
    CloseHandle(list);
    return found;
}

static int v9x_handle_process_list(V9xConnection *conn, DWORD request_id)
{
    V9xSnapshotFn snapshot;
    V9xProcessWalkFn first;
    V9xProcessWalkFn next;
    PROCESSENTRY32 entry;
    HANDLE list;
    DWORD self = GetCurrentProcessId();
    unsigned long offset = 4ul;
    unsigned long count = 0ul;
    if (!v9x_toolhelp(&snapshot, &first, &next)) {
        return v9x_tools_error(conn, request_id, V9X_STATUS_UNSUPPORTED_OPERATION,
                               ERROR_CALL_NOT_IMPLEMENTED, "no toolhelp on this Windows");
    }
    list = snapshot(TH32CS_SNAPPROCESS, 0ul);
    if (list == INVALID_HANDLE_VALUE) {
        DWORD native_error = GetLastError();
        return v9x_tools_error(conn, request_id, V9X_STATUS_IO_FAILED,
                               native_error, "process snapshot failed");
    }
    entry.dwSize = sizeof(entry);
    if (first(list, &entry)) {
        do {
            DWORD flags = 0ul;
            DWORD job = 0ul;
            if (entry.th32ProcessID == self) flags |= V9X_PROCESS_AGENT;
            if (v9x_agent_job(conn->machine, entry.th32ProcessID, &job)) {
                flags |= V9X_PROCESS_AGENT_JOB;
            }
            if (offset + 20ul > sizeof(conn->file_response)) break;
            v9x_write_u32(conn->file_response + offset, entry.th32ProcessID);
            v9x_write_u32(conn->file_response + offset + 4, entry.th32ParentProcessID);
            v9x_write_u32(conn->file_response + offset + 8, entry.cntThreads);
            v9x_write_u32(conn->file_response + offset + 12, flags);
            v9x_write_u32(conn->file_response + offset + 16, job);
            offset += 20ul;
            /* Full path on 9x, bare name on NT: send it as the system gave it. */
            if (!v9x_append_string(conn->file_response, sizeof(conn->file_response),
                                   &offset, entry.szExeFile)) {
                offset -= 20ul;
                break;
            }
            ++count;
            entry.dwSize = sizeof(entry);
        } while (next(list, &entry));
    }
    CloseHandle(list);
    v9x_write_u32(conn->file_response, count);
    return v9x_send_frame(conn, V9X_MSG_PROCESS_LIST_RESPONSE, request_id,
                          conn->file_response, offset);
}

static int v9x_handle_process_kill(V9xConnection *conn, DWORD request_id,
                                   const unsigned char *payload, unsigned long length)
{
    /* Killing these takes Windows down with it (9x: the 16-bit message
       server, the network provider host; NT: the session processes). */
    static const char *const protected_names[] = {
        "KERNEL32.DLL", "MSGSRV32.EXE", "MPREXE.EXE", "MMTASK.TSK",
        "SYSTEM", "SMSS.EXE", "CSRSS.EXE", "WINLOGON.EXE", "SERVICES.EXE",
        "LSASS.EXE"
    };
    char name[MAX_PATH];
    DWORD pid;
    DWORD exit_code;
    DWORD job = 0ul;
    DWORD native_error;
    HANDLE process;
    unsigned long index;
    if (length != 8ul) {
        return v9x_tools_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                               ERROR_INVALID_PARAMETER, "invalid kill request");
    }
    pid = v9x_read_u32(payload);
    exit_code = v9x_read_u32(payload + 4);
    if (pid == 0ul || pid == GetCurrentProcessId()) {
        return v9x_tools_error(conn, request_id, V9X_STATUS_ACCESS_DENIED,
                               ERROR_ACCESS_DENIED, "refused: the agent itself");
    }
    if (!v9x_process_name(pid, name, sizeof(name))) {
        return v9x_tools_error(conn, request_id, V9X_STATUS_NOT_FOUND,
                               ERROR_INVALID_PARAMETER, "no such process");
    }
    for (index = 0ul; index < sizeof(protected_names) / sizeof(protected_names[0]); ++index) {
        if (v9x_tools_same(name, protected_names[index])) {
            return v9x_tools_error(conn, request_id, V9X_STATUS_ACCESS_DENIED,
                                   ERROR_ACCESS_DENIED,
                                   "refused: Windows cannot run without this process");
        }
    }
    process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (process == 0) {
        native_error = GetLastError();
        return v9x_tools_error(conn, request_id, v9x_tools_status(native_error),
                               native_error, "open process failed");
    }
    (void)v9x_agent_job(conn->machine, pid, &job);
    if (!TerminateProcess(process, exit_code)) {
        native_error = GetLastError();
        CloseHandle(process);
        return v9x_tools_error(conn, request_id, V9X_STATUS_IO_FAILED,
                               native_error, "terminate failed");
    }
    CloseHandle(process);
    v9x_log_event("process-kill", name);
    v9x_write_u32(conn->response, pid);
    v9x_write_u32(conn->response + 4, job);
    return v9x_send_frame(conn, V9X_MSG_PROCESS_KILL_RESPONSE, request_id,
                          conn->response, 8ul);
}

/* ---- REG_QUERY ------------------------------------------------------ */

static int v9x_handle_reg_query(V9xConnection *conn, DWORD request_id,
                                const unsigned char *payload, unsigned long length)
{
    char key_path[MAX_PATH];
    char name[MAX_PATH];
    unsigned char *out = conn->file_response;
    unsigned long capacity = sizeof(conn->file_response);
    unsigned long offset = 12ul;
    unsigned long read_offset = 4ul;
    DWORD hive;
    DWORD index;
    DWORD subkeys = 0ul;
    DWORD values = 0ul;
    DWORD flags = 0ul;
    LONG result;
    HKEY key;
    FILETIME written;
    if (length < 6ul ||
        !v9x_tools_string(payload, length, &read_offset, key_path, MAX_PATH - 1ul, 1) ||
        read_offset != length) {
        return v9x_tools_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                               ERROR_INVALID_PARAMETER, "invalid registry request");
    }
    hive = v9x_read_u32(payload);
    if (hive < 0x80000000ul || hive > 0x80000006ul || hive == 0x80000004ul) {
        return v9x_tools_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                               ERROR_INVALID_PARAMETER, "unknown registry hive");
    }
    result = RegOpenKeyExA((HKEY)hive, key_path[0] != '\0' ? key_path : 0, 0ul,
                           KEY_READ, &key);
    if (result != ERROR_SUCCESS) {
        return v9x_tools_error(conn, request_id, v9x_tools_status((DWORD)result),
                               (DWORD)result, "registry key not found");
    }
    /* Subkey names, then values; stop (flagged) when the answer is full. */
    for (index = 0ul; ; ++index) {
        DWORD name_length = sizeof(name);
        result = RegEnumKeyExA(key, index, name, &name_length, 0, 0, 0, &written);
        if (result != ERROR_SUCCESS) break;
        if (!v9x_append_string(out, capacity, &offset, name)) {
            flags |= V9X_REG_TRUNCATED;
            break;
        }
        ++subkeys;
    }
    /* Layout: [subkeys][values][flags] (written last), the subkey strings,
       then per value: name string, type, length, data. */
    for (index = 0ul; (flags & V9X_REG_TRUNCATED) == 0ul; ++index) {
        DWORD name_length = sizeof(name);
        DWORD type = 0ul;
        DWORD data_length;
        unsigned long start = offset;
        if (offset + 2ul + sizeof(name) + 8ul > capacity) {
            flags |= V9X_REG_TRUNCATED;
            break;
        }
        data_length = (DWORD)(capacity - offset - 2ul - MAX_PATH - 8ul);
        result = RegEnumValueA(key, index, name, &name_length, 0, &type,
                               out + offset + 2ul + MAX_PATH + 8ul, &data_length);
        if (result == ERROR_NO_MORE_ITEMS) break;
        if (result == ERROR_MORE_DATA) {
            flags |= V9X_REG_TRUNCATED;
            break;
        }
        if (result != ERROR_SUCCESS) break;
        /* Pack: name string, type, length, data (the data was read past a
           MAX_PATH gap, so moving it down never overlaps the name). */
        if (!v9x_append_string(out, capacity, &offset, name)) {
            offset = start;
            flags |= V9X_REG_TRUNCATED;
            break;
        }
        {
            unsigned char *data = out + start + 2ul + MAX_PATH + 8ul;
            unsigned long move;
            v9x_write_u32(out + offset, type);
            v9x_write_u32(out + offset + 4, data_length);
            for (move = 0ul; move < data_length; ++move) {
                out[offset + 8ul + move] = data[move];
            }
            offset += 8ul + data_length;
        }
        ++values;
    }
    RegCloseKey(key);
    v9x_write_u32(out, subkeys);
    v9x_write_u32(out + 4, values);
    v9x_write_u32(out + 8, flags);
    return v9x_send_frame(conn, V9X_MSG_REG_QUERY_RESPONSE, request_id, out, offset);
}

/* ---- WINDOW_LIST ---------------------------------------------------- */

typedef struct V9xWindowWalk {
    unsigned char *out;
    unsigned long capacity;
    unsigned long offset;
    unsigned long count;
    HWND foreground;
    int full;
} V9xWindowWalk;

static BOOL CALLBACK v9x_window_entry(HWND window, LPARAM parameter)
{
    V9xWindowWalk *walk = (V9xWindowWalk *)parameter;
    char title[160];
    char class_name[64];
    RECT rect;
    DWORD pid = 0ul;
    DWORD flags = 0ul;
    DWORD copied = 0ul;
    unsigned long start = walk->offset;
    if (!IsWindowVisible(window)) return TRUE;
    /* WM_GETTEXT with a timeout: GetWindowText on another process's window
       would wait for ever on a hung program. */
    title[0] = '\0';
    if (SendMessageTimeoutA(window, WM_GETTEXT, sizeof(title), (LPARAM)title,
                            SMTO_ABORTIFHUNG, 500u, &copied) == 0) {
        title[0] = '\0';
    }
    title[sizeof(title) - 1u] = '\0';
    class_name[0] = '\0';
    (void)GetClassNameA(window, class_name, sizeof(class_name));
    if (!GetWindowRect(window, &rect)) {
        rect.left = rect.top = rect.right = rect.bottom = 0l;
    }
    (void)GetWindowThreadProcessId(window, &pid);
    if (window == walk->foreground) flags |= V9X_WINDOW_FOREGROUND;
    if (IsIconic(window)) flags |= V9X_WINDOW_MINIMIZED;
    if (walk->offset + 32ul > walk->capacity) {
        walk->full = 1;
        return FALSE;
    }
    v9x_write_u32(walk->out + walk->offset, (unsigned long)window);
    v9x_write_u32(walk->out + walk->offset + 4, pid);
    v9x_write_u32(walk->out + walk->offset + 8, flags);
    v9x_write_u32(walk->out + walk->offset + 12, (unsigned long)rect.left);
    v9x_write_u32(walk->out + walk->offset + 16, (unsigned long)rect.top);
    v9x_write_u32(walk->out + walk->offset + 20, (unsigned long)rect.right);
    v9x_write_u32(walk->out + walk->offset + 24, (unsigned long)rect.bottom);
    walk->offset += 28ul;
    if (!v9x_append_string(walk->out, walk->capacity, &walk->offset, class_name) ||
        !v9x_append_string(walk->out, walk->capacity, &walk->offset, title)) {
        walk->offset = start;
        walk->full = 1;
        return FALSE;
    }
    ++walk->count;
    return TRUE;
}

static int v9x_handle_window_list(V9xConnection *conn, DWORD request_id)
{
    V9xWindowWalk walk;
    walk.out = conn->file_response;
    walk.capacity = sizeof(conn->file_response);
    walk.offset = 8ul;
    walk.count = 0ul;
    walk.foreground = GetForegroundWindow();
    walk.full = 0;
    (void)EnumWindows(v9x_window_entry, (LPARAM)&walk);
    v9x_write_u32(conn->file_response, walk.count);
    v9x_write_u32(conn->file_response + 4, walk.full ? 1ul : 0ul);
    return v9x_send_frame(conn, V9X_MSG_WINDOW_LIST_RESPONSE, request_id,
                          conn->file_response, walk.offset);
}

/* ---- dispatch ------------------------------------------------------- */

int v9x_is_tools_message(unsigned short type)
{
    return type == V9X_MSG_FILE_DELETE_REQUEST || type == V9X_MSG_FILE_HASH_REQUEST ||
           type == V9X_MSG_FILE_READ_TEXT_REQUEST ||
           type == V9X_MSG_PROCESS_LIST_REQUEST || type == V9X_MSG_PROCESS_KILL_REQUEST ||
           type == V9X_MSG_REG_QUERY_REQUEST || type == V9X_MSG_WINDOW_LIST_REQUEST;
}

int v9x_handle_tools_message(V9xConnection *conn, unsigned short type,
                             unsigned long request_id,
                             const unsigned char *payload, unsigned long length)
{
    if (type == V9X_MSG_FILE_DELETE_REQUEST)
        return v9x_handle_delete(conn, request_id, payload, length);
    if (type == V9X_MSG_FILE_HASH_REQUEST)
        return v9x_handle_hash(conn, request_id, payload, length);
    if (type == V9X_MSG_FILE_READ_TEXT_REQUEST)
        return v9x_handle_read_text(conn, request_id, payload, length);
    if (type == V9X_MSG_PROCESS_LIST_REQUEST && length == 0ul)
        return v9x_handle_process_list(conn, request_id);
    if (type == V9X_MSG_PROCESS_KILL_REQUEST)
        return v9x_handle_process_kill(conn, request_id, payload, length);
    if (type == V9X_MSG_REG_QUERY_REQUEST)
        return v9x_handle_reg_query(conn, request_id, payload, length);
    if (type == V9X_MSG_WINDOW_LIST_REQUEST && length == 0ul)
        return v9x_handle_window_list(conn, request_id);
    return v9x_tools_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                           ERROR_INVALID_PARAMETER, "invalid request");
}
