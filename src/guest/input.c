#include "agent.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"

static volatile LONG v9x_input_busy = 0l;

typedef struct V9xInputTarget {
    const char *title;
    HWND window;
    int count;
} V9xInputTarget;

static int title_contains(const char *text, const char *part)
{
    const char *a, *b;
    unsigned char x, y;
    for (; *text; ++text) {
        a = text; b = part;
        while (*a && *b) {
            x = (unsigned char)*a; y = (unsigned char)*b;
            if (x >= 'a' && x <= 'z') x -= 32;
            if (y >= 'a' && y <= 'z') y -= 32;
            if (x != y) break;
            ++a; ++b;
        }
        if (!*b) return 1;
    }
    return 0;
}

static BOOL CALLBACK find_target(HWND w, LPARAM param)
{
    V9xInputTarget *target = (V9xInputTarget *)param;
    char title[256];
    if (IsWindowVisible(w) && GetWindowTextA(w, title, sizeof(title)) &&
        title_contains(title, target->title)) {
        target->window = w; ++target->count;
    }
    return TRUE;
}

/* The agent is a background process, so a plain SetForegroundWindow is
   usually refused by the foreground lock (Win98/2000+). Joining the current
   foreground thread's input queue for the call is the standard way past it.
   A restored minimized window and the switch itself land asynchronously,
   so poll briefly (up to 500 ms) before declaring failure. */
static int bring_to_front(HWND window)
{
    HWND current;
    DWORD mine, theirs, waited;
    BOOL attached = FALSE;
    if (IsIconic(window)) (void)ShowWindowAsync(window, SW_RESTORE);
    (void)SetForegroundWindow(window);
    for (waited = 0ul; GetForegroundWindow() != window; waited += 50ul) {
        if (waited >= 500ul) return 0;
        if (waited == 0ul) {
            current = GetForegroundWindow();
            mine = GetCurrentThreadId();
            theirs = current != 0 ? GetWindowThreadProcessId(current, 0) : 0ul;
            if (theirs != 0ul && theirs != mine)
                attached = AttachThreadInput(mine, theirs, TRUE);
            (void)BringWindowToTop(window);
            (void)SetForegroundWindow(window);
            if (attached) (void)AttachThreadInput(mine, theirs, FALSE);
            continue;
        }
        Sleep(50ul);
    }
    return 1;
}

static int foreground(unsigned char *out, unsigned long cap,
                        unsigned long *offset)
{
    HWND w = GetForegroundWindow();
    DWORD pid = 0ul;
    char title[256];
    unsigned long i;
    title[0] = 0;
    if (w != 0) { (void)GetWindowThreadProcessId(w, &pid); (void)GetWindowTextA(w, title, sizeof(title)); }
    for (i = 0ul; title[i]; ++i) if ((unsigned char)title[i] > 127u) title[i] = '?';
    if (*offset + 8ul > cap) return 0;
    v9x_write_u32(out + *offset, (DWORD)w);
    v9x_write_u32(out + *offset + 4, pid);
    *offset += 8ul;
    return v9x_append_string(out, cap, offset, title);
}

static int v9x_input_error(V9xConnection *conn, unsigned long request_id,
                           unsigned long status, unsigned long native_error,
                           const char *detail)
{
    unsigned long offset = 8ul;
    v9x_write_u32(conn->input_response, status);
    v9x_write_u32(conn->input_response + 4, native_error);
    if (!v9x_append_string(conn->input_response, sizeof(conn->input_response),
                           &offset, detail)) return 0;
    return v9x_send_frame(conn, V9X_MSG_ERROR_RESPONSE, request_id,
                          conn->input_response, offset);
}

static long v9x_read_s32(const unsigned char *source)
{
    return (long)v9x_read_u32(source);
}

static int v9x_read_s16(const unsigned char *source)
{
    unsigned short raw = v9x_read_u16(source);
    return raw >= 0x8000u ? (int)raw - 0x10000 : (int)raw;
}

/* Walk the action list once without acting, confirming every action is fully
   present and well formed. Returns 0 on any malformation. */
static int v9x_input_validate(const unsigned char *payload, unsigned long length,
                              unsigned long count)
{
    unsigned long offset = 2ul;
    unsigned long index;
    unsigned long text_length;
    unsigned char opcode;
    for (index = 0ul; index < count; ++index) {
        if (offset + 1ul > length) return 0;
        opcode = payload[offset++];
        switch (opcode) {
        case V9X_INPUT_OP_MOUSE_MOVE:
        case V9X_INPUT_OP_MOUSE_MOVE_RELATIVE:
            if (offset + 8ul > length) return 0;
            offset += 8ul;
            break;
        case V9X_INPUT_OP_MOUSE_BUTTON:
            if (offset + 2ul > length) return 0;
            if (payload[offset] > V9X_INPUT_BUTTON_MIDDLE ||
                payload[offset + 1ul] > 1u) return 0;
            offset += 2ul;
            break;
        case V9X_INPUT_OP_MOUSE_WHEEL:
            if (offset + 2ul > length) return 0;
            offset += 2ul;
            break;
        case V9X_INPUT_OP_KEY:
            if (offset + 2ul > length) return 0;
            if (payload[offset] == 0u || payload[offset + 1ul] > 1u) return 0;
            offset += 2ul;
            break;
        case V9X_INPUT_OP_TYPE:
            if (offset + 2ul > length) return 0;
            text_length = (unsigned long)v9x_read_u16(payload + offset);
            offset += 2ul;
            if (offset + text_length > length) return 0;
            offset += text_length;
            break;
        case V9X_INPUT_OP_DELAY:
            if (offset + 2ul > length) return 0;
            if ((unsigned long)v9x_read_u16(payload + offset) >
                    V9X_INPUT_MAX_DELAY_MS) return 0;
            offset += 2ul;
            break;
        default:
            return 0;
        }
    }
    return offset == length;
}

static void v9x_input_mouse_button(unsigned char button, unsigned char down)
{
    DWORD flags = 0ul;
    if (button == V9X_INPUT_BUTTON_LEFT) {
        flags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    } else if (button == V9X_INPUT_BUTTON_RIGHT) {
        flags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    } else {
        flags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    }
    mouse_event(flags, 0ul, 0ul, 0ul, 0ul);
}

static long v9x_input_step(long remaining)
{
    if (remaining > V9X_INPUT_RELATIVE_STEP) return V9X_INPUT_RELATIVE_STEP;
    if (remaining < -V9X_INPUT_RELATIVE_STEP) return -V9X_INPUT_RELATIVE_STEP;
    return remaining;
}

static void v9x_input_mouse_relative(long dx, long dy)
{
    while (dx != 0l || dy != 0l) {
        long step_x = v9x_input_step(dx);
        long step_y = v9x_input_step(dy);
        mouse_event(MOUSEEVENTF_MOVE, (DWORD)step_x, (DWORD)step_y, 0ul, 0ul);
        dx -= step_x;
        dy -= step_y;
    }
}

static void v9x_input_key(unsigned char vk, unsigned char down)
{
    UINT scan = MapVirtualKeyA((UINT)vk, 0u);
    keybd_event(vk, (BYTE)scan, down ? 0ul : KEYEVENTF_KEYUP, 0ul);
}

static void v9x_input_type_char(unsigned char character)
{
    SHORT scan_result;
    unsigned char vk;
    unsigned char modifiers;
    if (character == (unsigned char)'\n' || character == (unsigned char)'\r') {
        v9x_input_key((unsigned char)VK_RETURN, 1u);
        v9x_input_key((unsigned char)VK_RETURN, 0u);
        return;
    }
    if (character == (unsigned char)'\t') {
        v9x_input_key((unsigned char)VK_TAB, 1u);
        v9x_input_key((unsigned char)VK_TAB, 0u);
        return;
    }
    scan_result = VkKeyScanA((char)character);
    if (scan_result == -1) return;
    vk = (unsigned char)(scan_result & 0xffu);
    modifiers = (unsigned char)((scan_result >> 8) & 0xffu);
    if (modifiers & 1u) v9x_input_key((unsigned char)VK_SHIFT, 1u);
    if (modifiers & 2u) v9x_input_key((unsigned char)VK_CONTROL, 1u);
    if (modifiers & 4u) v9x_input_key((unsigned char)VK_MENU, 1u);
    v9x_input_key(vk, 1u);
    v9x_input_key(vk, 0u);
    if (modifiers & 4u) v9x_input_key((unsigned char)VK_MENU, 0u);
    if (modifiers & 2u) v9x_input_key((unsigned char)VK_CONTROL, 0u);
    if (modifiers & 1u) v9x_input_key((unsigned char)VK_SHIFT, 0u);
}

static int v9x_input_apply(const unsigned char *payload, unsigned long count, HWND target,
                           unsigned char *keyboard, unsigned long *keyboard_length)
{
    unsigned long offset = 2ul;
    unsigned long index;
    unsigned long text_length;
    unsigned long character;
    unsigned char opcode;
    unsigned char keys[256];
    unsigned char buttons[3];
    for (index = 0ul; index < 256ul; ++index) keys[index] = 0u;
    for (index = 0ul; index < 3ul; ++index) buttons[index] = 0u;
    for (index = 0ul; index < count; ++index) {
        opcode = payload[offset++];
        if (target != 0 && GetForegroundWindow() != target) {
            /* A final Enter/click can close its target on key/button down.
               Finish our matching release instead of reporting a false
               focus failure or leaving the key/button held. */
            if (!((opcode == V9X_INPUT_OP_KEY && payload[offset + 1ul] == 0u && keys[payload[offset]]) ||
                  (opcode == V9X_INPUT_OP_MOUSE_BUTTON && payload[offset + 1ul] == 0u && buttons[payload[offset]])))
                goto focus_lost;
        }
        if (keyboard != 0 && opcode == V9X_INPUT_OP_KEY && payload[offset + 1ul] != 0u) {
            *keyboard_length = 0ul;
            if (!foreground(keyboard, 512ul, keyboard_length)) goto focus_lost;
        }
        switch (opcode) {
        case V9X_INPUT_OP_MOUSE_MOVE:
            (void)SetCursorPos((int)v9x_read_s32(payload + offset),
                               (int)v9x_read_s32(payload + offset + 4ul));
            offset += 8ul;
            break;
        case V9X_INPUT_OP_MOUSE_MOVE_RELATIVE:
            v9x_input_mouse_relative(v9x_read_s32(payload + offset),
                                     v9x_read_s32(payload + offset + 4ul));
            offset += 8ul;
            break;
        case V9X_INPUT_OP_MOUSE_BUTTON:
            v9x_input_mouse_button(payload[offset], payload[offset + 1ul]);
            buttons[payload[offset]] = payload[offset + 1ul];
            offset += 2ul;
            break;
        case V9X_INPUT_OP_MOUSE_WHEEL:
            mouse_event(MOUSEEVENTF_WHEEL, 0ul, 0ul,
                        (DWORD)(v9x_read_s16(payload + offset) * WHEEL_DELTA),
                        0ul);
            offset += 2ul;
            break;
        case V9X_INPUT_OP_KEY:
            v9x_input_key(payload[offset], payload[offset + 1ul]);
            keys[payload[offset]] = payload[offset + 1ul];
            offset += 2ul;
            break;
        case V9X_INPUT_OP_TYPE:
            text_length = (unsigned long)v9x_read_u16(payload + offset);
            offset += 2ul;
            for (character = 0ul; character < text_length; ++character) {
                if (target != 0 && GetForegroundWindow() != target) goto focus_lost;
                if (keyboard != 0) {
                    *keyboard_length = 0ul;
                    if (!foreground(keyboard, 512ul, keyboard_length)) goto focus_lost;
                }
                v9x_input_type_char(payload[offset + character]);
            }
            offset += text_length;
            break;
        case V9X_INPUT_OP_DELAY:
            Sleep((DWORD)v9x_read_u16(payload + offset));
            offset += 2ul;
            break;
        default:
            break;
        }
    }
    return 1;
focus_lost:
    /* Release only the keys/buttons this batch pressed, so an interrupted
       CTRL+... sequence cannot leave the machine with a stuck modifier. */
    for (index = 0ul; index < 256ul; ++index)
        if (keys[index]) v9x_input_key((unsigned char)index, 0u);
    for (index = 0ul; index < 3ul; ++index)
        if (buttons[index]) v9x_input_mouse_button((unsigned char)index, 0u);
    return 0;
}

static int input_run(V9xConnection *conn, unsigned long request_id,
                       const unsigned char *payload, unsigned long length,
                       const char *title, int extended)
{
    unsigned long count;
    unsigned long offset = 12ul;
    unsigned long keyboard_length = 10ul, i;
    unsigned char keyboard[512];
    V9xInputTarget target;
    POINT cursor;
    if (length < 2ul) {
        return v9x_input_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                               ERROR_INVALID_PARAMETER, "input payload too short");
    }
    count = (unsigned long)v9x_read_u16(payload);
    if (count > V9X_INPUT_MAX_ACTIONS) {
        return v9x_input_error(conn, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                               ERROR_INVALID_PARAMETER, "too many input actions");
    }
    if (!v9x_input_validate(payload, length, count)) {
        return v9x_input_error(conn, request_id, V9X_STATUS_INVALID_PAYLOAD,
                               ERROR_INVALID_PARAMETER, "malformed input actions");
    }
    if (InterlockedExchange(&v9x_input_busy, 1l) != 0l)
        return v9x_input_error(conn, request_id, V9X_STATUS_BUSY, 0, "another input batch is active");
    target.window = 0; target.title = title; target.count = 0;
    if (title != 0 && title[0] != 0) {
        (void)EnumWindows(find_target, (LPARAM)&target);
        if (target.count != 1) {
            InterlockedExchange(&v9x_input_busy, 0l);
            return v9x_input_error(conn, request_id, V9X_STATUS_NOT_FOUND, 0,
                target.count == 0 ? "no window matches the input target" : "ambiguous input target; give more of the title");
        }
        if (!bring_to_front(target.window)) {
            InterlockedExchange(&v9x_input_busy, 0l);
            return v9x_input_error(conn, request_id, V9X_STATUS_ACCESS_DENIED, 0, "cannot bring input target to foreground; no input sent");
        }
    }
    if (extended && !foreground(conn->input_response, sizeof(conn->input_response), &offset)) {
        InterlockedExchange(&v9x_input_busy, 0l);
        return 0;
    }
    for (i = 0ul; i < keyboard_length; ++i) keyboard[i] = 0u;
    if (!v9x_input_apply(payload, count, target.window, extended ? keyboard : 0, &keyboard_length)) {
        InterlockedExchange(&v9x_input_busy, 0l);
        return v9x_input_error(conn, request_id, V9X_STATUS_ACCESS_DENIED, 0,
                               "input target lost foreground during batch; remaining input stopped");
    }
    cursor.x = 0l;
    cursor.y = 0l;
    (void)GetCursorPos(&cursor);
    v9x_write_u32(conn->input_response, count);
    v9x_write_u32(conn->input_response + 4, (unsigned long)(long)cursor.x);
    v9x_write_u32(conn->input_response + 8, (unsigned long)(long)cursor.y);
    v9x_log_line("input-complete");
    if (extended) {
        for (i = 0ul; i < keyboard_length; ++i) conn->input_response[offset++] = keyboard[i];
    }
    if (extended && !foreground(conn->input_response, sizeof(conn->input_response), &offset)) {
        InterlockedExchange(&v9x_input_busy, 0l);
        return 0;
    }
    InterlockedExchange(&v9x_input_busy, 0l);
    return v9x_send_frame(conn, extended ? V9X_MSG_INPUT_TARGET_RESPONSE : V9X_MSG_INPUT_RESPONSE,
                          request_id, conn->input_response, extended ? offset : 12ul);
}

int v9x_handle_input(V9xConnection *conn, unsigned long id,
                     const unsigned char *payload, unsigned long length)
{
    return input_run(conn, id, payload, length, 0, 0);
}

int v9x_handle_target_input(V9xConnection *conn, unsigned long id,
                            const unsigned char *payload, unsigned long length)
{
    char title[256];
    unsigned long size, i;
    if (length < 2ul || (size = v9x_read_u16(payload)) >= sizeof(title) || size + 2ul > length)
        return v9x_input_error(conn, id, V9X_STATUS_INVALID_PAYLOAD, 0, "invalid input target");
    for (i = 0ul; i < size; ++i) {
        if (payload[i + 2ul] == 0u || payload[i + 2ul] > 127u)
            return v9x_input_error(conn, id, V9X_STATUS_INVALID_PAYLOAD, 0, "input target must be ASCII");
        title[i] = (char)payload[i + 2ul];
    }
    title[size] = 0;
    return input_run(conn, id, payload + size + 2ul, length - size - 2ul, title, 1);
}
