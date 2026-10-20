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

/* AGENT.INI is a few hundred bytes; one damaged by the duplicate install_id
   bug grows about 50 bytes per boot. Anything larger is left alone. The
   buffers are allocated, not static: Open Watcom stores BSS in the image. */
#define V9X_INI_MAX 32768ul

static int v9x_ini_rewrite(const char *ini_path, char *in, char *out,
                           unsigned long capacity)
{
    char backup[MAX_PATH];
    HANDLE file;
    DWORD size;
    DWORD done;
    unsigned long length;
    unsigned long index;
    unsigned long dot = 0ul;
    int ok;

    /* Commit anything the profile cache still holds before reading the raw
       file. On Windows 98 this returns FALSE even when it works. */
    (void)WritePrivateProfileStringA(0, 0, 0, ini_path);
    file = CreateFileA(ini_path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(file, 0);
    ok = size != 0xfffffffful && size <= V9X_INI_MAX &&
         ReadFile(file, in, size, &done, 0) && done == size;
    (void)CloseHandle(file);
    if (!ok) return -1;
    if (!v9x_ini_repair(in, size, out, capacity, "install_id", &length)) return -1;
    if (length == size) {
        for (index = 0ul; index < length && in[index] == out[index]; ++index) {
        }
        if (index == length) return 0;
    }

    /* The original goes to AGENT.BAK beside it first. */
    for (index = 0ul; ini_path[index] != '\0' && index + 5ul < sizeof(backup); ++index) {
        backup[index] = ini_path[index];
        if (ini_path[index] == '.') dot = index;
        if (ini_path[index] == '\\') dot = 0ul;
    }
    if (dot == 0ul) dot = index;
    backup[dot] = '.';
    backup[dot + 1ul] = 'B';
    backup[dot + 2ul] = 'A';
    backup[dot + 3ul] = 'K';
    backup[dot + 4ul] = '\0';
    (void)SetFileAttributesA(backup, FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileA(ini_path, backup, FALSE)) return -1;

    file = CreateFileA(ini_path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return -1;
    ok = WriteFile(file, out, length, &done, 0) && done == length;
    if (!CloseHandle(file)) ok = 0;
    return ok ? 1 : -1;
}

int v9x_ini_normalize(const char *ini_path)
{
    HGLOBAL allocation = GlobalAlloc(GMEM_FIXED, V9X_INI_MAX * 3ul + 2ul);
    int result;
    if (allocation == 0) return -1;
    result = v9x_ini_rewrite(ini_path, (char *)allocation,
                             (char *)allocation + V9X_INI_MAX, V9X_INI_MAX * 2ul + 2ul);
    (void)GlobalFree(allocation);
    return result;
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
