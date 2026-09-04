/* Command execution for the DOS agent.

   The Win9x agent (src\guest\execute.c) spawned the child with CreateProcess,
   captured stdout/stderr through anonymous pipes on a worker thread, and streamed
   the output live while honouring timeout and cancel. None of that exists on
   single-tasking DOS: there are no pipes, no threads, and the agent cannot do
   anything while a child runs. So execution here is synchronous - run the command
   with stdout redirected to a temp file, then read the file back and stream it as
   EXEC_STDOUT frames followed by EXEC_COMPLETE.

   The EXEC_REQUEST parse and the 28-byte EXEC_COMPLETE layout are identical to
   the Win9x agent, so v9xctl.ps1 and the MCP server drive it unchanged. What the
   host loses against a DOS box: no live streaming (output arrives after the
   command finishes), no timeout, no cancel, and no stderr capture - COMMAND.COM
   redirects handle 1 only. The exit code is the DOS ERRORLEVEL system() returns. */

#include <ctype.h>
#include <direct.h>
#include <dos.h>
#include <errno.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dosagent.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"

#define V9X_EXEC_MAX_APPLICATION 259ul
#define V9X_EXEC_MAX_COMMAND 2047ul
#define V9X_EXEC_MAX_DIRECTORY 259ul
#define V9X_EXEC_MAX_TIMEOUT 3600000ul
#define V9X_EXEC_MAX_OUTPUT 1048576ul
#define V9X_EXEC_CHUNK 1024ul

typedef struct V9xDosJob {
    unsigned char mode;
    unsigned char show_window;
    unsigned short options;
    unsigned long timeout_ms;
    unsigned long stdout_limit;
    unsigned long stderr_limit;
    char application[260];
    char command[2048];
    char directory[260];
} V9xDosJob;

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

/* Build the string handed to system() (which wraps it in COMMAND.COM /C), then
   append stdout redirection to the capture file. Mirrors v9x_build_command_line
   in src\guest\execute.c; the redirect is the DOS-specific addition. */
static int v9x_build_command_line(const V9xDosJob *job, char *target,
                                  unsigned long capacity)
{
    unsigned long offset = 0ul;
    target[0] = '\0';
    if (job->mode == V9X_EXEC_MODE_SHELL) {
        if (!v9x_append_text(target, capacity, &offset, job->command)) return 0;
    } else {
        if (v9x_contains_quote(job->application)) return 0;
        if (!v9x_append_text(target, capacity, &offset, "\"") ||
            !v9x_append_text(target, capacity, &offset, job->application) ||
            !v9x_append_text(target, capacity, &offset, "\"")) return 0;
        if (job->command[0] != '\0') {
            if (!v9x_append_text(target, capacity, &offset, " ") ||
                !v9x_append_text(target, capacity, &offset, job->command)) {
                return 0;
            }
        }
    }
    return v9x_append_text(target, capacity, &offset, " > ") &&
           v9x_append_text(target, capacity, &offset, V9X_DOS_EXEC_OUT);
}

/* Change to the job's working directory (drive-aware), remembering the previous
   drive+directory in `saved`. Returns 1 on success. A no-op for an empty dir. */
static int v9x_pushd(const char *dir, char *saved, unsigned long saved_size)
{
    saved[0] = '\0';
    if (dir[0] == '\0') return 1;
    if (getcwd(saved, (int)saved_size) == 0) return 0;
    if (dir[1] == ':') {
        unsigned total;
        unsigned drive = (unsigned)(toupper((unsigned char)dir[0]) - 'A') + 1u;
        _dos_setdrive(drive, &total);
    }
    return chdir(dir) == 0;
}

static void v9x_popd(const char *saved)
{
    if (saved[0] == '\0') return;
    if (saved[1] == ':') {
        unsigned total;
        unsigned drive = (unsigned)(toupper((unsigned char)saved[0]) - 'A') + 1u;
        _dos_setdrive(drive, &total);
    }
    (void)chdir(saved);
}

/* Stream the capture file to the host as EXEC_STDOUT frames, up to `limit`
   bytes. *total receives the full byte count produced (even past the limit) and
   *flags gains STDOUT_TRUNCATED if output was clipped. Returns 1, or 0 if a
   frame send failed (the caller then stops serving). */
static int v9x_stream_output(V9xDos *dos, unsigned long request_id,
                             unsigned long limit, unsigned long *total,
                             unsigned long *flags)
{
    int fd;
    int read_count;
    unsigned long sent = 0ul;
    unsigned char buffer[V9X_EXEC_CHUNK];
    *total = 0ul;
    fd = open(V9X_DOS_EXEC_OUT, O_RDONLY | O_BINARY);
    if (fd < 0) return 1; /* no output file: nothing to stream */
    for (;;) {
        read_count = read(fd, buffer, (unsigned)sizeof(buffer));
        if (read_count <= 0) break;
        *total += (unsigned long)read_count;
        if (sent < limit) {
            unsigned long send_count = (unsigned long)read_count;
            if (send_count > limit - sent) send_count = limit - sent;
            if (send_count != 0ul) {
                if (!v9x_send_frame(dos, V9X_MSG_EXEC_STDOUT, request_id,
                                    buffer, send_count)) {
                    close(fd);
                    return 0;
                }
                sent += send_count;
            }
        }
    }
    close(fd);
    if (*total > limit) *flags |= V9X_EXEC_FLAG_STDOUT_TRUNCATED;
    return 1;
}

int v9x_dos_execute(V9xDos *dos, unsigned long request_id,
                    const unsigned char *payload, unsigned long length)
{
    V9xDosJob job;
    unsigned long offset = 16ul;
    unsigned long result = V9X_EXEC_RESULT_OK;
    unsigned long exit_code = 0xfffffffful;
    unsigned long native_error = 0ul;
    unsigned long flags = V9X_EXEC_FLAG_PIPE_CAPTURE;
    unsigned long stdout_total = 0ul;
    unsigned long started;
    unsigned long elapsed;
    char command_line[2400];
    char saved_dir[260];
    int status;
    unsigned char complete[28];

    if (length < 22ul) {
        return v9x_send_error(dos, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              "invalid execution request");
    }
    job.mode = payload[0];
    job.show_window = payload[1];
    job.options = v9x_read_u16(payload + 2);
    job.timeout_ms = v9x_read_u32(payload + 4);
    job.stdout_limit = v9x_read_u32(payload + 8);
    job.stderr_limit = v9x_read_u32(payload + 12);
    if ((job.options & ~V9X_EXEC_OPTION_MASK) != 0u ||
        job.mode > V9X_EXEC_MODE_SHELL || job.show_window > 1u ||
        job.timeout_ms > V9X_EXEC_MAX_TIMEOUT ||
        job.stdout_limit > V9X_EXEC_MAX_OUTPUT ||
        job.stderr_limit > V9X_EXEC_MAX_OUTPUT ||
        !v9x_read_string(payload, length, &offset, job.application,
                         V9X_EXEC_MAX_APPLICATION) ||
        !v9x_read_string(payload, length, &offset, job.command,
                         V9X_EXEC_MAX_COMMAND) ||
        !v9x_read_string(payload, length, &offset, job.directory,
                         V9X_EXEC_MAX_DIRECTORY) || offset != length ||
        (job.mode == V9X_EXEC_MODE_DIRECT && job.application[0] == '\0') ||
        (job.mode == V9X_EXEC_MODE_SHELL && job.command[0] == '\0')) {
        return v9x_send_error(dos, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              "invalid execution request");
    }

    /* Acknowledge, then run. The tick echoed here matches the Win9x agent. */
    started = v9x_dos_now_ms();
    v9x_write_u32(dos->response, started);
    if (!v9x_send_frame(dos, V9X_MSG_EXEC_ACCEPTED, request_id,
                        dos->response, 4ul)) return 0;

    (void)remove(V9X_DOS_EXEC_OUT);
    if (!v9x_build_command_line(&job, command_line, sizeof(command_line))) {
        result = V9X_EXEC_RESULT_CREATE_FAILED;
        native_error = (unsigned long)EINVAL;
    } else if (!v9x_pushd(job.directory, saved_dir, sizeof(saved_dir))) {
        result = V9X_EXEC_RESULT_CREATE_FAILED;
        native_error = (unsigned long)errno;
    } else {
        status = system(command_line);
        v9x_popd(saved_dir);
        if (status == -1) {
            result = V9X_EXEC_RESULT_CREATE_FAILED;
            native_error = (unsigned long)errno;
        } else {
            result = V9X_EXEC_RESULT_OK;
            exit_code = (unsigned long)(status & 0xff);
            if (!v9x_stream_output(dos, request_id, job.stdout_limit,
                                   &stdout_total, &flags)) {
                (void)remove(V9X_DOS_EXEC_OUT);
                return 0;
            }
        }
    }
    (void)remove(V9X_DOS_EXEC_OUT);

    elapsed = v9x_dos_now_ms() - started;
    v9x_write_u32(complete, result);
    v9x_write_u32(complete + 4, exit_code);
    v9x_write_u32(complete + 8, native_error);
    v9x_write_u32(complete + 12, elapsed);
    v9x_write_u32(complete + 16, stdout_total);
    v9x_write_u32(complete + 20, 0ul); /* stderr not captured on DOS */
    v9x_write_u32(complete + 24, flags);
    return v9x_send_frame(dos, V9X_MSG_EXEC_COMPLETE, request_id,
                          complete, sizeof(complete));
}
