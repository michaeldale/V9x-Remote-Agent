/* RESETPRB.EXE: does a flush survive a hard reset on Windows 9x?

   Run on a disposable VM only: it resets the machine.

     RESETPRB none    write, then reset at once (negative control)
     RESETPRB lock    write, RegFlushKey, level-1 lock of C: (VFAT flushes
                      the volume to grant it), then reset while holding it
     RESETPRB full    as lock, but CF9h's full reset (0Ah, 0Eh), as the
                      agent does
     RESETPRB kbc     write, then reset by the 8042 pulse (FEh to port 64h,
                      which VKD traps) instead of CF9; no flush
     RESETPRB p92     write, then reset by port 92h bit 0 (fast INIT); no
                      flush
     RESETPRB check   no reset: report what the previous run left behind

   A run writes C:\V9XREMOTE\JOBS\RSTPRB\<MODE>.DAT (64 KiB, no
   FlushFileBuffers) and HKLM\Software\V9xResetProbe <mode>=tick, then
   resets through port CF9 (02h, 06h). After the boot, `check` (or stat)
   shows whether the file and value made it to disk.

   GUI subsystem on purpose (no DOS VM). Built with the normal Watcom CRT:
     wcl386 -bt=nt -l=nt_win -zq tests\integration\resetprb.c advapi32.lib */

#include <stdio.h>
#include <string.h>
#include <windows.h>

void probe_outb(unsigned short port, unsigned char value);
#pragma aux probe_outb = "out dx, al" parm [dx] [al];

unsigned char probe_inb(unsigned short port);
#pragma aux probe_inb = "in al, dx" parm [dx] value [al];

typedef struct {
    DWORD reg_EBX;
    DWORD reg_EDX;
    DWORD reg_ECX;
    DWORD reg_EAX;
    DWORD reg_EDI;
    DWORD reg_ESI;
    DWORD reg_Flags;
} DiocRegisters;

static const char dir[] = "C:\\V9XREMOTE\\JOBS\\RSTPRB";
static const char key_name[] = "Software\\V9xResetProbe";

static void write_marker(const char *mode)
{
    static char block[65536];
    char path[MAX_PATH];
    HANDLE file;
    DWORD written;
    memset(block, 'A' + (GetTickCount() % 26ul), sizeof(block));
    CreateDirectoryA(dir, NULL);
    sprintf(path, "%s\\%s.DAT", dir, mode);
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, block, sizeof(block), &written, NULL);
        CloseHandle(file);
    }
}

static void write_value(const char *mode)
{
    HKEY key;
    DWORD disposition;
    DWORD tick = GetTickCount();
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, key_name, 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &key, &disposition) == 0) {
        RegSetValueExA(key, mode, 0, REG_DWORD, (const BYTE *)&tick,
                       sizeof(tick));
        RegCloseKey(key);
    }
}

static void lock_volume(void)
{
    HANDLE vwin32;
    DiocRegisters regs;
    DWORD returned;
    vwin32 = CreateFileA("\\\\.\\VWIN32", 0, 0, NULL, 0,
                         FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (vwin32 == INVALID_HANDLE_VALUE) return;
    memset(&regs, 0, sizeof(regs));
    regs.reg_EAX = 0x440dul;
    regs.reg_EBX = (1ul << 8) | 3ul;
    regs.reg_ECX = 0x084aul;
    regs.reg_EDX = 1ul;
    regs.reg_Flags = 1ul;
    DeviceIoControl(vwin32, 1ul, &regs, sizeof(regs), &regs, sizeof(regs),
                    &returned, NULL);
    /* Handle left open: the lock is held through the reset. */
}

static void check(void)
{
    static const char *modes[] = { "none", "lock", "full", "kbc", "p92" };
    char path[MAX_PATH];
    char text[256];
    FILE *out;
    HKEY key;
    unsigned index;
    sprintf(path, "%s\\CHECK.TXT", dir);
    out = fopen(path, "wb");
    if (out == NULL) return;
    for (index = 0; index < 5; ++index) {
        WIN32_FIND_DATAA found;
        HANDLE search;
        DWORD value = 0ul;
        DWORD size = sizeof(value);
        DWORD type;
        int have_value = 0;
        sprintf(path, "%s\\%s.DAT", dir, modes[index]);
        search = FindFirstFileA(path, &found);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key_name, 0, KEY_READ, &key) == 0) {
            have_value = RegQueryValueExA(key, modes[index], NULL, &type,
                                          (BYTE *)&value, &size) == 0;
            RegCloseKey(key);
        }
        sprintf(text, "%s: file=%s size=%lu value=%s %lu\r\n", modes[index],
                search == INVALID_HANDLE_VALUE ? "missing" : "present",
                search == INVALID_HANDLE_VALUE ? 0ul : found.nFileSizeLow,
                have_value ? "present" : "missing", value);
        if (search != INVALID_HANDLE_VALUE) FindClose(search);
        fputs(text, out);
    }
    fclose(out);
}

int PASCAL WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command,
                   int show)
{
    (void)instance; (void)previous; (void)show;
    while (*command == ' ') ++command;
    if (strncmp(command, "check", 5) == 0) {
        check();
        return 0;
    }
    if (strncmp(command, "none", 4) == 0) {
        write_marker("none");
        write_value("none");
    } else if (strncmp(command, "lock", 4) == 0) {
        write_marker("lock");
        write_value("lock");
        RegFlushKey(HKEY_LOCAL_MACHINE);
        lock_volume();
    } else if (strncmp(command, "full", 4) == 0) {
        unsigned char control;
        write_marker("full");
        write_value("full");
        RegFlushKey(HKEY_LOCAL_MACHINE);
        lock_volume();
        control = (unsigned char)(probe_inb(0x0cf9) & 0xf1u);
        probe_outb(0x0cf9, (unsigned char)(control | 0x0au));
        probe_outb(0x0cf9, (unsigned char)(control | 0x0eu));
        Sleep(8000);
        return 1;
    } else if (strncmp(command, "kbc", 3) == 0 ||
               strncmp(command, "p92", 3) == 0) {
        write_marker(command[0] == 'k' ? "kbc" : "p92");
        if (command[0] == 'k') {
            unsigned long spin;
            for (spin = 0ul; spin < 65536ul && (probe_inb(0x64) & 2u); ++spin) {}
            probe_outb(0x0064, 0xfe);
            Sleep(2000);
            return 1;
        }
        if (command[0] == 'p') {
            unsigned char port_a = (unsigned char)(probe_inb(0x92) & 0xfeu);
            probe_outb(0x0092, port_a);
            probe_outb(0x0092, (unsigned char)(port_a | 1u));
            Sleep(2000);
            return 1;
        }
    } else {
        return 2;
    }
    probe_outb(0x0cf9, 0x02);
    probe_outb(0x0cf9, 0x06);
    Sleep(2000);
    return 1;
}
