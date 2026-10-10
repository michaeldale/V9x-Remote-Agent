#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "platform_win.h"

/* Shared by V9XAGNT.EXE and SETUP.EXE. The token calls are resolved at run
   time rather than imported: they only matter on NT, and resolving them
   keeps the Win9x import table exactly as it was (Windows 95's ADVAPI32 has
   only stubs for them). */

typedef BOOL (WINAPI *V9xOpenProcessTokenFn)(HANDLE, DWORD, PHANDLE);
typedef BOOL (WINAPI *V9xLookupPrivilegeValueFn)(LPCSTR, LPCSTR, PLUID);
typedef BOOL (WINAPI *V9xAdjustTokenPrivilegesFn)(HANDLE, BOOL, PTOKEN_PRIVILEGES,
                                                  DWORD, PTOKEN_PRIVILEGES, PDWORD);

static const char v9x_run_key_9x[] =
    "Software\\Microsoft\\Windows\\CurrentVersion\\RunServices";
static const char v9x_run_key_nt[] =
    "Software\\Microsoft\\Windows\\CurrentVersion\\Run";

typedef BOOL (WINAPI *V9xSetHandleInformationFn)(HANDLE, DWORD, DWORD);
/* GetPwrCapabilities fills SYSTEM_POWER_CAPABILITIES, which opens with eight
   BOOLEANs: PowerButtonPresent, SleepButtonPresent, LidPresent, then
   SystemS1..SystemS5. SystemS5 (soft-off) is byte 7. */
typedef BOOLEAN (WINAPI *V9xGetPwrCapabilitiesFn)(void *);
#define V9X_POWER_CAPS_SIZE 256
#define V9X_POWER_CAPS_S5 7

int v9x_platform_is_nt(void)
{
    return (GetVersion() & 0x80000000ul) == 0ul;
}

const char *v9x_platform_run_key(void)
{
    return v9x_platform_is_nt() ? v9x_run_key_nt : v9x_run_key_9x;
}

const char *v9x_platform_shell_prefix(void)
{
    return v9x_platform_is_nt() ? "CMD.EXE /C " : "COMMAND.COM /C ";
}

int v9x_platform_enable_shutdown(void)
{
    HMODULE advapi;
    V9xOpenProcessTokenFn open_token;
    V9xLookupPrivilegeValueFn lookup;
    V9xAdjustTokenPrivilegesFn adjust;
    HANDLE token;
    TOKEN_PRIVILEGES privileges;
    BOOL adjusted;

    if (!v9x_platform_is_nt()) return 1;
    advapi = GetModuleHandleA("ADVAPI32.DLL");
    if (advapi == 0) return 0;
    open_token = (V9xOpenProcessTokenFn)GetProcAddress(advapi, "OpenProcessToken");
    lookup = (V9xLookupPrivilegeValueFn)GetProcAddress(advapi, "LookupPrivilegeValueA");
    adjust = (V9xAdjustTokenPrivilegesFn)GetProcAddress(advapi, "AdjustTokenPrivileges");
    if (open_token == 0 || lookup == 0 || adjust == 0) return 0;
    if (!open_token(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                    &token)) {
        return 0;
    }
    privileges.PrivilegeCount = 1ul;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!lookup(0, "SeShutdownPrivilege", &privileges.Privileges[0].Luid)) {
        CloseHandle(token);
        return 0;
    }
    /* AdjustTokenPrivileges succeeds even when the token lacks the
       privilege; ERROR_NOT_ALL_ASSIGNED is the only sign of that. */
    adjusted = adjust(token, FALSE, &privileges, 0ul, 0, 0);
    adjusted = adjusted && GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return adjusted ? 1 : 0;
}

void v9x_platform_no_inherit(HANDLE handle)
{
    /* Resolved per call: SetHandleInformation is missing from Windows 95's
       KERNEL32 and only a stub on 98, and this is never on a hot path. */
    V9xSetHandleInformationFn set_info;
    if (!v9x_platform_is_nt()) return;
    set_info = (V9xSetHandleInformationFn)GetProcAddress(
        GetModuleHandleA("KERNEL32.DLL"), "SetHandleInformation");
    if (set_info != 0) (void)set_info(handle, HANDLE_FLAG_INHERIT, 0ul);
}

int v9x_platform_can_power_off(void)
{
    HMODULE powrprof;
    V9xGetPwrCapabilitiesFn get_caps;
    unsigned char caps[V9X_POWER_CAPS_SIZE];
    int index;
    int can = 1;
    if (!v9x_platform_is_nt()) return 1;
    /* Without POWRPROF the answer is unknown: keep EWX_POWEROFF. */
    powrprof = LoadLibraryA("POWRPROF.DLL");
    if (powrprof == 0) return 1;
    get_caps = (V9xGetPwrCapabilitiesFn)GetProcAddress(powrprof, "GetPwrCapabilities");
    for (index = 0; index < V9X_POWER_CAPS_SIZE; ++index) caps[index] = 0u;
    if (get_caps != 0 && get_caps(caps)) can = caps[V9X_POWER_CAPS_S5] != 0u;
    (void)FreeLibrary(powrprof);
    return can;
}
