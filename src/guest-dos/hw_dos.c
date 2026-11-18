/* Hardware probes for the DOS agent: memory, I/O ports, the text screen, the
   BIOS keyboard buffer and reboot - COMrade's command set over the V9x
   connection. They run between programs, while the agent is in the foreground.

   DOS/4GW maps the first megabyte one to one into the flat data segment, so a
   linear address below 1 MB (plus the HMA) is used directly as a pointer. */

#include <conio.h>
#include <dos.h>
#include <i86.h>
#include <process.h>
#include <stdio.h>
#include <string.h>

#include "dosagent.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"

#define V9X_DOS_MEM_LIMIT  0x110000ul  /* first megabyte plus the HMA */
#define V9X_DOS_MEM_CHUNK  32768ul
#define V9X_DOS_IO_MAX     4096ul

#define BDA(offset) ((volatile unsigned char *)(0x400ul + (offset)))
#define BDA16(offset) (*(volatile unsigned short *)(0x400ul + (offset)))

static int v9x_range_ok(unsigned long address, unsigned long length)
{
    return length != 0ul && address < V9X_DOS_MEM_LIMIT &&
           length <= V9X_DOS_MEM_LIMIT - address;
}

static int v9x_mem_read(V9xDos *dos, unsigned long request_id,
                        const unsigned char *payload, unsigned long length)
{
    unsigned long address;
    unsigned long count;
    if (length != 8ul) goto invalid;
    address = v9x_read_u32(payload);
    count = v9x_read_u32(payload + 4);
    if (count > V9X_DOS_MEM_CHUNK || !v9x_range_ok(address, count)) goto invalid;
    return v9x_send_frame(dos, V9X_MSG_MEM_READ_RESPONSE, request_id,
                          (const unsigned char *)address, count);
invalid:
    return v9x_send_error(dos, request_id, V9X_STATUS_INVALID_PAYLOAD,
                          "invalid memory range");
}

static int v9x_mem_write(V9xDos *dos, unsigned long request_id,
                         const unsigned char *payload, unsigned long length)
{
    unsigned long address;
    unsigned long count;
    if (length < 5ul) goto invalid;
    address = v9x_read_u32(payload);
    count = length - 4ul;
    if (!v9x_range_ok(address, count)) goto invalid;
    memcpy((void *)address, payload + 4, count);
    v9x_write_u32(dos->response, count);
    return v9x_send_frame(dos, V9X_MSG_MEM_WRITE_RESPONSE, request_id,
                          dos->response, 4ul);
invalid:
    return v9x_send_error(dos, request_id, V9X_STATUS_INVALID_PAYLOAD,
                          "invalid memory range");
}

/* Payload: u16 port, u8 width (1 or 2), u16 count, then for OUT count values.
   Repeated access to one port, as a data or status register is read. */
static int v9x_io(V9xDos *dos, unsigned long request_id, int out,
                  const unsigned char *payload, unsigned long length)
{
    unsigned short port;
    unsigned int width;
    unsigned long count;
    unsigned long index;
    unsigned char *values = dos->file_response;
    if (length < 5ul) goto invalid;
    port = v9x_read_u16(payload);
    width = payload[2];
    count = v9x_read_u16(payload + 3);
    if ((width != 1u && width != 2u) || count == 0ul || count > V9X_DOS_IO_MAX ||
        length != 5ul + (out ? count * width : 0ul)) goto invalid;
    for (index = 0ul; index < count; ++index) {
        if (out) {
            const unsigned char *value = payload + 5ul + index * width;
            if (width == 1u) outp(port, value[0]);
            else outpw(port, v9x_read_u16(value));
        } else if (width == 1u) {
            values[index] = (unsigned char)inp(port);
        } else {
            v9x_write_u16(values + index * 2ul, (unsigned short)inpw(port));
        }
    }
    if (out) {
        v9x_write_u32(dos->response, count);
        return v9x_send_frame(dos, V9X_MSG_IO_OUT_RESPONSE, request_id,
                              dos->response, 4ul);
    }
    return v9x_send_frame(dos, V9X_MSG_IO_IN_RESPONSE, request_id, values,
                          count * width);
invalid:
    return v9x_send_error(dos, request_id, V9X_STATUS_INVALID_PAYLOAD,
                          "invalid port request");
}

/* Response: u8 mode, u8 columns, u8 rows, u8 cursor column, u8 cursor row,
   u8 page, then columns x rows character/attribute pairs. The XT BIOS does not
   keep the row count (40:84h), so 25 is assumed when it reads 0. */
static int v9x_screen(V9xDos *dos, unsigned long request_id)
{
    unsigned char mode = *BDA(0x49);
    unsigned char columns = *BDA(0x4A);
    unsigned char rows = (unsigned char)(*BDA(0x84) + 1u);
    unsigned char page = *BDA(0x62);
    unsigned long base = (mode == 7u) ? 0xB0000ul : 0xB8000ul;
    unsigned long size;
    unsigned char *out = dos->file_response;
    if (rows < 2u || rows > 60u) rows = 25u;
    if (columns == 0u || columns > 132u) columns = 80u;
    size = (unsigned long)columns * rows * 2ul;
    out[0] = mode;
    out[1] = columns;
    out[2] = rows;
    out[3] = *BDA(0x50 + page * 2u);
    out[4] = *BDA(0x51 + page * 2u);
    out[5] = page;
    memcpy(out + 6, (const void *)(base + BDA16(0x4E)), size);
    return v9x_send_frame(dos, V9X_MSG_SCREEN_RESPONSE, request_id, out,
                          6ul + size);
}

/* Payload: u16 words (scan code high, character low) appended to the BIOS
   keyboard buffer, as INT 16h would return them. Stops when the buffer fills. */
static int v9x_keys(V9xDos *dos, unsigned long request_id,
                    const unsigned char *payload, unsigned long length)
{
    unsigned short start = BDA16(0x80);
    unsigned short end = BDA16(0x82);
    unsigned long index;
    unsigned long stuffed = 0ul;
    if (length == 0ul || (length & 1ul) != 0ul) {
        return v9x_send_error(dos, request_id, V9X_STATUS_INVALID_PAYLOAD,
                              "invalid key list");
    }
    if (start == 0u || end <= start) {  /* the 1981 BIOS leaves these unset */
        start = 0x1Eu;
        end = 0x3Eu;
    }
    for (index = 0ul; index < length; index += 2ul) {
        unsigned short tail;
        unsigned short next;
        _disable();
        tail = BDA16(0x1C);
        next = (unsigned short)(tail + 2u);
        if (next >= end) next = start;
        if (next == BDA16(0x1A)) {
            _enable();
            break;
        }
        *(volatile unsigned short *)(0x400ul + tail) = v9x_read_u16(payload + index);
        BDA16(0x1C) = next;
        _enable();
        ++stuffed;
    }
    v9x_write_u32(dos->response, stuffed);
    return v9x_send_frame(dos, V9X_MSG_KEYS_RESPONSE, request_id,
                          dos->response, 4ul);
}

/* The reset vector is real-mode code, so the restart runs as a small real-mode
   .COM child: COMrade's OP_REBOOT steps. 1234h at 40:72h makes it a warm
   start; FEh to port 64h pulses an AT keyboard controller's reset line (an XT
   has none); the far jump to FFFF:0000 restarts the BIOS either way. */
void v9x_dos_reboot(int warm)
{
    static unsigned char code[] = {
        0xB8, 0x40, 0x00,                   /* mov ax, 0040h          */
        0x8E, 0xD8,                         /* mov ds, ax             */
        0xC7, 0x06, 0x72, 0x00, 0x00, 0x00, /* mov word [0072h], flag */
        0xB0, 0xFE,                         /* mov al, 0FEh           */
        0xE6, 0x64,                         /* out 64h, al            */
        0xEA, 0x00, 0x00, 0xFF, 0xFF        /* jmp far FFFF:0000      */
    };
    FILE *file;
    if (warm) {
        code[9] = 0x34u;
        code[10] = 0x12u;
    }
    file = fopen(V9X_DOS_REBOOT_COM, "wb");
    if (file == 0) return;
    if (fwrite(code, sizeof(code), 1u, file) != 1u) {
        fclose(file);
        return;
    }
    fclose(file);
    (void)spawnl(P_WAIT, V9X_DOS_REBOOT_COM, V9X_DOS_REBOOT_COM, (char *)0);
}

int v9x_is_hw_message(unsigned short type)
{
    return type >= V9X_MSG_MEM_READ_REQUEST && type <= V9X_MSG_REBOOT_DOS_REQUEST;
}

int v9x_handle_hw_message(V9xDos *dos, unsigned short type,
                          unsigned long request_id,
                          const unsigned char *payload, unsigned long length)
{
    switch (type) {
    case V9X_MSG_MEM_READ_REQUEST:
        return v9x_mem_read(dos, request_id, payload, length);
    case V9X_MSG_MEM_WRITE_REQUEST:
        return v9x_mem_write(dos, request_id, payload, length);
    case V9X_MSG_IO_IN_REQUEST:
        return v9x_io(dos, request_id, 0, payload, length);
    case V9X_MSG_IO_OUT_REQUEST:
        return v9x_io(dos, request_id, 1, payload, length);
    case V9X_MSG_SCREEN_REQUEST:
        if (length != 0ul) break;
        return v9x_screen(dos, request_id);
    case V9X_MSG_KEYS_REQUEST:
        return v9x_keys(dos, request_id, payload, length);
    case V9X_MSG_REBOOT_DOS_REQUEST:
        if (length != 1ul || payload[0] > 1u) break;
        dos->reboot = payload[0] ? 2 : 1;
        return v9x_send_frame(dos, V9X_MSG_REBOOT_DOS_RESPONSE, request_id,
                              0, 0ul);
    }
    return v9x_send_error(dos, request_id, V9X_STATUS_INVALID_PAYLOAD,
                          "invalid probe request");
}
