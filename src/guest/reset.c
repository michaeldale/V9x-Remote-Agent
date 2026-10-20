#include "agent.h"
#include "platform_win.h"

/* RESET: a hard reset after as much of a clean shutdown as ring 3 can do,
   for Win9x machines whose restart hangs once Windows drops back to real
   mode (a real-mode NDIS2 driver still resident, for one). Measured with
   tests\integration\portprobe.c, resetprb.c and flushprb.c on a PIIX4 VM
   and an ICH7 netbook:

   - The System VM's I/O permission bitmap leaves CF8h-CFBh and 92h
     untrapped, so a Win32 thread reaches the reset control register (CF9h)
     and port A (92h) directly. 64h is trapped by VKD.
   - Int 21h 710Dh (reset drive) is not reachable through either VWIN32
     ioctl, but granting a level-1 lock on a volume (440Dh/084Ah through
     VWIN32_DIOC_DOS_IOCTL) commits VCACHE for it and leaves the FAT marked
     clean: the files and registry values written just before the reset were
     on disk and ScanDisk did not run at the next boot. Without it the reset
     lost a directory created a moment earlier and the rename at the end of
     a PUT finished seconds before, and ScanDisk ran.
   - The reset is CF9h's full reset (0Ah, then 0Eh): the chipset drops
     SLP_S3#/S4#/S5# for a few seconds, a real power cycle. The hard reset
     (02h, 06h) worked on the VM but left the ICH7 netbook dark until its
     power button was held, as did either it or the port 92h INIT tried
     after it. There is no fallback: a trapped FEh to port 64h goes through
     VKD into Windows' own restart path, the real-mode return this verb
     exists to avoid (and it waits forever for a held volume lock). When
     CF9h does nothing the agent releases the locks and stays up, so a
     failed reset costs a timed-out proof, not a hung machine.
   - The full reset cuts the drive's power too. On the netbook the last
     AGENT.LOG lines before the locks reached the directory but not the
     data area: the lock had handed them to the drive, whose own write cache
     was lost. So the log is frozen once the flush starts, and the disk is
     left idle for V9X_RESET_SETTLE_MS before the reset. */

unsigned char v9x_port_in(unsigned short port);
#pragma aux v9x_port_in = "in al, dx" parm [dx] value [al];

void v9x_port_out(unsigned short port, unsigned char value);
#pragma aux v9x_port_out = "out dx, al" parm [dx] [al];

#define V9X_VWIN32_DIOC_DOS_IOCTL 1ul
#define V9X_NOTIFY_WINDOW_MS 2000u
#define V9X_NOTIFY_TOTAL_MS 8000ul
#define V9X_RESET_SETTLE_MS 3000ul

typedef struct {
    DWORD reg_EBX;
    DWORD reg_EDX;
    DWORD reg_ECX;
    DWORD reg_EAX;
    DWORD reg_EDI;
    DWORD reg_ESI;
    DWORD reg_Flags;
} V9xDiocRegisters;

typedef struct {
    DWORD self;
    DWORD start;
    unsigned long sent;
} V9xNotifyState;

static unsigned long v9x_reset_number(char *target, unsigned long offset,
                                      unsigned long value)
{
    char digits[10];
    unsigned long count = 0ul;
    do {
        digits[count++] = (char)('0' + value % 10ul);
        value /= 10ul;
    } while (value != 0ul);
    while (count != 0ul) target[offset++] = digits[--count];
    target[offset] = '\0';
    return offset;
}

/* EWX_FORCE semantics: no WM_QUERYENDSESSION, so nothing can veto or put
   up a "save changes?" prompt, but each top-level window of every other
   process gets WM_ENDSESSION(TRUE) and the chance to write its state, as
   it would just before a real shutdown. WinOldAp ("tty") windows are left
   alone: they front DOS VMs. A hung window costs at most
   V9X_NOTIFY_WINDOW_MS, and the whole pass V9X_NOTIFY_TOTAL_MS. */
static BOOL CALLBACK v9x_reset_notify_window(HWND window, LPARAM context)
{
    V9xNotifyState *state = (V9xNotifyState *)context;
    DWORD owner = 0ul;
    DWORD result = 0ul;
    char class_name[8];
    if (GetTickCount() - state->start > V9X_NOTIFY_TOTAL_MS) return FALSE;
    (void)GetWindowThreadProcessId(window, &owner);
    if (owner == state->self) return TRUE;
    if (GetClassNameA(window, class_name, sizeof(class_name)) == 3 &&
        lstrcmpiA(class_name, "tty") == 0) {
        return TRUE;
    }
    if (SendMessageTimeoutA(window, WM_ENDSESSION, (WPARAM)TRUE, 0,
                            SMTO_ABORTIFHUNG | SMTO_NORMAL,
                            V9X_NOTIFY_WINDOW_MS, &result)) {
        ++state->sent;
    }
    return TRUE;
}

static int v9x_reset_volume_lock(HANDLE vwin32, unsigned drive, int lock)
{
    V9xDiocRegisters regs;
    DWORD returned = 0ul;
    regs.reg_EAX = 0x440dul;
    /* Lock: BH=level 1, BL=drive (1=A:), DX=1 so other writers are not
       refused in the moment before the reset. Unlock: BL=drive. */
    regs.reg_EBX = lock ? (0x0100ul | drive) : drive;
    regs.reg_ECX = lock ? 0x084aul : 0x086aul;
    regs.reg_EDX = lock ? 1ul : 0ul;
    regs.reg_EDI = 0ul;
    regs.reg_ESI = 0ul;
    regs.reg_Flags = 1ul; /* carry preset: still set means it did not run */
    if (!DeviceIoControl(vwin32, V9X_VWIN32_DIOC_DOS_IOCTL, &regs,
                         sizeof(regs), &regs, sizeof(regs), &returned, 0)) {
        return 0;
    }
    return (regs.reg_Flags & 1ul) == 0ul;
}

void v9x_reset_machine(void)
{
    V9xNotifyState notify;
    HANDLE vwin32;
    DWORD drives;
    DWORD locked = 0ul;
    unsigned drive;
    unsigned char control;
    char detail[64];
    unsigned long offset;

    notify.self = GetCurrentProcessId();
    notify.start = GetTickCount();
    notify.sent = 0ul;
    (void)EnumWindows(v9x_reset_notify_window, (LPARAM)&notify);
    offset = v9x_reset_number(detail, 0ul, notify.sent);
    detail[offset++] = ' ';
    offset = v9x_reset_number(detail, offset, GetTickCount() - notify.start);
    detail[offset++] = 'm';
    detail[offset++] = 's';
    detail[offset] = '\0';
    v9x_log_event("reset-notified", detail);

    (void)RegFlushKey(HKEY_LOCAL_MACHINE);
    (void)RegFlushKey(HKEY_USERS);
    (void)RegFlushKey(HKEY_CURRENT_USER);

    drives = GetLogicalDrives();
    offset = 0ul;
    for (drive = 2u; drive < 26u; ++drive) {
        char root[4];
        if ((drives & (1ul << drive)) == 0ul) continue;
        root[0] = (char)('A' + drive);
        root[1] = ':';
        root[2] = '\\';
        root[3] = '\0';
        if (GetDriveTypeA(root) != DRIVE_FIXED) {
            drives &= ~(1ul << drive);
            continue;
        }
        detail[offset++] = root[0];
    }
    detail[offset] = '\0';
    /* The last line written before the locks; the lock commits it, so
       finding it in AGENT.LOG after the boot shows the flush worked. */
    v9x_log_event("reset-flush", detail);
    v9x_log_freeze(1);

    vwin32 = CreateFileA("\\\\.\\VWIN32", 0ul, 0ul, 0, 0ul,
                         FILE_FLAG_DELETE_ON_CLOSE, 0);
    if (vwin32 != INVALID_HANDLE_VALUE) {
        for (drive = 2u; drive < 26u; ++drive) {
            if ((drives & (1ul << drive)) != 0ul &&
                v9x_reset_volume_lock(vwin32, drive + 1u, 1)) {
                locked |= 1ul << drive;
            }
        }
    }

    Sleep(V9X_RESET_SETTLE_MS);

    /* ICH7 RST_CNT / PIIX4 RC: FULL_RST and SYS_RST first, then RST_CPU
       0->1 starts the reset. Reserved bits 7:4 and bit 0 are kept. */
    control = (unsigned char)(v9x_port_in(0x0cf9u) & 0xf1u);
    v9x_port_out(0x0cf9u, (unsigned char)(control | 0x0au));
    v9x_port_out(0x0cf9u, (unsigned char)(control | 0x0eu));
    /* A full reset holds the sleep signals for 3-5 s before the restart, so
       give it longer than that before concluding nothing happened. */
    Sleep(8000ul);

    if (vwin32 != INVALID_HANDLE_VALUE) {
        for (drive = 2u; drive < 26u; ++drive) {
            if ((locked & (1ul << drive)) != 0ul) {
                (void)v9x_reset_volume_lock(vwin32, drive + 1u, 0);
            }
        }
        CloseHandle(vwin32);
    }

    offset = 0ul;
    for (drive = 2u; drive < 26u; ++drive) {
        if ((locked & (1ul << drive)) != 0ul) detail[offset++] = (char)('A' + drive);
    }
    detail[offset] = '\0';
    v9x_log_freeze(0);
    v9x_log_event("reset-failed", offset != 0ul ? detail : "no-lock");
}
