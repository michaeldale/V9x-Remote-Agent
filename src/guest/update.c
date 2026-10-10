#include "agent.h"
#include "platform_win.h"
#include "v9xremote/crc32.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"

/* Hot update: apply a new agent (and screenshot helper) without a reboot. A
   running .EXE is locked on Win9x, so the agent cannot overwrite itself in
   place. Instead it verifies the already-uploaded staged binaries, launches
   the staged agent binary itself with "-hotswap" (entry.c routes that into
   v9x_hotswap_run below), and exits. The swapper spins until the agent has
   released the lock, swaps the files, re-asserts the RunServices entry, and
   relaunches the agent. The RunServices entry is never repointed, so an
   interrupted swap still recovers on the next reboot. On NT the entry lives
   in the Run key instead (v9x_platform_run_key), and SETUP.EXE stages an
   update for the next logon by pointing Run at "V9XNEW.EXE -hotswap": no
   agent is running then, so the swap is immediate.

   Until 0.6.4 the swapper was a COMMAND.COM batch. That creates a DOS
   virtual machine, hidden or not, and some display drivers (the Velocity9x
   GMA driver on the netbook) corrupt the screen whenever one starts. The
   staged binary is a Win32 GUI-subsystem PE, so running it needs no VM. */

static const char v9x_update_agent[] = "C:\\V9XREMOTE\\V9XAGNT.EXE";
static const char v9x_update_agent_old[] = "C:\\V9XREMOTE\\V9XAGNT.OLD";
static const char v9x_update_agent_new[] = "C:\\V9XREMOTE\\V9XNEW.EXE";
static const char v9x_update_helper[] = "C:\\V9XREMOTE\\V9XSHOT.EXE";
static const char v9x_update_helper_new[] = "C:\\V9XREMOTE\\V9XSNEW.EXE";
static const char v9x_update_log[] = "C:\\V9XREMOTE\\HOTSWAP.LOG";
static const char v9x_update_relaunch[] = "C:\\V9XREMOTE\\V9XAGNT.EXE -service";
static const char v9x_update_swap_command[] = "C:\\V9XREMOTE\\V9XNEW.EXE -hotswap";
static const char v9x_update_run_value[] = "V9xRemoteAgent";

/* How long the swapper waits for the old agent to release its executable.
   The agent exits about a second after accepting the update. */
#define V9X_HOTSWAP_WAIT_MS 120000ul
#define V9X_HOTSWAP_POLL_MS 250ul

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

/* Launch a process with inheritable NUL standard handles. The agent runs from
   RunServices with no console and no standard handles, and a child launched
   without any at all fails on Win9x, exactly as the detached-exec path in
   execute.c found. Both the swapper and the relaunched agent go through
   here. The command line must be writable (CreateProcessA may modify it). */
static int v9x_update_spawn(const char *command, DWORD *native_error)
{
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    SECURITY_ATTRIBUTES security;
    HANDLE nul;
    char command_line[96];
    unsigned long index;
    for (index = 0ul; index < sizeof(security); ++index)
        ((unsigned char *)&security)[index] = 0u;
    for (index = 0ul; index < sizeof(startup); ++index)
        ((unsigned char *)&startup)[index] = 0u;
    for (index = 0ul; index < sizeof(process); ++index)
        ((unsigned char *)&process)[index] = 0u;
    for (index = 0ul; command[index] != '\0' && index + 1ul < sizeof(command_line); ++index)
        command_line[index] = command[index];
    command_line[index] = '\0';
    /* NT runs a child with no standard handles happily, and inheriting
       nothing there means no socket can leak across the handover. That
       includes the listening socket of an agent built before sockets were
       made non-inheritable, which leaked into the swapper: relaunching without inheritance lets this swapper
       exit with it, and the new agent's bind retry then succeeds. */
    if (v9x_platform_is_nt()) {
        startup.cb = sizeof(startup);
        if (!CreateProcessA(0, command_line, 0, 0, FALSE,
                            CREATE_NEW_PROCESS_GROUP, 0, "C:\\V9XREMOTE",
                            &startup, &process)) {
            *native_error = GetLastError();
            return 0;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 1;
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
    /* A fresh log per swap, as the batch used to write. */
    (void)DeleteFileA(v9x_update_log);
    if (!v9x_update_spawn(v9x_update_swap_command, &native_error)) {
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
    /* Give the accepted frame time to flush and the swapper time to spawn,
       then exit so it can replace the now-unlocked executable and relaunch
       it. */
    v9x_mdns_goodbye();
    Sleep(1000ul);
    ExitProcess(0ul);
    return 1;
}

/* ---- The swapper: this process is V9XNEW.EXE running with -hotswap. ---- */

static void v9x_hotswap_log(const char *line)
{
    HANDLE file;
    DWORD written = 0ul;
    unsigned long length = 0ul;
    file = CreateFileA(v9x_update_log, GENERIC_WRITE, FILE_SHARE_READ, 0,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return;
    (void)SetFilePointer(file, 0l, 0, FILE_END);
    while (line[length] != '\0') ++length;
    (void)WriteFile(file, line, length, &written, 0);
    (void)WriteFile(file, "\r\n", 2ul, &written, 0);
    CloseHandle(file);
}

/* Re-assert the RunServices entry natively, which is what the batch did
   with REGEDIT /S INSTALL.REG. Failure is logged, not fatal: the entry
   is already there on any guest that was installed with INSTALL.BAT. On
   NT it is the Run entry, logged as "run-*". */
static void v9x_hotswap_register(void)
{
    HKEY key;
    DWORD disposition = 0ul;
    LONG status;
    int nt = v9x_platform_is_nt();
    status = RegCreateKeyExA(HKEY_LOCAL_MACHINE, v9x_platform_run_key(), 0ul, 0,
                             REG_OPTION_NON_VOLATILE, KEY_WRITE, 0, &key,
                             &disposition);
    if (status != ERROR_SUCCESS) {
        v9x_hotswap_log(nt ? "run-open-failed" : "runservices-open-failed");
        return;
    }
    status = RegSetValueExA(key, v9x_update_run_value, 0ul, REG_SZ,
                            (const BYTE *)v9x_update_relaunch,
                            (DWORD)(sizeof(v9x_update_relaunch)));
    RegCloseKey(key);
    if (status == ERROR_SUCCESS) {
        v9x_hotswap_log(nt ? "run-set" : "runservices-set");
    } else {
        v9x_hotswap_log(nt ? "run-set-failed" : "runservices-set-failed");
    }
}

int v9x_hotswap_run(void)
{
    unsigned long waited = 0ul;
    DWORD native_error = 0ul;
    v9x_hotswap_log("hotswap-start");

    /* Wait for the old agent to exit. A rename of a mapped executable fails
       while the process holds it and succeeds the instant it exits, so the
       rename is both the wait and the first step of the swap. Once
       V9XAGNT.EXE is gone the old process has fully exited, releasing both
       the file lock and the singleton mutex (the relaunched agent tolerates
       a briefly lingering mutex on its own). */
    for (;;) {
        if (GetFileAttributesA(v9x_update_agent) == 0xfffffffful) break;
        (void)DeleteFileA(v9x_update_agent_old);
        if (MoveFileA(v9x_update_agent, v9x_update_agent_old)) break;
        if (waited >= V9X_HOTSWAP_WAIT_MS) {
            v9x_hotswap_log("agent-still-running");
            return 1;
        }
        Sleep(V9X_HOTSWAP_POLL_MS);
        waited += V9X_HOTSWAP_POLL_MS;
    }
    /* NT, unlike Win9x, lets a running executable be renamed, so there the
       rename above proves nothing. Wait for the outgoing agent's singleton
       mutex to disappear instead: OpenMutexA fails once its last handle has
       closed, which is when the process has exited. */
    if (v9x_platform_is_nt()) {
        for (;;) {
            HANDLE mutex = OpenMutexA(SYNCHRONIZE, FALSE, V9X_AGENT_MUTEX_NAME);
            if (mutex == 0) break;
            CloseHandle(mutex);
            if (waited >= V9X_HOTSWAP_WAIT_MS) {
                /* Put the binary back; the old agent is still serving. */
                (void)MoveFileA(v9x_update_agent_old, v9x_update_agent);
                v9x_hotswap_log("agent-still-running");
                return 1;
            }
            Sleep(V9X_HOTSWAP_POLL_MS);
            waited += V9X_HOTSWAP_POLL_MS;
        }
    }
    v9x_hotswap_log("agent-exited");

    /* This process is V9XNEW.EXE, so copying it is reading a mapped image,
       which Win9x allows; deleting it is not, so the relaunched agent does
       that (v9x_hotswap_cleanup). */
    if (!CopyFileA(v9x_update_agent_new, v9x_update_agent, FALSE)) {
        v9x_hotswap_log("agent-copy-failed");
        /* Put the old binary back so RunServices still finds an agent. */
        (void)MoveFileA(v9x_update_agent_old, v9x_update_agent);
        return 2;
    }
    (void)DeleteFileA(v9x_update_agent_old);
    if (CopyFileA(v9x_update_helper_new, v9x_update_helper, FALSE)) {
        (void)DeleteFileA(v9x_update_helper_new);
    } else {
        v9x_hotswap_log("helper-copy-failed");
    }
    v9x_hotswap_register();
    v9x_hotswap_log("agent-relaunch");
    if (!v9x_update_spawn(v9x_update_relaunch, &native_error)) {
        v9x_hotswap_log("agent-relaunch-failed");
        return 3;
    }
    return 0;
}

/* Called by the agent at start: remove the staged binary the swapper could
   not delete because it was running from it. The swapper exits right after
   spawning us, so a few short retries cover the race; a leftover is
   harmless and is overwritten by the next update anyway. */
void v9x_hotswap_cleanup(void)
{
    int attempt;
    for (attempt = 0; attempt < 10; ++attempt) {
        if (GetFileAttributesA(v9x_update_agent_new) == 0xfffffffful) return;
        if (DeleteFileA(v9x_update_agent_new)) return;
        Sleep(200ul);
    }
}
