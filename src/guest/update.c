#include "agent.h"
#include "v9xremote/crc32.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"

/* Hot update: apply a new agent (and screenshot helper) without a reboot. A
   running .EXE is locked on Win9x, so the agent cannot overwrite itself in
   place. Instead it verifies the already-uploaded staged binaries, writes a
   small hot-swap batch, launches it detached, and exits; the batch spins until
   the agent has released the lock, swaps the files, and relaunches the agent.
   The RunServices entry is never repointed, so an interrupted swap still
   recovers on the next reboot. */

static const char v9x_update_agent_new[] = "C:\\V9XREMOTE\\V9XNEW.EXE";
static const char v9x_update_helper_new[] = "C:\\V9XREMOTE\\V9XSNEW.EXE";
static const char v9x_update_script[] = "C:\\V9XREMOTE\\HOTSWAP.BAT";

/* Batch is authored with explicit CRLF line endings: Win9x COMMAND.COM mangles
   bare-LF batch files.

   The wait loop must NOT rely on COPY's exit code: Win9x COMMAND.COM COPY /Y
   returns errorlevel 0 even when it silently fails to overwrite a locked,
   running .EXE, so a COPY-based spin does not actually wait and the swap is
   skipped. Instead it renames the running agent aside - a rename of a mapped
   executable fails while the process holds it and succeeds the instant it
   exits - and detects success with IF EXIST, the one primitive Win9x
   COMMAND.COM evaluates reliably (the same trick INSTALL.BAT uses). Only once
   V9XAGNT.EXE is gone (the old process has fully exited, releasing both the file
   lock and the singleton mutex) does it copy the new binary in and relaunch. */
static const char v9x_update_batch[] =
    "@ECHO OFF\r\n"
    "ECHO hotswap-start > C:\\V9XREMOTE\\HOTSWAP.LOG\r\n"
    ":WAIT\r\n"
    "IF NOT EXIST C:\\V9XREMOTE\\V9XAGNT.EXE GOTO SWAP\r\n"
    "REN C:\\V9XREMOTE\\V9XAGNT.EXE V9XAGNT.OLD\r\n"
    "IF EXIST C:\\V9XREMOTE\\V9XAGNT.EXE GOTO WAIT\r\n"
    ":SWAP\r\n"
    "ECHO agent-exited >> C:\\V9XREMOTE\\HOTSWAP.LOG\r\n"
    "COPY /Y C:\\V9XREMOTE\\V9XNEW.EXE C:\\V9XREMOTE\\V9XAGNT.EXE\r\n"
    "IF EXIST C:\\V9XREMOTE\\V9XAGNT.OLD DEL C:\\V9XREMOTE\\V9XAGNT.OLD\r\n"
    "COPY /Y C:\\V9XREMOTE\\V9XSNEW.EXE C:\\V9XREMOTE\\V9XSHOT.EXE\r\n"
    "DEL C:\\V9XREMOTE\\V9XNEW.EXE\r\n"
    "DEL C:\\V9XREMOTE\\V9XSNEW.EXE\r\n"
    "REGEDIT /S C:\\V9XREMOTE\\INSTALL.REG\r\n"
    "ECHO agent-relaunch >> C:\\V9XREMOTE\\HOTSWAP.LOG\r\n"
    "C:\\V9XREMOTE\\V9XAGNT.EXE -service\r\n";

static int v9x_update_error(V9xConnection *conn, DWORD request_id, DWORD status,
                            DWORD native_error, const char *detail)
{
    unsigned long offset = 8ul;
    v9x_write_u32(conn->power_response, status);
    v9x_write_u32(conn->power_response + 4, native_error);
    if (!v9x_append_string(conn->power_response, sizeof(conn->power_response),
                           &offset, detail)) return 0;
    return v9x_send_frame(conn, V9X_MSG_ERROR_RESPONSE, request_id,
                          conn->power_response, offset);
}

/* Confirm a staged binary exists, matches the declared size and CRC32, and
   begins with an MZ header. The caller's file_buffer is reused as scratch. */
static int v9x_update_verify(V9xConnection *conn, const char *path,
                             DWORD expected_size, DWORD expected_crc)
{
    HANDLE file;
    DWORD high = 0ul;
    DWORD size;
    DWORD read_count;
    DWORD running_crc = v9x_crc32_begin();
    DWORD total = 0ul;
    int first_chunk = 1;
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(file, &high);
    if (high != 0ul || size == 0xfffffffful || size != expected_size ||
        size < 2ul) {
        CloseHandle(file);
        return 0;
    }
    for (;;) {
        if (!ReadFile(file, conn->file_buffer, V9X_FILE_CHUNK_SIZE,
                      &read_count, 0)) {
            CloseHandle(file);
            return 0;
        }
        if (read_count == 0ul) break;
        if (first_chunk) {
            if (conn->file_buffer[0] != 'M' || conn->file_buffer[1] != 'Z') {
                CloseHandle(file);
                return 0;
            }
            first_chunk = 0;
        }
        running_crc = v9x_crc32_update(running_crc, conn->file_buffer, read_count);
        total += read_count;
    }
    CloseHandle(file);
    if (total != expected_size) return 0;
    return v9x_crc32_end(running_crc) == expected_crc;
}

static int v9x_update_write_script(DWORD *native_error)
{
    HANDLE file;
    DWORD written = 0ul;
    DWORD length = (DWORD)(sizeof(v9x_update_batch) - 1ul);
    file = CreateFileA(v9x_update_script, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        *native_error = GetLastError();
        return 0;
    }
    if (!WriteFile(file, v9x_update_batch, length, &written, 0) ||
        written != length || !FlushFileBuffers(file)) {
        *native_error = GetLastError();
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    return 1;
}

/* Launch the hot-swap batch detached so it outlives this process. On Win9x a
   child is independent of its parent, so the batch keeps running after the agent
   calls ExitProcess. The agent runs from RunServices with no console and no
   standard handles, so - exactly like the detached-exec path in execute.c -
   COMMAND.COM must be handed inheritable NUL std handles, or it fails to run the
   batch at all. */
static int v9x_update_launch(DWORD *native_error)
{
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    SECURITY_ATTRIBUTES security;
    HANDLE nul;
    char command_line[96];
    unsigned long index;
    static const char prefix[] = "COMMAND.COM /C ";
    for (index = 0ul; index < sizeof(security); ++index)
        ((unsigned char *)&security)[index] = 0u;
    for (index = 0ul; index < sizeof(startup); ++index)
        ((unsigned char *)&startup)[index] = 0u;
    for (index = 0ul; index < sizeof(process); ++index)
        ((unsigned char *)&process)[index] = 0u;
    for (index = 0ul; prefix[index] != '\0'; ++index)
        command_line[index] = prefix[index];
    {
        unsigned long at = index;
        for (index = 0ul; v9x_update_script[index] != '\0'; ++index)
            command_line[at + index] = v9x_update_script[index];
        command_line[at + index] = '\0';
    }
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (nul == INVALID_HANDLE_VALUE) {
        *native_error = GetLastError();
        return 0;
    }
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nul;
    startup.hStdOutput = nul;
    startup.hStdError = nul;
    if (!CreateProcessA(0, command_line, 0, 0, TRUE,
                        CREATE_NEW_PROCESS_GROUP, 0, "C:\\V9XREMOTE",
                        &startup, &process)) {
        *native_error = GetLastError();
        CloseHandle(nul);
        return 0;
    }
    CloseHandle(nul);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 1;
}

int v9x_handle_update(V9xConnection *conn, unsigned long request_id,
                      const unsigned char *payload, unsigned long length)
{
    DWORD agent_size;
    DWORD agent_crc;
    DWORD helper_size;
    DWORD helper_crc;
    DWORD native_error = 0ul;
    if (v9x_execution_any_active(conn->machine)) {
        return v9x_update_error(conn, request_id, V9X_STATUS_BUSY, 0ul,
                                "execution active");
    }
    if (conn->write_state.handle != 0 &&
        conn->write_state.handle != INVALID_HANDLE_VALUE) {
        return v9x_update_error(conn, request_id, V9X_STATUS_BUSY, 0ul,
                                "upload active");
    }
    if (length != 16ul) {
        return v9x_update_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                                ERROR_INVALID_PARAMETER, "invalid update request");
    }
    agent_size = v9x_read_u32(payload);
    agent_crc = v9x_read_u32(payload + 4);
    helper_size = v9x_read_u32(payload + 8);
    helper_crc = v9x_read_u32(payload + 12);
    if (!v9x_update_verify(conn, v9x_update_agent_new, agent_size, agent_crc)) {
        return v9x_update_error(conn, request_id, V9X_STATUS_CRC_MISMATCH,
                                ERROR_CRC, "staged agent binary invalid");
    }
    if (!v9x_update_verify(conn, v9x_update_helper_new, helper_size, helper_crc)) {
        return v9x_update_error(conn, request_id, V9X_STATUS_CRC_MISMATCH,
                                ERROR_CRC, "staged helper binary invalid");
    }
    if (!v9x_update_write_script(&native_error)) {
        return v9x_update_error(conn, request_id, V9X_STATUS_IO_FAILED,
                                native_error, "hot-swap script write failed");
    }
    if (!v9x_update_launch(&native_error)) {
        return v9x_update_error(conn, request_id, V9X_STATUS_CREATE_FAILED,
                                native_error, "hot-swap launch failed");
    }
    v9x_write_u32(conn->power_response, conn->machine->boot_counter);
    v9x_write_u32(conn->power_response + 4, agent_size);
    v9x_log_event("update-accepted", "hot-swap launched");
    if (!v9x_send_frame(conn, V9X_MSG_UPDATE_ACCEPTED, request_id,
                        conn->power_response, 8ul)) {
        return 0;
    }
    /* Give the accepted frame time to flush and the hot-swap child time to
       spawn, then exit so the batch can replace the now-unlocked executable and
       relaunch it. */
    Sleep(1000ul);
    ExitProcess(0ul);
    return 1;
}
