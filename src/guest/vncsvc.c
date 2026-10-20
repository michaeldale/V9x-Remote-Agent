#include "vncsvc.h"

static const char v9x_service_name[] = "V9xRemoteVnc";
static const char v9x_service_display[] = "V9x Remote VNC";

/* All NT-only entry points, resolved at run time. */
typedef SERVICE_STATUS_HANDLE (WINAPI *RegCtrlHandlerFn)(const char *,
                                                         LPHANDLER_FUNCTION);
typedef BOOL (WINAPI *SetServiceStatusFn)(SERVICE_STATUS_HANDLE, LPSERVICE_STATUS);
typedef BOOL (WINAPI *StartDispatcherFn)(const SERVICE_TABLE_ENTRYA *);
typedef SC_HANDLE (WINAPI *OpenSCManagerFn)(const char *, const char *, DWORD);
typedef SC_HANDLE (WINAPI *CreateServiceFn)(SC_HANDLE, const char *, const char *,
                                            DWORD, DWORD, DWORD, DWORD, const char *,
                                            const char *, DWORD *, const char *,
                                            const char *, const char *);
typedef SC_HANDLE (WINAPI *OpenServiceFn)(SC_HANDLE, const char *, DWORD);
typedef BOOL (WINAPI *StartServiceFn)(SC_HANDLE, DWORD, const char **);
typedef BOOL (WINAPI *DeleteServiceFn)(SC_HANDLE);
typedef BOOL (WINAPI *ControlServiceFn)(SC_HANDLE, DWORD, LPSERVICE_STATUS);
typedef BOOL (WINAPI *CloseServiceHandleFn)(SC_HANDLE);
typedef HDESK (WINAPI *OpenInputDesktopFn)(DWORD, BOOL, ACCESS_MASK);
typedef BOOL (WINAPI *SetThreadDesktopFn)(HDESK);
typedef BOOL (WINAPI *CloseDesktopFn)(HDESK);
typedef BOOL (WINAPI *GetUserObjectInformationFn)(HANDLE, int, void *, DWORD, DWORD *);

static RegCtrlHandlerFn v9x_reg_handler;
static SetServiceStatusFn v9x_set_status;
static OpenInputDesktopFn v9x_open_input_desktop;
static SetThreadDesktopFn v9x_set_thread_desktop;
static CloseDesktopFn v9x_close_desktop;
static GetUserObjectInformationFn v9x_get_user_object;
static int v9x_user32_loaded;

static SERVICE_STATUS_HANDLE v9x_status_handle;
static SERVICE_STATUS v9x_status;
static DWORD v9x_exit_code;
static volatile LONG v9x_stop_flag;

int v9x_is_nt(void)
{
    return (GetVersion() & 0x80000000ul) == 0ul;
}

static void v9x_load_user32(void)
{
    HMODULE user32;
    if (v9x_user32_loaded) return;
    v9x_user32_loaded = 1;
    user32 = GetModuleHandleA("USER32.DLL");
    if (user32 == 0) return;
    v9x_open_input_desktop = (OpenInputDesktopFn)GetProcAddress(user32, "OpenInputDesktop");
    v9x_set_thread_desktop = (SetThreadDesktopFn)GetProcAddress(user32, "SetThreadDesktop");
    v9x_close_desktop = (CloseDesktopFn)GetProcAddress(user32, "CloseDesktop");
    v9x_get_user_object = (GetUserObjectInformationFn)GetProcAddress(
        user32, "GetUserObjectInformationA");
}

/* --------------------------------------------------------- desktop follow */

#define V9X_UOI_NAME 2
/* DESKTOP_ access bits enough to BitBlt the screen and inject input. */
#define V9X_DESKTOP_ACCESS 0x01fful

int vnc_follow_input_desktop(VncDesktop *state)
{
    HDESK input;
    char name[64];
    DWORD needed = 0ul;
    DWORD index;
    if (!v9x_is_nt()) return 0;
    v9x_load_user32();
    if (v9x_open_input_desktop == 0 || v9x_set_thread_desktop == 0) return 0;
    input = v9x_open_input_desktop(0ul, FALSE, V9X_DESKTOP_ACCESS);
    if (input == 0) return 0; /* a user process cannot open Winlogon: stay put */
    name[0] = '\0';
    if (v9x_get_user_object != 0) {
        (void)v9x_get_user_object((HANDLE)input, V9X_UOI_NAME, name, sizeof(name), &needed);
    }
    if (state->tried && name[0] != '\0') {
        int same = 1;
        for (index = 0ul; index < sizeof(name); ++index) {
            if (state->name[index] != name[index]) { same = 0; break; }
            if (name[index] == '\0') break;
        }
        if (same) {
            if (v9x_close_desktop != 0) (void)v9x_close_desktop(input);
            return 0;
        }
    }
    if (!v9x_set_thread_desktop(input)) {
        if (v9x_close_desktop != 0) (void)v9x_close_desktop(input);
        return 0;
    }
    if (state->desktop != 0 && v9x_close_desktop != 0) (void)v9x_close_desktop(state->desktop);
    state->desktop = input;
    for (index = 0ul; index < sizeof(name); ++index) {
        state->name[index] = name[index];
        if (name[index] == '\0') break;
    }
    state->name[sizeof(state->name) - 1ul] = '\0';
    state->tried = 1;
    return 1;
}

void vnc_desktop_release(VncDesktop *state)
{
    if (state->desktop != 0 && v9x_close_desktop != 0) {
        (void)v9x_close_desktop(state->desktop);
    }
    state->desktop = 0;
    state->tried = 0;
}

/* ---------------------------------------------------------------- service */

DWORD vnc_service_exit_code(void) { return v9x_exit_code; }
int vnc_service_stopping(void) { return v9x_stop_flag != 0l; }

/* Defined in vnc.c: the server's actual work, shared by the console and
   service paths. Returns the process exit code. */
DWORD vnc_service_main(void);

static void v9x_report(DWORD state, DWORD wait_hint)
{
    v9x_status.dwCurrentState = state;
    v9x_status.dwWaitHint = wait_hint;
    v9x_status.dwCheckPoint = state == SERVICE_START_PENDING ||
                              state == SERVICE_STOP_PENDING ? v9x_status.dwCheckPoint + 1ul : 0ul;
    if (v9x_set_status != 0 && v9x_status_handle != 0) {
        (void)v9x_set_status(v9x_status_handle, &v9x_status);
    }
}

static void WINAPI v9x_service_ctrl(DWORD control)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        v9x_stop_flag = 1l;
        v9x_report(SERVICE_STOP_PENDING, 10000ul);
    } else if (control == SERVICE_CONTROL_INTERROGATE) {
        v9x_report(v9x_status.dwCurrentState, 0ul);
    }
}

static void WINAPI v9x_service_entry(DWORD argc, char **argv)
{
    (void)argc;
    (void)argv;
    v9x_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS;
    v9x_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    v9x_status.dwWin32ExitCode = 0ul;
    v9x_status.dwServiceSpecificExitCode = 0ul;
    v9x_status.dwCheckPoint = 0ul;
    v9x_status_handle = v9x_reg_handler != 0 ?
        v9x_reg_handler(v9x_service_name, v9x_service_ctrl) : 0;
    if (v9x_status_handle == 0) return;
    v9x_report(SERVICE_START_PENDING, 10000ul);
    v9x_report(SERVICE_RUNNING, 0ul);
    v9x_exit_code = vnc_service_main();
    v9x_report(SERVICE_STOPPED, 0ul);
}

int v9x_service_run(void)
{
    HMODULE advapi;
    StartDispatcherFn dispatch;
    SERVICE_TABLE_ENTRYA table[2];
    if (!v9x_is_nt()) return 1;
    advapi = LoadLibraryA("ADVAPI32.DLL");
    if (advapi == 0) return 1;
    v9x_reg_handler = (RegCtrlHandlerFn)GetProcAddress(advapi, "RegisterServiceCtrlHandlerA");
    v9x_set_status = (SetServiceStatusFn)GetProcAddress(advapi, "SetServiceStatus");
    dispatch = (StartDispatcherFn)GetProcAddress(advapi, "StartServiceCtrlDispatcherA");
    if (v9x_reg_handler == 0 || v9x_set_status == 0 || dispatch == 0) return 1;
    table[0].lpServiceName = (char *)v9x_service_name;
    table[0].lpServiceProc = v9x_service_entry;
    table[1].lpServiceName = 0;
    table[1].lpServiceProc = 0;
    /* Fails with ERROR_FAILED_SERVICE_CONTROLLER_CONNECT (1063) when this
       was not launched by the SCM; the caller then runs on the console. */
    if (!dispatch(table)) return 1;
    return 0;
}

/* ------------------------------------------------------- install / remove */

static SC_HANDLE v9x_scm(OpenSCManagerFn open_scm, HMODULE advapi, DWORD access)
{
    (void)advapi;
    return open_scm(0, 0, access);
}

#define V9X_SC_MANAGER_ALL 0xf003ful
#define V9X_SERVICE_ALL 0xf01fful

int v9x_service_install(const char *image_path)
{
    HMODULE advapi;
    OpenSCManagerFn open_scm;
    CreateServiceFn create;
    OpenServiceFn open_service;
    StartServiceFn start;
    CloseServiceHandleFn close_handle;
    SC_HANDLE manager;
    SC_HANDLE service;
    char command[MAX_PATH + 16];
    unsigned long index = 0ul;
    if (!v9x_is_nt()) return 1;
    advapi = LoadLibraryA("ADVAPI32.DLL");
    if (advapi == 0) return 1;
    open_scm = (OpenSCManagerFn)GetProcAddress(advapi, "OpenSCManagerA");
    create = (CreateServiceFn)GetProcAddress(advapi, "CreateServiceA");
    open_service = (OpenServiceFn)GetProcAddress(advapi, "OpenServiceA");
    start = (StartServiceFn)GetProcAddress(advapi, "StartServiceA");
    close_handle = (CloseServiceHandleFn)GetProcAddress(advapi, "CloseServiceHandle");
    if (open_scm == 0 || create == 0 || close_handle == 0) return 1;
    command[index++] = '"';
    while (*image_path != '\0' && index < MAX_PATH) command[index++] = *image_path++;
    command[index++] = '"';
    command[index++] = ' ';
    command[index++] = '-';
    command[index++] = 's';
    command[index++] = 'e';
    command[index++] = 'r';
    command[index++] = 'v';
    command[index++] = 'i';
    command[index++] = 'c';
    command[index++] = 'e';
    command[index] = '\0';
    manager = v9x_scm(open_scm, advapi, V9X_SC_MANAGER_ALL);
    if (manager == 0) return 2;
    service = create(manager, v9x_service_name, v9x_service_display, V9X_SERVICE_ALL,
                     SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS,
                     SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command, 0, 0, 0,
                     0 /* LocalSystem */, 0);
    if (service == 0) {
        DWORD error = GetLastError();
        if (error == ERROR_SERVICE_EXISTS && open_service != 0) {
            service = open_service(manager, v9x_service_name, V9X_SERVICE_ALL);
        }
        if (service == 0) {
            (void)close_handle(manager);
            return error == ERROR_SERVICE_EXISTS ? 0 : 3;
        }
    }
    /* Start it now too, so enabling does not wait for a reboot. */
    if (start != 0) (void)start(service, 0ul, 0);
    (void)close_handle(service);
    (void)close_handle(manager);
    return 0;
}

#define V9X_SC_MANAGER_CONNECT 0x0001ul
#define V9X_SERVICE_CONTROL_ACCESS 0x0034ul /* QUERY_STATUS | START | STOP */

int v9x_service_control(int start)
{
    HMODULE advapi;
    OpenSCManagerFn open_scm;
    OpenServiceFn open_service;
    StartServiceFn start_service;
    ControlServiceFn control;
    CloseServiceHandleFn close_handle;
    SC_HANDLE manager;
    SC_HANDLE service;
    SERVICE_STATUS status;
    DWORD error = 0ul;
    int ok;
    if (!v9x_is_nt()) return -1;
    advapi = LoadLibraryA("ADVAPI32.DLL");
    if (advapi == 0) return -1;
    open_scm = (OpenSCManagerFn)GetProcAddress(advapi, "OpenSCManagerA");
    open_service = (OpenServiceFn)GetProcAddress(advapi, "OpenServiceA");
    start_service = (StartServiceFn)GetProcAddress(advapi, "StartServiceA");
    control = (ControlServiceFn)GetProcAddress(advapi, "ControlService");
    close_handle = (CloseServiceHandleFn)GetProcAddress(advapi, "CloseServiceHandle");
    if (open_scm == 0 || open_service == 0 || start_service == 0 || control == 0 ||
        close_handle == 0) {
        return -1;
    }
    manager = open_scm(0, 0, V9X_SC_MANAGER_CONNECT);
    if (manager == 0) return -1;
    service = open_service(manager, v9x_service_name, V9X_SERVICE_CONTROL_ACCESS);
    if (service == 0) {
        error = GetLastError();
        (void)close_handle(manager);
        return error == ERROR_SERVICE_DOES_NOT_EXIST ? -1 : 1;
    }
    if (start) {
        ok = start_service(service, 0ul, 0) != FALSE;
        if (!ok) ok = GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
    } else {
        ok = control(service, SERVICE_CONTROL_STOP, &status) != FALSE;
        if (!ok) ok = GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
    }
    (void)close_handle(service);
    (void)close_handle(manager);
    return ok ? 0 : 1;
}

int v9x_service_uninstall(void)
{
    HMODULE advapi;
    OpenSCManagerFn open_scm;
    OpenServiceFn open_service;
    DeleteServiceFn delete_service;
    ControlServiceFn control;
    CloseServiceHandleFn close_handle;
    SC_HANDLE manager;
    SC_HANDLE service;
    SERVICE_STATUS status;
    if (!v9x_is_nt()) return 1;
    advapi = LoadLibraryA("ADVAPI32.DLL");
    if (advapi == 0) return 1;
    open_scm = (OpenSCManagerFn)GetProcAddress(advapi, "OpenSCManagerA");
    open_service = (OpenServiceFn)GetProcAddress(advapi, "OpenServiceA");
    delete_service = (DeleteServiceFn)GetProcAddress(advapi, "DeleteService");
    control = (ControlServiceFn)GetProcAddress(advapi, "ControlService");
    close_handle = (CloseServiceHandleFn)GetProcAddress(advapi, "CloseServiceHandle");
    if (open_scm == 0 || open_service == 0 || delete_service == 0 || close_handle == 0) {
        return 1;
    }
    manager = v9x_scm(open_scm, advapi, V9X_SC_MANAGER_ALL);
    if (manager == 0) return 2;
    service = open_service(manager, v9x_service_name, V9X_SERVICE_ALL);
    if (service == 0) {
        (void)close_handle(manager);
        return 0; /* already gone */
    }
    if (control != 0) (void)control(service, SERVICE_CONTROL_STOP, &status);
    (void)delete_service(service);
    (void)close_handle(service);
    (void)close_handle(manager);
    return 0;
}
