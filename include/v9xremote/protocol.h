#ifndef V9XREMOTE_PROTOCOL_H
#define V9XREMOTE_PROTOCOL_H

#define V9X_PROTOCOL_MAGIC_0 'V'
#define V9X_PROTOCOL_MAGIC_1 '9'
#define V9X_PROTOCOL_MAGIC_2 'X'
#define V9X_PROTOCOL_MAGIC_3 'R'
#define V9X_PROTOCOL_VERSION 0x0100u
#define V9X_HEADER_SIZE 24u
#define V9X_MAX_PAYLOAD 65536ul
#define V9X_DEFAULT_PORT 9869u

#define V9X_MSG_HELLO_REQUEST  0x0001u
#define V9X_MSG_HELLO_RESPONSE 0x8001u
#define V9X_MSG_PING_REQUEST   0x0002u
#define V9X_MSG_PING_RESPONSE  0x8002u
#define V9X_MSG_EXEC_REQUEST   0x0010u
#define V9X_MSG_EXEC_ACCEPTED  0x8010u
#define V9X_MSG_EXEC_STDOUT    0x9010u
#define V9X_MSG_EXEC_STDERR    0x9011u
#define V9X_MSG_EXEC_COMPLETE  0x8011u
#define V9X_MSG_CANCEL_REQUEST 0x0011u
#define V9X_MSG_CANCEL_RESPONSE 0x8012u
#define V9X_MSG_FILE_STAT_REQUEST    0x0020u
#define V9X_MSG_FILE_STAT_RESPONSE   0x8020u
#define V9X_MSG_FILE_LIST_REQUEST    0x0021u
#define V9X_MSG_FILE_LIST_RESPONSE   0x8021u
#define V9X_MSG_FILE_MKDIR_REQUEST   0x0022u
#define V9X_MSG_FILE_MKDIR_RESPONSE  0x8022u
#define V9X_MSG_FILE_OPEN_WRITE      0x0023u
#define V9X_MSG_FILE_WRITE_READY     0x8023u
#define V9X_MSG_FILE_WRITE_CHUNK     0x0024u
#define V9X_MSG_FILE_WRITE_ACK       0x8024u
#define V9X_MSG_FILE_COMMIT          0x0025u
#define V9X_MSG_FILE_WRITE_COMPLETE  0x8025u
#define V9X_MSG_FILE_OPEN_READ       0x0026u
#define V9X_MSG_FILE_READ_CHUNK      0x9026u
#define V9X_MSG_FILE_READ_COMPLETE   0x8026u
#define V9X_MSG_DOWNLOAD_REQUEST     0x0027u
#define V9X_MSG_DOWNLOAD_PROGRESS    0x9027u
#define V9X_MSG_DOWNLOAD_COMPLETE    0x8027u
#define V9X_MSG_INFO_REQUEST   0x0030u
#define V9X_MSG_INFO_RESPONSE  0x8030u
#define V9X_MSG_REBOOT_REQUEST   0x0040u
#define V9X_MSG_REBOOT_ACCEPTED  0x8040u
#define V9X_MSG_SHUTDOWN_REQUEST 0x0041u
#define V9X_MSG_SHUTDOWN_ACCEPTED 0x8041u
#define V9X_MSG_UPDATE_REQUEST   0x0042u
#define V9X_MSG_UPDATE_ACCEPTED  0x8042u
#define V9X_MSG_SCREENSHOT_REQUEST  0x0050u
#define V9X_MSG_SCREENSHOT_RESPONSE 0x8050u
#define V9X_MSG_INPUT_REQUEST       0x0060u
#define V9X_MSG_INPUT_RESPONSE      0x8060u
#define V9X_MSG_INVENTORY_REQUEST   0x0070u
#define V9X_MSG_INVENTORY_RESPONSE  0x8070u
#define V9X_MSG_ERROR_RESPONSE 0x8fffu

#define V9X_CAP_INFO 0x00000001ul
#define V9X_CAP_PING 0x00000002ul
#define V9X_CAP_EXEC_DIRECT 0x00000004ul
#define V9X_CAP_EXEC_SHELL  0x00000008ul
#define V9X_CAP_EXEC_CANCEL 0x00000010ul
#define V9X_CAP_FILE_READ   0x00000020ul
#define V9X_CAP_FILE_WRITE  0x00000040ul
#define V9X_CAP_POWER_CONTROL  0x00000080ul
#define V9X_CAP_SCREENSHOT_BMP 0x00000100ul
#define V9X_CAP_DRIVER_UPDATE  0x00000200ul
#define V9X_CAP_INPUT_INJECT   0x00000400ul
#define V9X_CAP_EXEC_DETACH    0x00000800ul
#define V9X_CAP_HTTP_DOWNLOAD  0x00001000ul
#define V9X_CAP_INVENTORY      0x00002000ul
/* INFO accepts a u32 flags payload; V9X_INFO_FLAG_EXTENDED appends the
   install ID after allowed_client. Opt-in, so older hosts (which reject
   trailing INFO data) never see it. */
#define V9X_CAP_INSTALL_ID     0x00004000ul
/* INPUT accepts V9X_INPUT_OP_MOUSE_MOVE_RELATIVE (agent 0.8.1+). Older agents
   reject the whole request as malformed, so hosts check this bit first. */
#define V9X_CAP_INPUT_RELATIVE 0x00008000ul
#define V9X_INFO_FLAG_EXTENDED 0x00000001ul
#define V9X_CAPABILITIES (V9X_CAP_INFO | V9X_CAP_PING | \
                          V9X_CAP_EXEC_DIRECT | V9X_CAP_EXEC_SHELL | \
                          V9X_CAP_EXEC_CANCEL | V9X_CAP_FILE_READ | \
                          V9X_CAP_FILE_WRITE | V9X_CAP_POWER_CONTROL | \
                          V9X_CAP_SCREENSHOT_BMP | V9X_CAP_DRIVER_UPDATE | \
                          V9X_CAP_INPUT_INJECT | V9X_CAP_EXEC_DETACH | \
                          V9X_CAP_HTTP_DOWNLOAD | V9X_CAP_INVENTORY | \
                          V9X_CAP_INSTALL_ID | V9X_CAP_INPUT_RELATIVE)

#define V9X_MAX_FILE_SIZE 268435456ul
/* DOWNLOAD streams straight to the guest disk, so it is not held to the
 * PUT/GET limit; 2 GiB keeps every size well inside the u32 wire fields. */
#define V9X_MAX_DOWNLOAD_SIZE 2147483647ul
#define V9X_FILE_CHUNK_SIZE 32768ul

#define V9X_EXEC_MODE_DIRECT 0u
#define V9X_EXEC_MODE_SHELL  1u
#define V9X_EXEC_OPTION_DETACH 0x0001u
#define V9X_EXEC_OPTION_MASK   0x0001u
#define V9X_EXEC_RESULT_OK            0ul
#define V9X_EXEC_RESULT_CREATE_FAILED 1ul
#define V9X_EXEC_RESULT_TIMEOUT       2ul
#define V9X_EXEC_RESULT_CANCELLED     3ul
#define V9X_EXEC_RESULT_INTERNAL      4ul
#define V9X_EXEC_FLAG_STDOUT_TRUNCATED 0x00000001ul
#define V9X_EXEC_FLAG_STDERR_TRUNCATED 0x00000002ul
#define V9X_EXEC_FLAG_TIMED_OUT        0x00000004ul
#define V9X_EXEC_FLAG_CANCELLED        0x00000008ul
#define V9X_EXEC_FLAG_PIPE_CAPTURE     0x00000010ul
#define V9X_EXEC_FLAG_GUI_WINDOW       0x00000020ul
#define V9X_EXEC_FLAG_DETACHED         0x00000040ul
#define V9X_EXEC_FLAG_ORPHANED         0x00000080ul

/* Input-injection action opcodes (V9X_MSG_INPUT_REQUEST payload).
   Payload: u16 action count, then that many actions, each a u8 opcode
   followed by opcode-specific little-endian fields described below. */
#define V9X_INPUT_OP_MOUSE_MOVE   1u  /* s32 x, s32 y (absolute screen pixels) */
#define V9X_INPUT_OP_MOUSE_BUTTON 2u  /* u8 button, u8 down */
#define V9X_INPUT_OP_MOUSE_WHEEL  3u  /* s16 notches (positive = away from user) */
#define V9X_INPUT_OP_KEY          4u  /* u8 virtual-key, u8 down */
#define V9X_INPUT_OP_TYPE         5u  /* u16 length, ASCII bytes */
#define V9X_INPUT_OP_DELAY        6u  /* u16 milliseconds */
/* s32 dx, s32 dy: a relative mouse_event, as a real mouse moves, for
   programs that read motion deltas and recentre the cursor (games) and so
   never see an absolute SetCursorPos jump as motion. Sent in steps of at most
   V9X_INPUT_RELATIVE_STEP pixels so Windows' mouse acceleration (threshold 6
   by default) leaves the distance unscaled. */
#define V9X_INPUT_OP_MOUSE_MOVE_RELATIVE 7u
#define V9X_INPUT_RELATIVE_STEP 4l

#define V9X_INPUT_BUTTON_LEFT   0u
#define V9X_INPUT_BUTTON_RIGHT  1u
#define V9X_INPUT_BUTTON_MIDDLE 2u

#define V9X_INPUT_MAX_ACTIONS 256ul
#define V9X_INPUT_MAX_DELAY_MS 10000ul

typedef struct V9xFrameHeader {
    unsigned short version;
    unsigned short type;
    unsigned long request_id;
    unsigned long flags;
    unsigned long payload_length;
    unsigned long reserved;
} V9xFrameHeader;

unsigned short v9x_read_u16(const unsigned char *source);
unsigned long v9x_read_u32(const unsigned char *source);
void v9x_write_u16(unsigned char *target, unsigned short value);
void v9x_write_u32(unsigned char *target, unsigned long value);
int v9x_decode_header(const unsigned char *source, V9xFrameHeader *header);
void v9x_encode_header(unsigned char *target, const V9xFrameHeader *header);
unsigned long v9x_bounded_length(const char *text, unsigned long maximum);
int v9x_append_string(unsigned char *target, unsigned long capacity,
                      unsigned long *offset, const char *text);

#endif
