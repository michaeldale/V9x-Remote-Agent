/* Exercise the guest handler with deterministic USER32/clock substitutes.
   No real desktop, logon settings or foreground input are touched. */
#include "../../src/guest/agent.h"
#include "../../src/guest/platform_win.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static DWORD ticks;
static int ready, visible, nt, posts, events, dismisses, starts_shell, can_post;
static const char *dialog_title;
static const char *dialog_class;
static unsigned short response_type;
static DWORD response_length;
static V9xAgentState machine;
static V9xConnection connection;

static BOOL fake_visible(HWND w) { (void)w; return visible; }
static BOOL fake_enabled(HWND w) { (void)w; return TRUE; }
static int fake_class(HWND w, LPSTR s, int n) { (void)w; (void)n; strcpy(s, dialog_class); return strlen(s); }
static int fake_title(HWND w, LPSTR s, int n) { (void)w; (void)n; strcpy(s, dialog_title); return strlen(s); }
static HWND fake_item(HWND w, int id) { (void)w; assert(id == IDCANCEL); return (HWND)2; }
static BOOL fake_enum(WNDENUMPROC callback, LPARAM arg) { return callback((HWND)1, arg); }
static BOOL fake_post(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    assert(w == (HWND)1 && msg == WM_COMMAND && LOWORD(wp) == IDCANCEL);
    assert(lp == (LPARAM)2);
    ++posts;
    return can_post;
}
static DWORD fake_ticks(void) { return ticks; }
static void fake_sleep(DWORD ms)
{
    ticks += ms;
    if (posts && can_post && dismisses) { visible = 0; if (starts_shell) ready = 1; }
}
int v9x_platform_is_nt(void) { return nt; }
int v9x_desktop_ready(void) { return ready; }
void v9x_screen_info(DWORD *w, DWORD *h, DWORD *bpp) { *w = 1024; *h = 768; *bpp = 0; }
void v9x_event(const char *text, const char *detail) { (void)text; (void)detail; ++events; }
int v9x_send_frame(V9xConnection *c, unsigned short type, unsigned long id,
                    const unsigned char *payload, unsigned long length)
{
    assert(c == &connection && id == 42 && (payload == c->response || payload == c->file_response || payload == c->input_response));
    response_type = type; response_length = length; return 1;
}

#define IsWindowVisible fake_visible
#define IsWindowEnabled fake_enabled
#define IsWindow fake_visible
#define GetClassNameA fake_class
#define GetWindowTextA fake_title
#define GetDlgItem fake_item
#define EnumWindows fake_enum
#define PostMessageA fake_post
#define GetTickCount fake_ticks
#define Sleep fake_sleep
#define lstrcmpA strcmp
#include "../../src/guest/desktop.c"

/* One in-memory registry/display lifecycle check; no host settings change. */
static unsigned char reg_data[1024];
static DWORD reg_type, reg_size;
static int reg_exists, reg_calls, restored;
static LONG reg_open(HKEY root, LPCSTR path, DWORD flags, REGSAM access, PHKEY key)
{ (void)root; (void)path; (void)flags; (void)access; ++reg_calls; *key = (HKEY)3; return 0; }
static LONG reg_create(HKEY root, LPCSTR path, DWORD reserved, LPSTR cls, DWORD options,
                       REGSAM access, LPSECURITY_ATTRIBUTES sec, PHKEY key, LPDWORD disposition)
{ (void)reserved; (void)cls; (void)options; (void)sec; *disposition = 1; return reg_open(root, path, 0, access, key); }
static LONG reg_query(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size)
{
    (void)key; (void)name; (void)reserved;
    if (!reg_exists) return ERROR_FILE_NOT_FOUND;
    if (*size < reg_size) return ERROR_MORE_DATA;
    *type = reg_type; *size = reg_size; memcpy(data, reg_data, reg_size); return 0;
}
static LONG reg_set(HKEY key, LPCSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD size)
{ (void)key; (void)name; (void)reserved; reg_type = type; reg_size = size; memcpy(reg_data, data, size); reg_exists = 1; return 0; }
static LONG reg_delete(HKEY key, LPCSTR name) { (void)key; (void)name; reg_exists = 0; return 0; }
static LONG reg_close(HKEY key) { (void)key; return 0; }
static BOOL enum_mode(LPCSTR name, DWORD which, DEVMODEA *mode)
{
    (void)name;
    mode->dmPelsWidth = which == (DWORD)-2 || restored ? 1024 : 640;
    mode->dmPelsHeight = which == (DWORD)-2 || restored ? 768 : 480;
    mode->dmBitsPerPel = 32; mode->dmDisplayFrequency = 60; return TRUE;
}
static LONG change_mode(DEVMODEA *mode, DWORD flags)
{ assert(mode == 0 && flags == 0); restored = 1; return DISP_CHANGE_SUCCESSFUL; }
#define RegOpenKeyExA reg_open
#define RegCreateKeyExA reg_create
#define RegQueryValueExA reg_query
#define RegSetValueExA reg_set
#define RegDeleteValueA reg_delete
#define RegFlushKey reg_close
#define RegCloseKey reg_close
#define EnumDisplaySettingsA enum_mode
#define ChangeDisplaySettingsA change_mode
#include "../../src/guest/operator.c"

static HWND focused;
static int key_events, reject_focus, focus_lock, attached, attaches;
static HWND get_focus(void) { return focused; }
/* focus_lock models the Win98+ foreground lock: only a thread attached to
   the foreground input queue may switch. reject_focus refuses outright. */
static BOOL set_focus(HWND w)
{
    if (reject_focus || (focus_lock && !attached)) return FALSE;
    focused = w; return TRUE;
}
static BOOL not_iconic(HWND w) { (void)w; return FALSE; }
static DWORD window_pid(HWND w, LPDWORD pid) { (void)w; if (pid) *pid = 123; return 77; }
static DWORD thread_id(void) { return 5; }
static BOOL attach_input(DWORD a, DWORD b, BOOL on)
{ assert(a == 5 && b == 77); attached = on; if (on) ++attaches; return TRUE; }
static BOOL to_top(HWND w) { (void)w; return TRUE; }
static BOOL cursor_pos(LPPOINT p) { p->x = 10; p->y = 20; return TRUE; }
static void key_event(BYTE vk, BYTE scan, DWORD flags, DWORD extra)
{
    (void)scan; (void)extra; ++key_events;
    if (vk == VK_RETURN && !(flags & KEYEVENTF_KEYUP)) { focused = 0; visible = 0; }
}
static void mouse_input(DWORD a, DWORD b, DWORD c, DWORD d, DWORD e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; }
void v9x_log_line(const char *text) { (void)text; }
#define GetForegroundWindow get_focus
#define SetForegroundWindow set_focus
#define IsIconic not_iconic
#define GetWindowThreadProcessId window_pid
#define GetCurrentThreadId thread_id
#define AttachThreadInput attach_input
#define BringWindowToTop to_top
#define GetCursorPos cursor_pos
#define keybd_event key_event
#define mouse_event mouse_input
#include "../../src/guest/input.c"

static void check_input(void)
{
    unsigned char p[64];
    unsigned long at = 0ul;
    dialog_title = "Game"; visible = 1; focused = (HWND)1;
    assert(v9x_append_string(p, sizeof(p), &at, "Game"));
    v9x_write_u16(p + at, 2); at += 2;
    p[at++] = V9X_INPUT_OP_KEY; p[at++] = VK_RETURN; p[at++] = 1;
    p[at++] = V9X_INPUT_OP_KEY; p[at++] = VK_RETURN; p[at++] = 0;
    /* The final Enter closes its target on key-down: still release the key
       and report the target's captured name instead of a false failure. */
    assert(v9x_handle_target_input(&connection, 42, p, at));
    assert(response_type == V9X_MSG_INPUT_TARGET_RESPONSE && key_events == 2);
    assert(v9x_read_u32(connection.input_response) == 2);
    assert(v9x_read_u32(connection.input_response + response_length - 10) == 0);
    /* Foreground lock: the plain call fails, the attached retry succeeds
       and the queues are detached again afterwards. */
    visible = 1; focused = (HWND)9; focus_lock = 1;
    assert(v9x_handle_target_input(&connection, 42, p, at));
    assert(response_type == V9X_MSG_INPUT_TARGET_RESPONSE && key_events == 4);
    assert(attaches == 1 && !attached);
    focus_lock = 0;
    visible = 1; focused = 0; reject_focus = 1;
    assert(v9x_handle_target_input(&connection, 42, p, at));
    assert(response_type == V9X_MSG_ERROR_RESPONSE && key_events == 4);
}

static void check_operator(void)
{
    unsigned char p[128];
    unsigned long at, size_at;
    ready = 1;
    reg_exists = 1; reg_type = REG_DWORD; reg_size = 4; v9x_write_u32(reg_data, 2);
    v9x_write_u32(p, 0x80000001ul); v9x_write_u32(p + 4, V9X_REG_SET_DWORD); at = 8;
    assert(v9x_append_string(p, sizeof(p), &at, "Software\\Test"));
    assert(v9x_append_string(p, sizeof(p), &at, "EngineType"));
    size_at = at; v9x_write_u32(p + at, 4); at += 4; v9x_write_u32(p + at, 3); at += 4;
    assert(v9x_handle_operator(&connection, V9X_MSG_REG_WRITE_REQUEST, 42, p, at));
    assert(response_type == V9X_MSG_REG_WRITE_RESPONSE && v9x_read_u32(reg_data) == 3);
    assert(v9x_read_u32(connection.file_response + 12) == 2);
    v9x_write_u32(p + 4, V9X_REG_DELETE_VALUE); v9x_write_u32(p + size_at, 0);
    assert(v9x_handle_operator(&connection, V9X_MSG_REG_WRITE_REQUEST, 42, p, size_at + 4));
    assert(!reg_exists && v9x_read_u32(connection.file_response + 12) == 3);
    /* Protection is checked before opening or creating any registry key. */
    reg_calls = 0; at = 8; v9x_write_u32(p, 0x80000002ul);
    assert(v9x_append_string(p, sizeof(p), &at, "SYSTEM\\CurrentControlSet"));
    assert(v9x_append_string(p, sizeof(p), &at, "Value"));
    v9x_write_u32(p + at, 0); at += 4;
    assert(v9x_handle_operator(&connection, V9X_MSG_REG_WRITE_REQUEST, 42, p, at));
    assert(response_type == V9X_MSG_ERROR_RESPONSE && reg_calls == 0);
    v9x_write_u32(p, 0);
    assert(v9x_handle_operator(&connection, V9X_MSG_DISPLAY_REQUEST, 42, p, 4));
    assert(v9x_read_u32(connection.response + 8) == 640);
    v9x_write_u32(p, 1);
    assert(v9x_handle_operator(&connection, V9X_MSG_DISPLAY_REQUEST, 42, p, 4));
    assert(restored && v9x_read_u32(connection.response + 8) == 1024);
}

static void reset(void)
{
    ticks = 0; ready = nt = posts = events = 0;
    visible = dismisses = starts_shell = can_post = 1;
    dialog_title = "Enter Network Password"; dialog_class = "#32770";
    machine.boot_counter = 7; connection.machine = &machine;
}
static DWORD run(DWORD flags, DWORD timeout)
{
    unsigned char request[8];
    v9x_write_u32(request, flags); v9x_write_u32(request + 4, timeout);
    assert(v9x_handle_desktop_wait(&connection, 42, request, 8));
    assert(response_type == V9X_MSG_DESKTOP_WAIT_RESPONSE && response_length == 24);
    assert(v9x_read_u32(connection.response + 4) == 7);
    assert(v9x_read_u32(connection.response + 8) == 1024);
    return v9x_read_u32(connection.response);
}
int main(void)
{
    unsigned char request[8];
    reset(); ready = 1; assert(run(1, 500) == 1 && posts == 0);
    reset(); assert(run(1, 500) == 3 && posts == 1 && events == 1);
    reset(); dialog_title = "Enter Windows Password"; assert(run(1, 500) == 3);
    reset(); assert(run(0, 500) == 0 && posts == 0 && ticks == 500);
    reset(); dialog_title = "Some application"; assert(run(1, 500) == 0 && posts == 0);
    reset(); dialog_class = "OtherClass"; assert(run(1, 500) == 0 && posts == 0);
    reset(); dismisses = 0; assert(run(1, 500) == 0 && posts == 1);
    reset(); starts_shell = 0; assert(run(1, 500) == 2 && posts == 1);
    reset(); can_post = 0; assert(run(1, 0) == 0 && events == 0);
    reset(); nt = 1;
    assert(v9x_handle_desktop_wait(&connection, 42, request, 0));
    assert(response_type == V9X_MSG_ERROR_RESPONSE);
    assert(v9x_read_u32(connection.response) == V9X_STATUS_UNSUPPORTED_OPERATION);
    reset(); v9x_write_u32(request, 2); v9x_write_u32(request + 4, 0);
    assert(v9x_handle_desktop_wait(&connection, 42, request, 8));
    assert(v9x_read_u32(connection.response) == V9X_STATUS_INVALID_PAYLOAD);
    v9x_write_u32(request, 0); v9x_write_u32(request + 4, 60001ul);
    assert(v9x_handle_desktop_wait(&connection, 42, request, 8));
    assert(v9x_read_u32(connection.response) == V9X_STATUS_INVALID_PAYLOAD);
    assert(v9x_handle_desktop_wait(&connection, 42, request, 7));
    assert(v9x_read_u32(connection.response) == V9X_STATUS_INVALID_PAYLOAD);
    check_operator();
    check_input();
    puts("PASS: guest operator lifecycle (logon, registry, display, targeted input)");
    return 0;
}
