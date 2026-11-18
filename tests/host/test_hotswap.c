/* Deterministic hot-swap failure injection. No host files/processes or
   registry settings are changed. Exercise the actual guest swapper. */
#include "../../src/guest/agent.h"
#include "../../src/guest/platform_win.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int image, backup, copy_fails, spawn_fails, event_fails, stop_fails;
static int launches, terminated, helper_copies, healthy, closed;
static DWORD wait_result;
static int nt;
static char log_text[4096];
static unsigned int log_size;
static DWORD attrs(LPCSTR p)
{
    if (strstr(p, "V9XAGNT.OLD")) return backup ? 0 : 0xfffffffful;
    if (strstr(p, "V9XAGNT.EXE")) return image ? 0 : 0xfffffffful;
    return 0;
}
static BOOL remove_file(LPCSTR p)
{
    if (strstr(p, "V9XAGNT.OLD")) { if (backup) assert(healthy); backup = 0; }
    if (strstr(p, "V9XAGNT.EXE")) image = 0;
    return TRUE;
}
static BOOL move_file(LPCSTR from, LPCSTR to)
{
    if (strstr(from, "V9XAGNT.OLD")) {
        assert(backup && !image && strstr(to, "V9XAGNT.EXE"));
        image = 1; backup = 0;
    } else {
        assert(image == 1 && !backup && strstr(to, "V9XAGNT.OLD"));
        backup = 1; image = 0;
    }
    return TRUE;
}
static BOOL copy_file(LPCSTR from, LPCSTR to, BOOL fail_exists)
{
    (void)to; (void)fail_exists;
    if (strstr(from, "V9XSNEW")) { assert(healthy); ++helper_copies; return TRUE; }
    image = 2; /* failed copies may leave a partial destination */
    return !copy_fails;
}
static HANDLE create_file(LPCSTR p, DWORD a, DWORD b, LPSECURITY_ATTRIBUTES c,
                           DWORD d, DWORD e, HANDLE f)
{ (void)p; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return (HANDLE)1; }
static BOOL write_file(HANDLE h, LPCVOID data, DWORD size, LPDWORD written, LPOVERLAPPED ov)
{
    (void)h; (void)ov; assert(log_size + size < sizeof(log_text));
    memcpy(log_text + log_size, data, size); log_size += size; log_text[log_size] = 0;
    *written = size; return TRUE;
}
static BOOL close_handle(HANDLE h) { (void)h; ++closed; return TRUE; }
static DWORD seek_file(HANDLE h, LONG n, PLONG high, DWORD method)
{ (void)h; (void)n; (void)high; (void)method; return 0; }
static BOOL spawn(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES a, LPSECURITY_ATTRIBUTES b,
                   BOOL inherit, DWORD flags, LPVOID env, LPCSTR dir,
                   LPSTARTUPINFOA startup, LPPROCESS_INFORMATION process)
{
    (void)app; (void)cmd; (void)a; (void)b; (void)inherit; (void)flags;
    (void)env; (void)dir; (void)startup;
    ++launches;
    if (image == 2 && spawn_fails) return FALSE;
    process->hThread = (HANDLE)2; process->hProcess = (HANDLE)3;
    return TRUE;
}
static HANDLE create_event(LPSECURITY_ATTRIBUTES a, BOOL manual, BOOL initial, LPCSTR name)
{ (void)a; (void)name; assert(manual && !initial); return event_fails ? 0 : (HANDLE)4; }
static BOOL reset_event(HANDLE h) { assert(h == (HANDLE)4); return TRUE; }
static DWORD wait_many(DWORD count, const HANDLE *handles, BOOL all, DWORD ms)
{
    assert(count == 2 && handles[0] == (HANDLE)3 && handles[1] == (HANDLE)4 && !all && ms == 60000ul);
    healthy = wait_result == WAIT_OBJECT_0 + 1;
    assert(backup); return wait_result;
}
static DWORD wait_one(HANDLE h, DWORD ms)
{ (void)ms; assert(h == (HANDLE)3); return terminated || wait_result == WAIT_OBJECT_0 ? WAIT_OBJECT_0 : WAIT_TIMEOUT; }
static BOOL stop_process(HANDLE h, UINT code)
{ (void)code; assert(h == (HANDLE)3); if (stop_fails) return FALSE; terminated = 1; return TRUE; }
static HANDLE open_mutex(DWORD access, BOOL inherit, LPCSTR name)
{ (void)access; (void)inherit; (void)name; return 0; }
static BOOL exit_code(HANDLE h, LPDWORD code)
{ assert(h == (HANDLE)3); *code = 123ul; return TRUE; }
static DWORD last_error(void) { return ERROR_ACCESS_DENIED; }
static LONG reg_open(HKEY h, LPCSTR p, DWORD r, LPSTR cls, DWORD opt, REGSAM access,
                       LPSECURITY_ATTRIBUTES sa, PHKEY key, LPDWORD disp)
{ (void)h; (void)p; (void)r; (void)cls; (void)opt; (void)access; (void)sa; (void)key; (void)disp; return ERROR_ACCESS_DENIED; }
int v9x_platform_is_nt(void) { return nt; }
const char *v9x_platform_run_key(void) { return "unused"; }
int v9x_execution_any_active(V9xAgentState *s) { (void)s; return 0; }
void v9x_mdns_goodbye(void) {}
void v9x_log_event(const char *a, const char *b) { (void)a; (void)b; }
void v9x_event(const char *a, const char *b) { (void)a; (void)b; }
int v9x_send_frame(V9xConnection *c, unsigned short t, unsigned long id,
                    const unsigned char *p, unsigned long n)
{ (void)c; (void)t; (void)id; (void)p; (void)n; return 1; }

#define GetFileAttributesA attrs
#define DeleteFileA remove_file
#define MoveFileA move_file
#define CopyFileA copy_file
#define CreateFileA create_file
#define WriteFile write_file
#define SetFilePointer seek_file
#define CloseHandle close_handle
#define CreateProcessA spawn
#define CreateEventA create_event
#define ResetEvent reset_event
#define WaitForMultipleObjects wait_many
#define WaitForSingleObject wait_one
#define TerminateProcess stop_process
#define OpenMutexA open_mutex
#define GetExitCodeProcess exit_code
#define GetLastError last_error
#define RegCreateKeyExA reg_open
#include "../../src/guest/update.c"

static void reset(void)
{
    image = 1; backup = copy_fails = spawn_fails = event_fails = stop_fails = 0;
    launches = terminated = helper_copies = healthy = closed = 0;
    log_size = 0; log_text[0] = 0; wait_result = WAIT_OBJECT_0 + 1;
}
int main(void)
{
    for (nt = 0; nt <= 1; ++nt) {
        reset(); assert(v9x_hotswap_run() == 0);
        assert(image == 2 && !backup && launches == 1 && helper_copies == 1 && closed);
        assert(strstr(log_text, "agent-ready\r\n"));
        reset(); copy_fails = 1; assert(v9x_hotswap_run() == 3);
        assert(image == 1 && !backup && launches == 1 && !helper_copies);
        reset(); spawn_fails = 1; assert(v9x_hotswap_run() == 3);
        assert(image == 1 && launches == 2 && strstr(log_text, "rollback-relaunched"));
        assert(strstr(log_text, "agent-relaunch-failed 0x00000005"));
        reset(); event_fails = 1; assert(v9x_hotswap_run() == 3);
        assert(image == 1 && launches == 1);
        reset(); wait_result = WAIT_OBJECT_0; assert(v9x_hotswap_run() == 3);
        assert(image == 1 && launches == 2 && !terminated && !helper_copies);
        assert(strstr(log_text, "agent-exit-code 0x0000007B"));
        reset(); wait_result = WAIT_TIMEOUT; assert(v9x_hotswap_run() == 3);
        assert(image == 1 && launches == 2 && terminated && !helper_copies);
        reset(); wait_result = WAIT_TIMEOUT; stop_fails = 1; assert(v9x_hotswap_run() == 4);
        assert(image == 2 && backup && launches == 1 && !helper_copies);
    }
    puts("PASS: hot-swap readiness, copy/launch/early-exit/timeout rollback, retained backup on stop failure");
    return 0;
}
