#include "agent.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"

static unsigned char v9x_input_response[16];

static int v9x_input_error(V9xAgentState *state, unsigned long request_id,
                           unsigned long status, unsigned long native_error,
                           const char *detail)
{
    unsigned long offset = 8ul;
    v9x_write_u32(v9x_input_response, status);
    v9x_write_u32(v9x_input_response + 4, native_error);
    if (!v9x_append_string(v9x_input_response, sizeof(v9x_input_response),
                           &offset, detail)) return 0;
    return v9x_send_frame(state, V9X_MSG_ERROR_RESPONSE, request_id,
                          v9x_input_response, offset);
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

static void v9x_input_apply(const unsigned char *payload, unsigned long count)
{
    unsigned long offset = 2ul;
    unsigned long index;
    unsigned long text_length;
    unsigned long character;
    unsigned char opcode;
    for (index = 0ul; index < count; ++index) {
        opcode = payload[offset++];
        switch (opcode) {
        case V9X_INPUT_OP_MOUSE_MOVE:
            (void)SetCursorPos((int)v9x_read_s32(payload + offset),
                               (int)v9x_read_s32(payload + offset + 4ul));
            offset += 8ul;
            break;
        case V9X_INPUT_OP_MOUSE_BUTTON:
            v9x_input_mouse_button(payload[offset], payload[offset + 1ul]);
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
            offset += 2ul;
            break;
        case V9X_INPUT_OP_TYPE:
            text_length = (unsigned long)v9x_read_u16(payload + offset);
            offset += 2ul;
            for (character = 0ul; character < text_length; ++character) {
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
}

int v9x_handle_input(V9xAgentState *state, unsigned long request_id,
                     const unsigned char *payload, unsigned long length)
{
    unsigned long count;
    POINT cursor;
    if (length < 2ul) {
        return v9x_input_error(state, request_id, V9X_STATUS_INVALID_PAYLOAD,
                               ERROR_INVALID_PARAMETER, "input payload too short");
    }
    count = (unsigned long)v9x_read_u16(payload);
    if (count > V9X_INPUT_MAX_ACTIONS) {
        return v9x_input_error(state, request_id, V9X_STATUS_LIMIT_EXCEEDED,
                               ERROR_INVALID_PARAMETER, "too many input actions");
    }
    if (!v9x_input_validate(payload, length, count)) {
        return v9x_input_error(state, request_id, V9X_STATUS_INVALID_PAYLOAD,
                               ERROR_INVALID_PARAMETER, "malformed input actions");
    }
    v9x_input_apply(payload, count);
    cursor.x = 0l;
    cursor.y = 0l;
    (void)GetCursorPos(&cursor);
    v9x_write_u32(v9x_input_response, count);
    v9x_write_u32(v9x_input_response + 4, (unsigned long)(long)cursor.x);
    v9x_write_u32(v9x_input_response + 8, (unsigned long)(long)cursor.y);
    v9x_log_line("input-complete");
    return v9x_send_frame(state, V9X_MSG_INPUT_RESPONSE, request_id,
                          v9x_input_response, 12ul);
}
