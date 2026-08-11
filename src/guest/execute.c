#include "agent.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"

#define V9X_EXEC_MAX_APPLICATION 259ul
#define V9X_EXEC_MAX_COMMAND 2047ul
#define V9X_EXEC_MAX_DIRECTORY 259ul
#define V9X_EXEC_MAX_TIMEOUT 3600000ul
#define V9X_EXEC_MAX_OUTPUT 1048576ul
#define V9X_EXEC_CHUNK 1024ul

typedef struct V9xExecutionJob {
    V9xAgentState *state;
    DWORD request_id;
    DWORD timeout_ms;
    DWORD stdout_limit;
    DWORD stderr_limit;
    BYTE mode;
    BYTE show_window;
    char application[260];
    char command[2048];
    char directory[260];
} V9xExecutionJob;

static V9xExecutionJob v9x_job;

static void v9x_zero_bytes(void *target, unsigned long length)
{
    unsigned char *bytes = (unsigned char *)target;
    unsigned long index;
    for (index = 0ul; index < length; ++index) bytes[index] = 0u;
}

static int v9x_read_string(const unsigned char *payload, unsigned long length,
                           unsigned long *offset, char *target,
                           unsigned long maximum)
{
    unsigned long string_length;
    unsigned long index;
    if (*offset > length || length - *offset < 2ul) return 0;
    string_length = (unsigned long)v9x_read_u16(payload + *offset);
    *offset += 2ul;
    if (string_length > maximum || string_length > length - *offset) return 0;
    for (index = 0ul; index < string_length; ++index) {
        if (payload[*offset + index] == 0u || payload[*offset + index] > 0x7fu) {
            return 0;
        }
        target[index] = (char)payload[*offset + index];
    }
    target[string_length] = '\0';
    *offset += string_length;
    return 1;
}

static int v9x_contains_quote(const char *text)
{
    while (*text != '\0') {
        if (*text == '"') return 1;
        ++text;
    }
    return 0;
}

static int v9x_append_text(char *target, unsigned long capacity,
                           unsigned long *offset, const char *text)
{
    while (*text != '\0') {
        if (*offset + 1ul >= capacity) return 0;
        target[(*offset)++] = *text++;
    }
    target[*offset] = '\0';
    return 1;
}

static int v9x_build_command_line(const V9xExecutionJob *job, char *target,
                                  unsigned long capacity)
{
    unsigned long offset = 0ul;
    target[0] = '\0';
    if (job->mode == V9X_EXEC_MODE_SHELL) {
        return v9x_append_text(target, capacity, &offset, "COMMAND.COM /C ") &&
               v9x_append_text(target, capacity, &offset, job->command);
    }
    if (v9x_contains_quote(job->application)) return 0;
    if (!v9x_append_text(target, capacity, &offset, "\"") ||
        !v9x_append_text(target, capacity, &offset, job->application) ||
        !v9x_append_text(target, capacity, &offset, "\"")) return 0;
    if (job->command[0] != '\0') {
        if (!v9x_append_text(target, capacity, &offset, " ") ||
            !v9x_append_text(target, capacity, &offset, job->command)) return 0;
    }
    return 1;
}

/* A hidden GUI child never receives WM_PAINT, so applying the show-window
   flag to a GUI-subsystem executable deadlocks paint-driven test programs.
   Direct mode therefore reads the target's PE optional header and leaves a
   GUI child's initial window state to the application, matching START. */
static int v9x_application_is_gui(const char *path)
{
    HANDLE file;
    unsigned char header[96];
    DWORD read_count = 0ul;
    unsigned long pe_offset;
    int is_gui = 0;
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (ReadFile(file, header, 64ul, &read_count, 0) && read_count == 64ul &&
        header[0] == 'M' && header[1] == 'Z') {
        pe_offset = v9x_read_u32(header + 60);
        if (pe_offset >= 64ul && pe_offset <= 0x08000000ul &&
            SetFilePointer(file, (LONG)pe_offset, 0, FILE_BEGIN) == pe_offset &&
            ReadFile(file, header, 96ul, &read_count, 0) && read_count == 96ul &&
            header[0] == 'P' && header[1] == 'E' &&
            header[2] == 0u && header[3] == 0u &&
            v9x_read_u16(header + 20) >= 70u &&
            v9x_read_u16(header + 24) == 0x010bu) {
            is_gui = v9x_read_u16(header + 92) == 2u;
        }
    }
    CloseHandle(file);
    return is_gui;
}

static int v9x_send_pipe_data(V9xExecutionJob *job, HANDLE pipe_handle,
                              unsigned short message_type, DWORD limit,
                              DWORD *total, DWORD *sent_total, DWORD *flags,
                              DWORD truncate_flag)
{
    unsigned char buffer[V9X_EXEC_CHUNK];
    DWORD available = 0ul;
    DWORD read_count = 0ul;
    DWORD send_count;
    if (!PeekNamedPipe(pipe_handle, 0, 0ul, 0, &available, 0) || available == 0ul) {
        return 1;
    }
    if (available > sizeof(buffer)) available = sizeof(buffer);
    if (!ReadFile(pipe_handle, buffer, available, &read_count, 0)) return 1;
    if (0xfffffffful - *total < read_count) *total = 0xfffffffful;
    else *total += read_count;
    send_count = read_count;
    if (*sent_total >= limit) send_count = 0ul;
    else if (send_count > limit - *sent_total) send_count = limit - *sent_total;
    if (send_count != 0ul) {
        if (!v9x_send_frame(job->state, message_type, job->request_id,
                            buffer, send_count)) return 0;
        *sent_total += send_count;
    }
    if (send_count != read_count) *flags |= truncate_flag;
    return 1;
}

static DWORD WINAPI v9x_execution_worker(LPVOID parameter)
{
    V9xExecutionJob *job = (V9xExecutionJob *)parameter;
    SECURITY_ATTRIBUTES security;
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    HANDLE stdout_read = 0;
    HANDLE stdout_write = 0;
    HANDLE stderr_read = 0;
    HANDLE stderr_write = 0;
    HANDLE stdin_handle = INVALID_HANDLE_VALUE;
    DWORD result = V9X_EXEC_RESULT_INTERNAL;
    DWORD native_error = 0ul;
    DWORD exit_code = 0xfffffffful;
    DWORD flags = V9X_EXEC_FLAG_PIPE_CAPTURE;
    DWORD stdout_total = 0ul;
    DWORD stderr_total = 0ul;
    DWORD stdout_sent = 0ul;
    DWORD stderr_sent = 0ul;
    DWORD started = GetTickCount();
    DWORD wait_result;
    DWORD drain_count;
    unsigned char complete[28];
    char command_line[2320];
    BOOL created = FALSE;

    v9x_zero_bytes(&security, sizeof(security));
    v9x_zero_bytes(&startup, sizeof(startup));
    v9x_zero_bytes(&process, sizeof(process));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    if (job->mode == V9X_EXEC_MODE_DIRECT &&
        v9x_application_is_gui(job->application)) {
        flags |= V9X_EXEC_FLAG_GUI_WINDOW;
    } else {
        startup.dwFlags |= STARTF_USESHOWWINDOW;
        startup.wShowWindow = job->show_window ? SW_SHOWNORMAL : SW_HIDE;
    }

    if (!v9x_build_command_line(job, command_line, sizeof(command_line))) {
        native_error = ERROR_INVALID_PARAMETER;
        result = V9X_EXEC_RESULT_CREATE_FAILED;
        goto complete_job;
    }
    v9x_log_line("exec-start");
    if (!CreatePipe(&stdout_read, &stdout_write, &security, 4096ul) ||
        !CreatePipe(&stderr_read, &stderr_write, &security, 4096ul)) {
        native_error = GetLastError();
        result = V9X_EXEC_RESULT_CREATE_FAILED;
        goto complete_job;
    }
    stdin_handle = CreateFileA("NUL", GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (stdin_handle == INVALID_HANDLE_VALUE) {
        native_error = GetLastError();
        result = V9X_EXEC_RESULT_CREATE_FAILED;
        goto complete_job;
    }
    startup.hStdInput = stdin_handle;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stderr_write;
    created = CreateProcessA(job->mode == V9X_EXEC_MODE_DIRECT ?
                                 job->application : 0,
                             command_line, 0, 0, TRUE, 0ul, 0,
                             job->directory[0] != '\0' ? job->directory : 0,
                             &startup, &process);
    native_error = created ? 0ul : GetLastError();
    CloseHandle(stdin_handle); stdin_handle = INVALID_HANDLE_VALUE;
    CloseHandle(stdout_write); stdout_write = 0;
    CloseHandle(stderr_write); stderr_write = 0;
    if (!created) {
        result = V9X_EXEC_RESULT_CREATE_FAILED;
        goto complete_job;
    }
    CloseHandle(process.hThread); process.hThread = 0;

    result = V9X_EXEC_RESULT_OK;
    for (;;) {
        if (!v9x_send_pipe_data(job, stdout_read, V9X_MSG_EXEC_STDOUT,
                                job->stdout_limit, &stdout_total, &stdout_sent,
                                &flags, V9X_EXEC_FLAG_STDOUT_TRUNCATED) ||
            !v9x_send_pipe_data(job, stderr_read, V9X_MSG_EXEC_STDERR,
                                job->stderr_limit, &stderr_total, &stderr_sent,
                                &flags, V9X_EXEC_FLAG_STDERR_TRUNCATED)) {
            InterlockedExchange(&job->state->exec_cancel, 1l);
        }
        if (InterlockedCompareExchange(&job->state->exec_cancel, 0l, 0l) != 0l) {
            flags |= V9X_EXEC_FLAG_CANCELLED;
            result = V9X_EXEC_RESULT_CANCELLED;
            (void)TerminateProcess(process.hProcess, 0xfffffffeul);
            break;
        }
        if (job->timeout_ms != 0ul && GetTickCount() - started >= job->timeout_ms) {
            flags |= V9X_EXEC_FLAG_TIMED_OUT;
            result = V9X_EXEC_RESULT_TIMEOUT;
            (void)TerminateProcess(process.hProcess, 0xfffffffdul);
            break;
        }
        wait_result = WaitForSingleObject(process.hProcess, 20ul);
        if (wait_result == WAIT_OBJECT_0) break;
        if (wait_result == WAIT_FAILED) {
            native_error = GetLastError();
            result = V9X_EXEC_RESULT_INTERNAL;
            (void)TerminateProcess(process.hProcess, 0xfffffffcul);
            break;
        }
    }
    (void)WaitForSingleObject(process.hProcess, 5000ul);
    for (drain_count = 0ul; drain_count < 32ul; ++drain_count) {
        DWORD before_stdout = stdout_total;
        DWORD before_stderr = stderr_total;
        (void)v9x_send_pipe_data(job, stdout_read, V9X_MSG_EXEC_STDOUT,
                                 job->stdout_limit, &stdout_total, &stdout_sent,
                                 &flags, V9X_EXEC_FLAG_STDOUT_TRUNCATED);
        (void)v9x_send_pipe_data(job, stderr_read, V9X_MSG_EXEC_STDERR,
                                 job->stderr_limit, &stderr_total, &stderr_sent,
                                 &flags, V9X_EXEC_FLAG_STDERR_TRUNCATED);
        if (before_stdout == stdout_total && before_stderr == stderr_total) break;
    }
    if (!GetExitCodeProcess(process.hProcess, &exit_code) && native_error == 0ul) {
        native_error = GetLastError();
        result = V9X_EXEC_RESULT_INTERNAL;
    }

complete_job:
    if (stdout_write != 0) CloseHandle(stdout_write);
    if (stderr_write != 0) CloseHandle(stderr_write);
    if (stdin_handle != INVALID_HANDLE_VALUE) CloseHandle(stdin_handle);
    if (stdout_read != 0) CloseHandle(stdout_read);
    if (stderr_read != 0) CloseHandle(stderr_read);
    if (process.hThread != 0) CloseHandle(process.hThread);
    if (process.hProcess != 0) CloseHandle(process.hProcess);
    v9x_write_u32(complete, result);
    v9x_write_u32(complete + 4, exit_code);
    v9x_write_u32(complete + 8, native_error);
    v9x_write_u32(complete + 12, GetTickCount() - started);
    v9x_write_u32(complete + 16, stdout_total);
    v9x_write_u32(complete + 20, stderr_total);
    v9x_write_u32(complete + 24, flags);
    (void)v9x_send_frame(job->state, V9X_MSG_EXEC_COMPLETE, job->request_id,
                         complete, sizeof(complete));
    v9x_log_line("exec-complete");
    InterlockedExchange(&job->state->exec_active, 0l);
    return 0ul;
}

unsigned long v9x_execution_prepare(V9xAgentState *state,
                                    unsigned long request_id,
                                    const unsigned char *payload,
                                    unsigned long length)
{
    unsigned long offset = 16ul;
    DWORD thread_id = 0ul;
    if (length < 22ul) return V9X_STATUS_INVALID_PAYLOAD;
    if (InterlockedCompareExchange(&state->exec_active, 1l, 0l) != 0l) {
        return V9X_STATUS_BUSY;
    }
    if (state->exec_thread != 0) {
        CloseHandle(state->exec_thread);
        state->exec_thread = 0;
    }
    v9x_zero_bytes(&v9x_job, sizeof(v9x_job));
    v9x_job.state = state;
    v9x_job.request_id = request_id;
    v9x_job.mode = payload[0];
    v9x_job.show_window = payload[1];
    v9x_job.timeout_ms = v9x_read_u32(payload + 4);
    v9x_job.stdout_limit = v9x_read_u32(payload + 8);
    v9x_job.stderr_limit = v9x_read_u32(payload + 12);
    if (v9x_read_u16(payload + 2) != 0u ||
        v9x_job.mode > V9X_EXEC_MODE_SHELL || v9x_job.show_window > 1u ||
        v9x_job.timeout_ms > V9X_EXEC_MAX_TIMEOUT ||
        v9x_job.stdout_limit > V9X_EXEC_MAX_OUTPUT ||
        v9x_job.stderr_limit > V9X_EXEC_MAX_OUTPUT ||
        !v9x_read_string(payload, length, &offset, v9x_job.application,
                         V9X_EXEC_MAX_APPLICATION) ||
        !v9x_read_string(payload, length, &offset, v9x_job.command,
                         V9X_EXEC_MAX_COMMAND) ||
        !v9x_read_string(payload, length, &offset, v9x_job.directory,
                         V9X_EXEC_MAX_DIRECTORY) || offset != length ||
        (v9x_job.mode == V9X_EXEC_MODE_DIRECT && v9x_job.application[0] == '\0') ||
        (v9x_job.mode == V9X_EXEC_MODE_SHELL && v9x_job.command[0] == '\0')) {
        InterlockedExchange(&state->exec_active, 0l);
        return V9X_STATUS_INVALID_PAYLOAD;
    }
    state->exec_request_id = request_id;
    InterlockedExchange(&state->exec_cancel, 0l);
    state->exec_thread = CreateThread(0, 65536ul, v9x_execution_worker,
                                      &v9x_job, CREATE_SUSPENDED, &thread_id);
    if (state->exec_thread == 0) {
        InterlockedExchange(&state->exec_active, 0l);
        return V9X_STATUS_CREATE_FAILED;
    }
    return V9X_STATUS_OK;
}

int v9x_execution_resume(V9xAgentState *state)
{
    if (state->exec_thread != 0 && ResumeThread(state->exec_thread) != 0xfffffffful) {
        return 1;
    }
    if (state->exec_thread != 0) {
        (void)TerminateThread(state->exec_thread, 1ul);
        CloseHandle(state->exec_thread);
        state->exec_thread = 0;
    }
    InterlockedExchange(&state->exec_active, 0l);
    return 0;
}

int v9x_execution_cancel(V9xAgentState *state, unsigned long request_id)
{
    if (InterlockedCompareExchange(&state->exec_active, 0l, 0l) == 0l ||
        state->exec_request_id != request_id) return 0;
    InterlockedExchange(&state->exec_cancel, 1l);
    return 1;
}

void v9x_execution_disconnect(V9xAgentState *state)
{
    if (InterlockedCompareExchange(&state->exec_active, 0l, 0l) != 0l) {
        InterlockedExchange(&state->exec_cancel, 1l);
        if (state->exec_thread != 0 &&
            WaitForSingleObject(state->exec_thread, 5000ul) == WAIT_TIMEOUT) {
            (void)TerminateThread(state->exec_thread, 1ul);
            InterlockedExchange(&state->exec_active, 0l);
        }
    }
    if (state->exec_thread != 0) {
        CloseHandle(state->exec_thread);
        state->exec_thread = 0;
    }
}
