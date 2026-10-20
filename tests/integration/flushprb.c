/* FLUSHPRB.EXE: which VWIN32 ioctl runs Int 21h 710Dh (Reset Drive) on
   Windows 9x, the volume-wide "commit buffers and VCACHE" call the agent's
   `reset` verb relies on.

   For drive C: it issues 710Dh with CX=0 (flush buffers) and CX=1 (flush
   buffers and cache) through VWIN32_DIOC_DOS_IOCTL (1) and
   VWIN32_DIOC_DOS_DRIVEINFO (6), with the carry flag preset, and reports
   the DeviceIoControl result, carry and AX. Carry clear means the call ran
   and succeeded. Flushing is harmless; nothing else is written.

   GUI subsystem on purpose (no DOS VM). Writes FLUSHPRB.TXT next to itself.
   Built with the normal Watcom CRT (it is a test tool, not shipped):
     wcl386 -bt=nt -l=nt_win -zq tests\integration\flushprb.c */

#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef struct {
    DWORD reg_EBX;
    DWORD reg_EDX;
    DWORD reg_ECX;
    DWORD reg_EAX;
    DWORD reg_EDI;
    DWORD reg_ESI;
    DWORD reg_Flags;
} DiocRegisters;

int PASCAL WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command,
                   int show)
{
    static const DWORD codes[] = { 1ul, 6ul };
    char path[MAX_PATH];
    char *dot;
    FILE *out;
    HANDLE vwin32;
    unsigned code_index;
    DWORD mode;
    (void)previous; (void)command; (void)show;
    GetModuleFileNameA(instance, path, sizeof(path) - 4);
    dot = strrchr(path, '.');
    if (dot == NULL) dot = path + strlen(path);
    strcpy(dot, ".TXT");
    out = fopen(path, "wb");
    if (out == NULL) return 2;
    vwin32 = CreateFileA("\\\\.\\VWIN32", 0, 0, NULL, 0,
                         FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (vwin32 == INVALID_HANDLE_VALUE) {
        fprintf(out, "open VWIN32 failed: %lu\r\n", GetLastError());
        fclose(out);
        return 1;
    }
    for (code_index = 0; code_index < 2; ++code_index) {
        for (mode = 0ul; mode <= 1ul; ++mode) {
            DiocRegisters regs;
            DWORD returned = 0ul;
            DWORD start;
            BOOL ok;
            memset(&regs, 0, sizeof(regs));
            regs.reg_EAX = 0x710dul;
            regs.reg_ECX = mode;
            regs.reg_EDX = 3ul;            /* 1 = A:, 3 = C: */
            regs.reg_Flags = 1ul;          /* carry preset: "not run" */
            start = GetTickCount();
            ok = DeviceIoControl(vwin32, codes[code_index], &regs, sizeof(regs),
                                 &regs, sizeof(regs), &returned, NULL);
            fprintf(out, "dioc=%lu cx=%lu ok=%d err=%lu carry=%lu ax=%04lX ms=%lu\r\n",
                    codes[code_index], mode, ok ? 1 : 0,
                    ok ? 0ul : GetLastError(), regs.reg_Flags & 1ul,
                    regs.reg_EAX & 0xfffful, GetTickCount() - start);
            fflush(out);
        }
    }
    /* Lock Logical Volume (440Dh CX=084Ah, BH=level, BL=drive, DX=perms),
       then Unlock (CX=086Ah). Granting a lock makes VFAT flush the volume,
       so the time it takes is part of the answer. */
    for (mode = 0ul; mode <= 1ul; ++mode) {
        DiocRegisters regs;
        DWORD returned = 0ul;
        DWORD start;
        BOOL ok;
        memset(&regs, 0, sizeof(regs));
        regs.reg_EAX = 0x440dul;
        regs.reg_EBX = (mode << 8) | 3ul;
        regs.reg_ECX = 0x084aul;
        regs.reg_EDX = 1ul;                /* allow writes by others */
        regs.reg_Flags = 1ul;
        start = GetTickCount();
        ok = DeviceIoControl(vwin32, 1ul, &regs, sizeof(regs), &regs,
                             sizeof(regs), &returned, NULL);
        fprintf(out, "lock level=%lu ok=%d carry=%lu ax=%04lX ms=%lu\r\n",
                mode, ok ? 1 : 0, regs.reg_Flags & 1ul,
                regs.reg_EAX & 0xfffful, GetTickCount() - start);
        fflush(out);
        if (ok && (regs.reg_Flags & 1ul) == 0ul) {
            memset(&regs, 0, sizeof(regs));
            regs.reg_EAX = 0x440dul;
            regs.reg_EBX = 3ul;
            regs.reg_ECX = 0x086aul;
            regs.reg_Flags = 1ul;
            ok = DeviceIoControl(vwin32, 1ul, &regs, sizeof(regs), &regs,
                                 sizeof(regs), &returned, NULL);
            fprintf(out, "unlock ok=%d carry=%lu ax=%04lX\r\n", ok ? 1 : 0,
                    regs.reg_Flags & 1ul, regs.reg_EAX & 0xfffful);
            fflush(out);
        }
    }
    CloseHandle(vwin32);
    fprintf(out, "done\r\n");
    fclose(out);
    return 0;
}
