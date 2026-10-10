#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "v9xremote/installid.h"
#include "installid_win.h"

/* Shared by V9XAGNT.EXE and SETUP.EXE, so it depends on nothing but KERNEL32.
   RDTSC is deliberately not used: it faults on a 386/486, and the mix below
   (a high-resolution counter sampled around scheduler yields, plus machine
   facts) is already different on every machine and every boot. */

void v9x_install_id_generate(char *target)
{
    V9xIdPool pool;
    LARGE_INTEGER counter;
    MEMORYSTATUS memory;
    SYSTEMTIME now;
    DWORD serial = 0ul;
    DWORD component = 0ul;
    DWORD flags = 0ul;
    DWORD length;
    char name[MAX_COMPUTERNAME_LENGTH + 1];
    int round;

    v9x_id_begin(&pool);
    GetSystemTime(&now);
    v9x_id_mix(&pool, ((unsigned long)now.wYear << 16) | now.wMonth);
    v9x_id_mix(&pool, ((unsigned long)now.wDay << 16) | now.wHour);
    v9x_id_mix(&pool, ((unsigned long)now.wMinute << 16) | now.wSecond);
    v9x_id_mix(&pool, now.wMilliseconds);
    v9x_id_mix(&pool, GetTickCount());
    v9x_id_mix(&pool, GetCurrentProcessId());
    v9x_id_mix(&pool, GetCurrentThreadId());
    v9x_id_mix(&pool, (unsigned long)&pool);
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatus(&memory);
    v9x_id_mix(&pool, memory.dwAvailPhys);
    v9x_id_mix(&pool, memory.dwAvailPageFile);
    v9x_id_mix(&pool, memory.dwAvailVirtual);
    if (GetVolumeInformationA("C:\\", 0, 0ul, &serial, &component, &flags, 0, 0ul)) {
        v9x_id_mix(&pool, serial);
    }
    length = sizeof(name);
    if (GetComputerNameA(name, &length)) {
        name[sizeof(name) - 1] = '\0';
        v9x_id_mix_text(&pool, name);
    }
    for (round = 0; round < 8; ++round) {
        if (QueryPerformanceCounter(&counter)) {
            v9x_id_mix(&pool, counter.LowPart);
            v9x_id_mix(&pool, (unsigned long)counter.HighPart);
        }
        Sleep(0ul);
        v9x_id_mix(&pool, GetTickCount());
    }
    v9x_id_finish(&pool, target);
}

int v9x_install_id_ensure(const char *ini_path, char *target)
{
    char current[64];
    (void)GetPrivateProfileStringA("agent", "install_id", "", current,
                                   sizeof(current), ini_path);
    if (v9x_id_valid(current)) {
        int index;
        for (index = 0; index < V9X_INSTALL_ID_SIZE; ++index) {
            char ch = current[index];
            if (ch >= 'A' && ch <= 'F') ch = (char)(ch - 'A' + 'a');
            target[index] = ch;
        }
        return 0;
    }
    v9x_install_id_generate(target);
    return WritePrivateProfileStringA("agent", "install_id", target, ini_path)
        ? 1 : -1;
}
