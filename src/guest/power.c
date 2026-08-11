#include "agent.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"

static const char v9x_pending_path[] = "C:\\V9XREMOTE\\PENDING.DAT";
static unsigned char v9x_power_response[96];

static int v9x_power_error(V9xAgentState *state, DWORD request_id,
                           DWORD status, DWORD native_error,
                           const char *detail)
{
    unsigned long offset = 8ul;
    v9x_write_u32(v9x_power_response, status);
    v9x_write_u32(v9x_power_response + 4, native_error);
    if (!v9x_append_string(v9x_power_response, sizeof(v9x_power_response),
                           &offset, detail)) return 0;
    return v9x_send_frame(state, V9X_MSG_ERROR_RESPONSE, request_id,
                          v9x_power_response, offset);
}

static int v9x_read_job(const unsigned char *payload, unsigned long length,
                        char *job)
{
    unsigned long job_length;
    unsigned long index;
    if (length < 3ul) return 0;
    job_length = (unsigned long)v9x_read_u16(payload);
    if (job_length == 0ul || job_length > 63ul || job_length + 2ul != length) {
        return 0;
    }
    for (index = 0ul; index < job_length; ++index) {
        if (payload[index + 2ul] == 0u || payload[index + 2ul] > 0x7fu) {
            return 0;
        }
        job[index] = (char)payload[index + 2ul];
    }
    job[job_length] = '\0';
    return 1;
}

static int v9x_persist_job(const char *job, DWORD *native_error)
{
    HANDLE file;
    DWORD length = (DWORD)v9x_bounded_length(job, 64ul);
    DWORD written = 0ul;
    file = CreateFileA(v9x_pending_path, GENERIC_WRITE, FILE_SHARE_READ, 0,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        *native_error = GetLastError();
        return 0;
    }
    if (!WriteFile(file, job, length, &written, 0) || written != length ||
        !FlushFileBuffers(file)) {
        *native_error = GetLastError();
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);
    return 1;
}

void v9x_load_pending_job(V9xAgentState *state)
{
    HANDLE file;
    DWORD read_count = 0ul;
    DWORD index;
    state->pending_job[0] = '\0';
    file = CreateFileA(v9x_pending_path, GENERIC_READ, FILE_SHARE_READ, 0,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return;
    if (!ReadFile(file, state->pending_job, 63ul, &read_count, 0)) {
        read_count = 0ul;
    }
    CloseHandle(file);
    for (index = 0ul; index < read_count; ++index) {
        if (state->pending_job[index] == '\0' ||
            (unsigned char)state->pending_job[index] > 0x7fu) {
            read_count = 0ul;
            break;
        }
    }
    state->pending_job[read_count] = '\0';
}

int v9x_handle_power_message(V9xAgentState *state, unsigned short type,
                             unsigned long request_id,
                             const unsigned char *payload,
                             unsigned long length)
{
    char job[64];
    DWORD native_error = 0ul;
    unsigned long offset = 4ul;
    unsigned short response_type;
    UINT flags;
    if (InterlockedCompareExchange(&state->exec_active, 0l, 0l) != 0l) {
        return v9x_power_error(state, request_id, V9X_STATUS_BUSY, 0ul,
                               "execution active");
    }
    if (!v9x_read_job(payload, length, job)) {
        return v9x_power_error(state, request_id, V9X_STATUS_INVALID_PAYLOAD,
                               ERROR_INVALID_PARAMETER, "invalid resume token");
    }
    if (!v9x_persist_job(job, &native_error)) {
        return v9x_power_error(state, request_id, V9X_STATUS_IO_FAILED,
                               native_error, "resume token persistence failed");
    }
    {
        unsigned long index = 0ul;
        do {
            state->pending_job[index] = job[index];
        } while (job[index++] != '\0' && index < sizeof(state->pending_job));
    }
    v9x_files_disconnect();
    v9x_write_u32(v9x_power_response, state->boot_counter);
    if (!v9x_append_string(v9x_power_response, sizeof(v9x_power_response),
                           &offset, job)) return 0;
    if (type == V9X_MSG_REBOOT_REQUEST) {
        response_type = V9X_MSG_REBOOT_ACCEPTED;
        flags = EWX_REBOOT | EWX_FORCE;
        v9x_log_line("reboot-accepted");
    } else {
        response_type = V9X_MSG_SHUTDOWN_ACCEPTED;
        flags = EWX_SHUTDOWN | EWX_POWEROFF | EWX_FORCE;
        v9x_log_line("shutdown-accepted");
    }
    if (!v9x_send_frame(state, response_type, request_id,
                        v9x_power_response, offset)) return 0;
    Sleep(250ul);
    if (!ExitWindowsEx(flags, 0ul)) {
        v9x_log_line("power-request-failed");
    }
    return 1;
}
