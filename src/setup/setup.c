/* SETUP.EXE: the V9x Remote Agent install wizard.

   Welcome -> Options -> Install -> Finish, in one plain window whose pages
   are groups of controls shown and hidden together. Built like the agent:
   C89, Open Watcom, no C runtime and no resource compiler (the build has no
   resource step; see docs\decisions\2026-09-24-tray-activity-icon-without-
   gdi.md), so every control is created in code.

   It does what INSTALL.BAT does, plus writing the chosen [agent] settings:
   a fresh install copies the binaries and registers RunServices; over an
   existing install it stages V9XNEW.EXE/V9XSNEW.EXE and points RunServices
   at UPDATE.BAT, exactly like INSTALL.BAT's update path. On Windows 2000
   (NT) RunServices is ignored, so it registers the Run key instead and
   stages an update by pointing Run at "V9XNEW.EXE -hotswap", the agent's own
   swapper (update.c); UPDATE.BAT's REGEDIT/RunServices steps are 9x-only. Run from
   C:\V9XREMOTE itself (Add/Remove Programs, or to change settings) it only
   rewrites AGENT.INI. The install ID is kept once it exists.

     SETUP.EXE             the wizard
     SETUP.EXE /S          silent install with current or default settings
     SETUP.EXE /vnc        tick the VNC option (/novnc unticks it); with /S too
     SETUP.EXE /uninstall  remove autostart and the Add/Remove Programs entry
     SETUP.EXE /settings   the tray icon's Settings: opens on the options page
                           (installed copy only); exit 0 means saved, and the
                           agent then restarts itself to apply them
     SETUP.EXE /remote     the tray icon's Remote help: the opt-in page for
                           lending this machine to the developer (installed
                           copy only); exit 0 means saved, as /settings
     SETUP.EXE /update     check for, verify and stage a newer version
                           (selfupdate.c)

   VNC remote control (V9XVNC.EXE) is off unless the VNC option is ticked:
   then [vnc] enabled=1 is written and the server starts from Run at each
   logon, and now; unticked, or on uninstall, both are undone. The optional
   VNC password (at most 8 characters) goes to [vnc] password.

   Exit code: 0 success, 1 failed, 2 cancelled. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "v9xremote/installid.h"
#include "v9xremote/version.h"
#include "../guest/installid_win.h"
#include "../guest/platform_win.h"

#define V9X_SETUP_WIDTH 480
#define V9X_SETUP_HEIGHT 424
#define V9X_SETUP_MAX_CONTROLS 64

#define V9X_PAGE_WELCOME 0
#define V9X_PAGE_OPTIONS 1
#define V9X_PAGE_INSTALL 2
#define V9X_PAGE_FINISH 3
#define V9X_PAGE_REMOTE 4
#define V9X_PAGE_ALL -1

#define V9X_ID_BACK 100
#define V9X_ID_NEXT 101
#define V9X_ID_CANCEL 102
#define V9X_ID_DISCOVERY 110
#define V9X_ID_DOS 111
#define V9X_ID_PORT 112
#define V9X_ID_BIND 113
#define V9X_ID_CLIENT 114
#define V9X_ID_PROGRESS 115
#define V9X_ID_REBOOT 116
#define V9X_ID_VNC 117
#define V9X_ID_VNCPASS 118
#define V9X_ID_REMOTE_OFF 120
#define V9X_ID_REMOTE_ON 121
#define V9X_ID_REMOTE_NAME 122
#define V9X_ID_REMOTE_GITHUB 123
#define V9X_ID_REMOTE_EMAIL 124
#define V9X_ID_REMOTE_STATUS 126
#define V9X_ID_UPDATE_STARTUP 127

int v9x_self_update(int child);

static const char v9x_root[] = "C:\\V9XREMOTE";
static const char v9x_ini[] = "C:\\V9XREMOTE\\AGENT.INI";
static const char v9x_agent[] = "C:\\V9XREMOTE\\V9XAGNT.EXE";
static const char v9x_run_value[] = "V9xRemoteAgent";
static const char v9x_run_fresh[] = "C:\\V9XREMOTE\\V9XAGNT.EXE -service";
static const char v9x_run_update[] = "C:\\V9XREMOTE\\UPDATE.BAT";
static const char v9x_run_update_nt[] = "C:\\V9XREMOTE\\V9XNEW.EXE -hotswap";
/* Both autostart keys: uninstall clears either, whichever this is. */
static const char v9x_run_key_9x[] =
    "Software\\Microsoft\\Windows\\CurrentVersion\\RunServices";
static const char v9x_run_key_nt[] =
    "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
/* V9XVNC.EXE needs a logged-on desktop, so it starts from Run on both
   Windows 9x and 2000, never RunServices. */
static const char v9x_vnc_run_value[] = "V9xRemoteVnc";
static const char v9x_vnc_exe[] = "C:\\V9XREMOTE\\V9XVNC.EXE";
static const char v9x_arp_key[] =
    "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\V9xRemoteAgent";
static const char v9x_setup_title[] = "V9x Remote Agent " V9X_AGENT_VERSION " Setup";
static const char v9x_settings_title[] =
    "V9x Remote Agent " V9X_AGENT_VERSION " Settings";
static const char v9x_remote_title[] =
    "V9x Remote Agent " V9X_AGENT_VERSION " - Remote help";
static const char v9x_status_ini[] = "C:\\V9XREMOTE\\STATUS.INI";
static const char *v9x_title = v9x_setup_title;

/* Support files copied as-is; AGENT.INI is copied only when none exists. */
static const char *const v9x_support_files[] = {
    "INSTALL.REG", "REMOVE.REG", "UPDATE.REG", "UPDATE.BAT", "UNINSTALL.BAT",
    "README.TXT", "V9XVNC.EXE"
};

typedef struct V9xSettings {
    int discovery;
    int allow_dos_vm;
    int vnc;
    char port[8];
    char bind_address[16];
    char allowed_client[16];
    char vnc_password[9];
    char install_id[V9X_INSTALL_ID_SIZE];
    /* [remote], the /remote page */
    int remote_on;
    int update_on_startup;
    char remote_name[64];
    char remote_github[40];
    char remote_email[96];
    char remote_machine[16];
} V9xSettings;

typedef struct V9xControl {
    HWND window;
    int page;
} V9xControl;

static HINSTANCE v9x_instance;
static HWND v9x_main;
static HFONT v9x_font;
static V9xControl v9x_controls[V9X_SETUP_MAX_CONTROLS];
static int v9x_control_count;
static int v9x_page;
static int v9x_existing;       /* V9XAGNT.EXE already in C:\V9XREMOTE */
static int v9x_settings_only;  /* running from C:\V9XREMOTE: no files to copy */
static int v9x_from_tray;      /* /settings: the agent restarts when this saves */
static int v9x_remote_page;    /* /remote: only the remote help page */
static int v9x_install_ok;
static int v9x_busy;           /* install steps running: the window cannot close */
static int v9x_exit_code = 2;
static char v9x_source[MAX_PATH];
static V9xSettings v9x_settings;

/* ---- small runtime-free string helpers ---- */

static unsigned long v9x_len(const char *text)
{
    unsigned long length = 0ul;
    while (text[length] != '\0') ++length;
    return length;
}

static void v9x_copy(char *target, const char *source, unsigned long capacity)
{
    unsigned long at = 0ul;
    while (source[at] != '\0' && at + 1ul < capacity) {
        target[at] = source[at];
        ++at;
    }
    target[at] = '\0';
}

static void v9x_append(char *target, const char *source, unsigned long capacity)
{
    unsigned long at = v9x_len(target);
    while (*source != '\0' && at + 1ul < capacity) target[at++] = *source++;
    target[at] = '\0';
}

static void v9x_path(char *target, const char *directory, const char *name)
{
    v9x_copy(target, directory, MAX_PATH);
    if (target[0] != '\0' && target[v9x_len(target) - 1ul] != '\\') {
        v9x_append(target, "\\", MAX_PATH);
    }
    v9x_append(target, name, MAX_PATH);
}

static int v9x_exists(const char *path)
{
    return GetFileAttributesA(path) != 0xfffffffful;
}

/* 1 when text is a dotted-quad IPv4 address with every part 0-255. */
static int v9x_valid_ipv4(const char *text)
{
    int parts = 0;
    for (;;) {
        unsigned long value = 0ul;
        int digits = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10ul + (unsigned long)(*text - '0');
            if (++digits > 3) return 0;
            ++text;
        }
        if (digits == 0 || value > 255ul) return 0;
        ++parts;
        if (*text == '\0') return parts == 4;
        if (*text != '.' || parts == 4) return 0;
        ++text;
    }
}

static int v9x_parse_port(const char *text, unsigned long *port)
{
    unsigned long value = 0ul;
    int digits = 0;
    while (*text == ' ') ++text;
    while (*text >= '0' && *text <= '9') {
        value = value * 10ul + (unsigned long)(*text - '0');
        if (++digits > 5) return 0;
        ++text;
    }
    while (*text == ' ') ++text;
    if (digits == 0 || *text != '\0' || value == 0ul || value > 65535ul) return 0;
    *port = value;
    return 1;
}

static void v9x_decimal(char *target, unsigned long value)
{
    char digits[10];
    int count = 0;
    int at = 0;
    do {
        digits[count++] = (char)('0' + (value % 10ul));
        value /= 10ul;
    } while (value != 0ul);
    while (count != 0) target[at++] = digits[--count];
    target[at] = '\0';
}

/* ---- settings ---- */

/* Current settings from C:\V9XREMOTE\AGENT.INI where there is one, defaults
   otherwise. Discovery is ticked unless the INI already says discovery=0:
   the checkbox is how a user opts in, and it is in plain view. */
static void v9x_load_settings(void)
{
    UINT port;
    /* Same repair as the agent's, so an LF-only AGENT.INI does not get the
       new keys written in a form Windows 9x cannot read back. */
    (void)v9x_ini_normalize(v9x_ini);
    port = GetPrivateProfileIntA("agent", "port", 9869, v9x_ini);
    if (port == 0u || port > 65535u) port = 9869u;
    v9x_decimal(v9x_settings.port, port);
    v9x_settings.discovery = GetPrivateProfileIntA("agent", "discovery", 1, v9x_ini) != 0;
    v9x_settings.allow_dos_vm =
        GetPrivateProfileIntA("agent", "allow_dos_vm", 1, v9x_ini) != 0;
    /* VNC is opt-in: unticked unless this machine already turned it on. */
    v9x_settings.vnc = GetPrivateProfileIntA("vnc", "enabled", 0, v9x_ini) == 1;
    (void)GetPrivateProfileStringA("vnc", "password", "", v9x_settings.vnc_password,
                                   sizeof(v9x_settings.vnc_password), v9x_ini);
    (void)GetPrivateProfileStringA("agent", "bind_address", "0.0.0.0",
                                   v9x_settings.bind_address,
                                   sizeof(v9x_settings.bind_address), v9x_ini);
    (void)GetPrivateProfileStringA("agent", "allowed_client", "",
                                   v9x_settings.allowed_client,
                                   sizeof(v9x_settings.allowed_client), v9x_ini);
    {
        char mode[16];
        (void)GetPrivateProfileStringA("remote", "mode", "off", mode, sizeof(mode), v9x_ini);
        v9x_settings.remote_on = lstrcmpiA(mode, "on") == 0;
    }
    v9x_settings.update_on_startup =
        GetPrivateProfileIntA("update", "check_on_startup", 0, v9x_ini) != 0;
    (void)GetPrivateProfileStringA("remote", "display_name", "", v9x_settings.remote_name,
                                   sizeof(v9x_settings.remote_name), v9x_ini);
    (void)GetPrivateProfileStringA("remote", "github", "", v9x_settings.remote_github,
                                   sizeof(v9x_settings.remote_github), v9x_ini);
    (void)GetPrivateProfileStringA("remote", "email", "", v9x_settings.remote_email,
                                   sizeof(v9x_settings.remote_email), v9x_ini);
    (void)GetPrivateProfileStringA("remote", "machine_id", "", v9x_settings.remote_machine,
                                   sizeof(v9x_settings.remote_machine), v9x_ini);
    {
        char current[64];
        (void)GetPrivateProfileStringA("agent", "install_id", "", current,
                                       sizeof(current), v9x_ini);
        if (v9x_id_valid(current)) {
            v9x_copy(v9x_settings.install_id, current, sizeof(v9x_settings.install_id));
        } else {
            v9x_install_id_generate(v9x_settings.install_id);
        }
    }
}

static int v9x_write_settings(void)
{
    char port_text[8];
    unsigned long port;
    int ok;
    if (!v9x_parse_port(v9x_settings.port, &port)) return 0;
    v9x_decimal(port_text, port);
    ok = WritePrivateProfileStringA("agent", "port", port_text, v9x_ini) &&
           WritePrivateProfileStringA("agent", "bind_address",
                                      v9x_settings.bind_address, v9x_ini) &&
           WritePrivateProfileStringA("agent", "allowed_client",
                                      v9x_settings.allowed_client, v9x_ini) &&
           WritePrivateProfileStringA("agent", "allow_dos_vm",
                                      v9x_settings.allow_dos_vm ? "1" : "0", v9x_ini) &&
           WritePrivateProfileStringA("agent", "discovery",
                                      v9x_settings.discovery ? "1" : "0", v9x_ini) &&
           WritePrivateProfileStringA("agent", "install_id",
                                      v9x_settings.install_id, v9x_ini) &&
           WritePrivateProfileStringA("vnc", "enabled",
                                      v9x_settings.vnc ? "1" : "0", v9x_ini) &&
           WritePrivateProfileStringA("vnc", "password",
                                      v9x_settings.vnc_password, v9x_ini);
    /* Flush the profile cache so the file is on disk before a restart. On
       Windows 98 this call returns FALSE even when it works, so its result
       is not a failure. */
    (void)WritePrivateProfileStringA(0, 0, 0, v9x_ini);
    return ok;
}

/* The /remote page's settings. */
static int v9x_write_remote(void)
{
    int ok = WritePrivateProfileStringA("remote", "mode",
                                        v9x_settings.remote_on ? "on" : "off", v9x_ini) &&
             WritePrivateProfileStringA("remote", "display_name",
                                        v9x_settings.remote_name, v9x_ini) &&
             WritePrivateProfileStringA("remote", "github",
                                        v9x_settings.remote_github, v9x_ini) &&
             WritePrivateProfileStringA("remote", "email",
                                        v9x_settings.remote_email, v9x_ini) &&
             WritePrivateProfileStringA("update", "check_on_startup",
                                        v9x_settings.update_on_startup ? "1" : "0", v9x_ini);
    (void)WritePrivateProfileStringA(0, 0, 0, v9x_ini);
    return ok;
}

/* ---- install steps ---- */

static void v9x_step(const char *text, int ok)
{
    char line[160];
    HWND list;
    v9x_copy(line, ok ? "OK      " : "FAILED  ", sizeof(line));
    v9x_append(line, text, sizeof(line));
    if (v9x_main == 0) return;
    list = GetDlgItem(v9x_main, V9X_ID_PROGRESS);
    (void)SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)line);
    UpdateWindow(list);
}

static int v9x_copy_file(const char *name, const char *target_name)
{
    char source[MAX_PATH];
    char target[MAX_PATH];
    char label[80];
    int ok;
    v9x_path(source, v9x_source, name);
    v9x_path(target, v9x_root, target_name);
    (void)SetFileAttributesA(target, FILE_ATTRIBUTE_NORMAL);
    ok = CopyFileA(source, target, FALSE) != FALSE;
    /* Files copied off a CD arrive read-only; the agent must be able to
       replace them (hot update, UPDATE.BAT). */
    if (ok) (void)SetFileAttributesA(target, FILE_ATTRIBUTE_NORMAL);
    v9x_copy(label, "Copy ", sizeof(label));
    v9x_append(label, target_name, sizeof(label));
    v9x_step(label, ok);
    return ok;
}

static void v9x_remove(const char *name)
{
    char target[MAX_PATH];
    v9x_path(target, v9x_root, name);
    (void)SetFileAttributesA(target, FILE_ATTRIBUTE_NORMAL);
    (void)DeleteFileA(target);
}

static int v9x_run_vnc_at(const char *exe, const char *switches);

/* A running V9XVNC.EXE holds its image open, so the copy over it would fail:
   ask it to exit and give it a moment. On Windows 9x that is its -stop
   event. The Windows 2000 service's event is LocalSystem's and cannot be
   opened from here, so on NT run "V9XVNC.EXE -stop", which stops the service
   through the SCM: the package's copy when there is one, because a
   V9XVNC.EXE from before 0.11.0's fix only tries the event. */
static void v9x_stop_vnc(void)
{
    HANDLE stop = OpenEventA(EVENT_MODIFY_STATE, FALSE, "V9XVNC.STOP");
    if (stop != 0) {
        (void)SetEvent(stop);
        CloseHandle(stop);
        Sleep(1500ul);
        return;
    }
    if (v9x_platform_is_nt()) {
        char exe[MAX_PATH];
        exe[0] = '\0';
        if (v9x_source[0] != '\0') v9x_path(exe, v9x_source, "V9XVNC.EXE");
        if (exe[0] == '\0' || !v9x_exists(exe)) v9x_copy(exe, v9x_vnc_exe, sizeof(exe));
        if (v9x_exists(exe) && v9x_run_vnc_at(exe, "-stop")) Sleep(1500ul);
    }
}

static int v9x_set_string(HKEY root, const char *path, const char *name,
                          const char *value)
{
    HKEY key;
    DWORD disposition;
    LONG status;
    if (RegCreateKeyExA(root, path, 0ul, 0, REG_OPTION_NON_VOLATILE, KEY_WRITE, 0,
                        &key, &disposition) != ERROR_SUCCESS) {
        return 0;
    }
    status = RegSetValueExA(key, name, 0ul, REG_SZ, (const BYTE *)value,
                            v9x_len(value) + 1ul);
    (void)RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

static void v9x_delete_vnc_autostart(void)
{
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, v9x_run_key_nt, 0ul, KEY_WRITE, &key) ==
        ERROR_SUCCESS) {
        (void)RegDeleteValueA(key, v9x_vnc_run_value);
        (void)RegCloseKey(key);
    }
}

/* Run V9XVNC.EXE with one switch and wait for it (for -install / -uninstall,
   which are quick). Returns 1 when it exited with code 0. */
static int v9x_run_vnc_at(const char *exe, const char *switches)
{
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    char command[MAX_PATH + 32];
    unsigned long index = 0ul;
    const char *source = exe;
    DWORD code = 1ul;
    command[index++] = '"';
    while (*source != '\0' && index < MAX_PATH) command[index++] = *source++;
    command[index++] = '"';
    command[index++] = ' ';
    while (*switches != '\0' && index < sizeof(command) - 1ul) command[index++] = *switches++;
    command[index] = '\0';
    GetStartupInfoA(&startup);
    if (!CreateProcessA(exe, command, 0, 0, FALSE, 0ul, 0, v9x_root,
                        &startup, &process)) {
        return 0;
    }
    (void)WaitForSingleObject(process.hProcess, 20000ul);
    (void)GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code == 0ul;
}

static int v9x_run_vnc(const char *switches)
{
    return v9x_run_vnc_at(v9x_vnc_exe, switches);
}

/* The VNC option. On Windows 2000 V9XVNC.EXE runs as a SYSTEM service, the
   only way to reach the Winlogon (logon and lock) desktop; on Windows 9x it
   is a plain program started from the RunServices-era Run key. On, it is
   installed and started now; off, removed and stopped. */
static int v9x_apply_vnc(void)
{
    int ok;
    v9x_stop_vnc();
    if (v9x_platform_is_nt()) {
        /* A VNC build before 0.11 service support may have left a Run value. */
        v9x_delete_vnc_autostart();
        if (!v9x_settings.vnc) {
            v9x_step("VNC remote control is off", v9x_run_vnc("-uninstall"));
            return 1;
        }
        if (!v9x_exists(v9x_vnc_exe)) { v9x_step("V9XVNC.EXE is missing", 0); return 0; }
        ok = v9x_run_vnc("-install");
        v9x_step("Install and start the VNC service", ok);
        return ok;
    }
    if (!v9x_settings.vnc) {
        v9x_delete_vnc_autostart();
        v9x_step("VNC remote control is off", 1);
        return 1;
    }
    ok = v9x_exists(v9x_vnc_exe) &&
         v9x_set_string(HKEY_LOCAL_MACHINE, v9x_run_key_nt, v9x_vnc_run_value,
                        v9x_vnc_exe);
    v9x_step("Start VNC remote control at logon (Run)", ok);
    if (!ok) return 0;
    /* CreateProcess, not WinExec: on Windows 9x WinExec waits for the new
       process to read its message queue, which V9XVNC.EXE never does, so
       setup stalled about 30 s here. */
    {
        STARTUPINFOA startup;
        PROCESS_INFORMATION process;
        char command[MAX_PATH];
        v9x_copy(command, v9x_vnc_exe, sizeof(command));
        GetStartupInfoA(&startup);
        ok = CreateProcessA(v9x_vnc_exe, command, 0, 0, FALSE, 0ul, 0, v9x_root,
                            &startup, &process) != FALSE;
        if (ok) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
    }
    v9x_step("Start VNC remote control now", ok);
    return 1;
}

static int v9x_register_arp(void)
{
    char uninstall[MAX_PATH];
    v9x_path(uninstall, v9x_root, "SETUP.EXE /uninstall");
    return v9x_set_string(HKEY_LOCAL_MACHINE, v9x_arp_key, "DisplayName",
                          "V9x Remote Agent") &&
           v9x_set_string(HKEY_LOCAL_MACHINE, v9x_arp_key, "DisplayVersion",
                          V9X_AGENT_VERSION) &&
           v9x_set_string(HKEY_LOCAL_MACHINE, v9x_arp_key, "UninstallString",
                          uninstall);
}

static int v9x_install(void)
{
    unsigned int index;
    int ok;
    char self[MAX_PATH];
    char installed_setup[MAX_PATH];

    ok = CreateDirectoryA(v9x_root, 0) || v9x_exists(v9x_root);
    v9x_step("Create C:\\V9XREMOTE", ok);
    if (!ok) return 0;

    if (!v9x_settings_only) {
        char template_ini[MAX_PATH];
        v9x_path(template_ini, v9x_source, "AGENT.INI");
        if (!v9x_exists(v9x_ini) && v9x_exists(template_ini) &&
            !v9x_copy_file("AGENT.INI", "AGENT.INI")) {
            return 0;
        }
        v9x_stop_vnc();
        for (index = 0u; index < sizeof(v9x_support_files) / sizeof(v9x_support_files[0]);
             ++index) {
            if (!v9x_copy_file(v9x_support_files[index], v9x_support_files[index])) {
                return 0;
            }
        }
    }

    ok = v9x_write_settings();
    v9x_step("Write settings to AGENT.INI", ok);
    if (!ok) return 0;
    if (!v9x_apply_vnc()) return 0;

    if (v9x_from_tray) {
        v9x_step("The agent restarts to apply the settings", 1);
    } else if (v9x_settings_only) {
        v9x_step("Settings take effect when the agent next starts", 1);
    } else if (v9x_existing) {
        /* Same as INSTALL.BAT's update path: stage beside the running agent
           and let UPDATE.BAT swap them in at the next boot. */
        v9x_remove("V9XNEW.EXE");
        v9x_remove("V9XSNEW.EXE");
        if (!v9x_copy_file("V9XSHOT.EXE", "V9XSNEW.EXE") ||
            !v9x_copy_file("V9XAGNT.EXE", "V9XNEW.EXE")) {
            v9x_remove("V9XNEW.EXE");
            v9x_remove("V9XSNEW.EXE");
            return 0;
        }
        if (v9x_platform_is_nt()) {
            ok = v9x_set_string(HKEY_LOCAL_MACHINE, v9x_platform_run_key(),
                                v9x_run_value, v9x_run_update_nt);
            v9x_step("Stage the update for the next logon (Run)", ok);
        } else {
            ok = v9x_set_string(HKEY_LOCAL_MACHINE, v9x_platform_run_key(),
                                v9x_run_value, v9x_run_update);
            v9x_step("Stage the update for the next boot (UPDATE.BAT)", ok);
        }
        if (!ok) return 0;
    } else {
        if (!v9x_copy_file("V9XSHOT.EXE", "V9XSHOT.EXE") ||
            !v9x_copy_file("V9XAGNT.EXE", "V9XAGNT.EXE")) {
            v9x_remove("V9XAGNT.EXE");
            v9x_remove("V9XSHOT.EXE");
            return 0;
        }
        ok = v9x_set_string(HKEY_LOCAL_MACHINE, v9x_platform_run_key(),
                            v9x_run_value, v9x_run_fresh);
        v9x_step(v9x_platform_is_nt() ? "Start the agent at logon (Run)"
                                      : "Start the agent at boot (RunServices)", ok);
        if (!ok) return 0;
    }

    /* Keep a copy of setup itself for Add/Remove Programs and for changing
       settings later. Skipped when this already is that copy. */
    if (GetModuleFileNameA(0, self, sizeof(self)) != 0ul) {
        v9x_path(installed_setup, v9x_root, "SETUP.EXE");
        if (lstrcmpiA(self, installed_setup) != 0) {
            (void)SetFileAttributesA(installed_setup, FILE_ATTRIBUTE_NORMAL);
            ok = CopyFileA(self, installed_setup, FALSE) != FALSE;
            if (ok) (void)SetFileAttributesA(installed_setup, FILE_ATTRIBUTE_NORMAL);
            v9x_step("Copy SETUP.EXE", ok);
        }
    }
    ok = v9x_register_arp();
    v9x_step("Register in Add/Remove Programs", ok);
    return 1;
}

static int v9x_uninstall(int quiet)
{
    HKEY key;
    int removed = 0;
    if (!quiet &&
        MessageBoxA(0, "Stop the V9x Remote Agent from starting with Windows?",
                    "V9x Remote Agent", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return 2;
    }
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, v9x_run_key_9x, 0ul, KEY_WRITE, &key) ==
        ERROR_SUCCESS) {
        removed = RegDeleteValueA(key, v9x_run_value) == ERROR_SUCCESS;
        (void)RegCloseKey(key);
    }
    if (v9x_platform_is_nt() &&
        RegOpenKeyExA(HKEY_LOCAL_MACHINE, v9x_run_key_nt, 0ul, KEY_WRITE, &key) ==
        ERROR_SUCCESS) {
        removed |= RegDeleteValueA(key, v9x_run_value) == ERROR_SUCCESS;
        (void)RegCloseKey(key);
    }
    (void)RegDeleteKeyA(HKEY_LOCAL_MACHINE, v9x_arp_key);
    v9x_delete_vnc_autostart();
    v9x_stop_vnc();
    /* Remove the VNC service too (NT); a no-op if it was never installed. */
    if (v9x_platform_is_nt() && v9x_exists(v9x_vnc_exe)) (void)v9x_run_vnc("-uninstall");
    v9x_remove("V9XNEW.EXE");
    v9x_remove("V9XSNEW.EXE");
    if (!quiet) {
        MessageBoxA(0,
                    "The agent will not start again after Windows restarts.\r\n\r\n"
                    "Restart Windows, then delete the C:\\V9XREMOTE folder to "
                    "remove its files.",
                    "V9x Remote Agent", MB_OK | MB_ICONINFORMATION);
    }
    (void)removed;
    return 0;
}

/* ---- window ---- */

static HWND v9x_add(int page, DWORD ex_style, const char *klass, const char *text,
                    DWORD style, int x, int y, int width, int height, int id)
{
    HWND window = CreateWindowExA(ex_style, klass, text, WS_CHILD | style, x, y,
                                  width, height, v9x_main, (HMENU)id, v9x_instance, 0);
    if (window != 0) {
        if (v9x_font != 0) (void)SendMessageA(window, WM_SETFONT, (WPARAM)v9x_font, 0);
        if (v9x_control_count < V9X_SETUP_MAX_CONTROLS) {
            v9x_controls[v9x_control_count].window = window;
            v9x_controls[v9x_control_count].page = page;
            ++v9x_control_count;
        }
    }
    return window;
}

static void v9x_text(int page, const char *text, int x, int y, int width, int height)
{
    (void)v9x_add(page, 0ul, "STATIC", text, SS_LEFT, x, y, width, height, -1);
}

static void v9x_build(void)
{
    char id_line[64];
    const int w = V9X_SETUP_WIDTH;

    v9x_add(V9X_PAGE_ALL, 0ul, "STATIC", v9x_title, SS_LEFT, 16, 12, w - 32, 20, -1);
    v9x_add(V9X_PAGE_ALL, 0ul, "STATIC", "", SS_ETCHEDHORZ, 16, 36, w - 32, 2, -1);

    if (v9x_settings_only) {
        v9x_text(V9X_PAGE_WELCOME,
                 "The agent is installed in C:\\V9XREMOTE. This changes its "
                 "settings; they take effect the next time the agent starts.",
                 24, 52, w - 48, 44);
    } else if (v9x_existing) {
        v9x_text(V9X_PAGE_WELCOME,
                 "An existing installation was found in C:\\V9XREMOTE. Setup keeps "
                 "its install ID, lets you review its settings, and stages this "
                 "version to replace it at the next restart.",
                 24, 52, w - 48, 60);
    } else {
        v9x_text(V9X_PAGE_WELCOME,
                 "This installs the V9x Remote Agent into C:\\V9XREMOTE and starts "
                 "it every time Windows starts, before anyone logs on.",
                 24, 52, w - 48, 44);
    }
    v9x_text(V9X_PAGE_WELCOME,
             "The agent is not authenticated: anyone who can reach its TCP port "
             "can run programs and read or write files on this machine. Use it "
             "only on an isolated or trusted network.",
             24, 124, w - 48, 60);
    v9x_text(V9X_PAGE_WELCOME, "Click Next to choose its settings.", 24, 200, w - 48, 20);

    v9x_add(V9X_PAGE_OPTIONS, 0ul, "BUTTON",
            "Enable LAN discovery (mDNS) so hosts on the network can find it",
            BS_AUTOCHECKBOX | WS_TABSTOP, 24, 48, w - 48, 20, V9X_ID_DISCOVERY);
    v9x_text(V9X_PAGE_OPTIONS,
             "Hosts find it with \"v9xctl discover\". Unticked, the agent stays "
             "silent on the network.", 44, 68, w - 68, 30);
    v9x_add(V9X_PAGE_OPTIONS, 0ul, "BUTTON",
            v9x_platform_is_nt() ?
                "Allow DOS and 16-bit Windows programs (NTVDM)" :
                "Allow DOS programs (COMMAND.COM and console applications)",
            BS_AUTOCHECKBOX | WS_TABSTOP, 24, 102, w - 48, 20, V9X_ID_DOS);
    v9x_text(V9X_PAGE_OPTIONS,
             "Untick it if starting a DOS box corrupts this machine's display.",
             44, 122, w - 68, 18);
    v9x_text(V9X_PAGE_OPTIONS, "TCP port:", 24, 154, 120, 18);
    v9x_add(V9X_PAGE_OPTIONS, WS_EX_CLIENTEDGE, "EDIT", v9x_settings.port,
            ES_AUTOHSCROLL | WS_TABSTOP, 150, 151, 70, 22, V9X_ID_PORT);
    v9x_text(V9X_PAGE_OPTIONS, "Listen on address:", 24, 184, 120, 18);
    v9x_add(V9X_PAGE_OPTIONS, WS_EX_CLIENTEDGE, "EDIT", v9x_settings.bind_address,
            ES_AUTOHSCROLL | WS_TABSTOP, 150, 181, 130, 22, V9X_ID_BIND);
    v9x_text(V9X_PAGE_OPTIONS, "(0.0.0.0 = every address)", 290, 184, w - 306, 18);
    v9x_text(V9X_PAGE_OPTIONS, "Only accept client:", 24, 214, 120, 18);
    v9x_add(V9X_PAGE_OPTIONS, WS_EX_CLIENTEDGE, "EDIT", v9x_settings.allowed_client,
            ES_AUTOHSCROLL | WS_TABSTOP, 150, 211, 130, 22, V9X_ID_CLIENT);
    v9x_text(V9X_PAGE_OPTIONS, "(blank = any host)", 290, 214, w - 306, 18);
    v9x_add(V9X_PAGE_OPTIONS, 0ul, "BUTTON",
            "Enable VNC remote control (screen, keyboard and mouse)",
            BS_AUTOCHECKBOX | WS_TABSTOP, 24, 244, w - 48, 20, V9X_ID_VNC);
    v9x_text(V9X_PAGE_OPTIONS,
             "Any VNC viewer on TCP port 5900, from logon on.", 44, 264, w - 68, 18);
    v9x_text(V9X_PAGE_OPTIONS, "VNC password:", 44, 289, 100, 18);
    v9x_add(V9X_PAGE_OPTIONS, WS_EX_CLIENTEDGE, "EDIT", v9x_settings.vnc_password,
            ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, 150, 286, 100, 22, V9X_ID_VNCPASS);
    v9x_text(V9X_PAGE_OPTIONS, "(optional, up to 8 characters)", 260, 289, w - 276, 18);
    v9x_copy(id_line, "Install ID:  ", sizeof(id_line));
    v9x_append(id_line, v9x_settings.install_id, sizeof(id_line));
    v9x_text(V9X_PAGE_OPTIONS, id_line, 24, 326, w - 48, 18);
    v9x_text(V9X_PAGE_OPTIONS,
             "Identifies this machine even when its IP address changes.",
             24, 344, w - 48, 18);

    v9x_text(V9X_PAGE_REMOTE,
             "Remote help lets the developer of this agent use this computer over "
             "the Internet, to debug or write drivers for hardware they do not "
             "have: run programs, see the screen, use the keyboard and mouse, copy "
             "files and restart it. Every command is signed on the developer's PC and checked "
             "here. The connection is not encrypted, so do not keep private files "
             "on this machine while it is on.",
             24, 46, w - 48, 92);
    v9x_add(V9X_PAGE_REMOTE, 0ul, "BUTTON", "Off: never contact the server (default)",
            BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 24, 142, w - 48, 20,
            V9X_ID_REMOTE_OFF);
    v9x_add(V9X_PAGE_REMOTE, 0ul, "BUTTON",
            "On: offer this machine for remote help",
            BS_AUTORADIOBUTTON, 24, 162, w - 48, 20, V9X_ID_REMOTE_ON);
    v9x_text(V9X_PAGE_REMOTE,
             "\"Pause remote help\" on the tray icon blocks commands until the "
             "agent restarts; chat keeps working.", 44, 182, w - 68, 30);
    v9x_text(V9X_PAGE_REMOTE, "Your name:", 24, 220, 120, 18);
    v9x_add(V9X_PAGE_REMOTE, WS_EX_CLIENTEDGE, "EDIT", v9x_settings.remote_name,
            ES_AUTOHSCROLL | WS_TABSTOP | WS_GROUP, 150, 217, w - 174, 22, V9X_ID_REMOTE_NAME);
    v9x_text(V9X_PAGE_REMOTE, "GitHub username:", 24, 248, 120, 18);
    v9x_add(V9X_PAGE_REMOTE, WS_EX_CLIENTEDGE, "EDIT", v9x_settings.remote_github,
            ES_AUTOHSCROLL | WS_TABSTOP, 150, 245, 160, 22, V9X_ID_REMOTE_GITHUB);
    v9x_text(V9X_PAGE_REMOTE, "(optional, for issues)", 318, 248, w - 334, 18);
    v9x_text(V9X_PAGE_REMOTE, "Contact email:", 24, 276, 120, 18);
    v9x_add(V9X_PAGE_REMOTE, WS_EX_CLIENTEDGE, "EDIT", v9x_settings.remote_email,
            ES_AUTOHSCROLL | WS_TABSTOP, 150, 273, w - 174, 22, V9X_ID_REMOTE_EMAIL);
    v9x_add(V9X_PAGE_REMOTE, 0ul, "STATIC", "", SS_LEFT, 24, 302, w - 48, 34,
            V9X_ID_REMOTE_STATUS);
    v9x_add(V9X_PAGE_REMOTE, 0ul, "BUTTON",
            "Check for agent updates when Windows starts",
            BS_AUTOCHECKBOX | WS_TABSTOP, 24, 336, w - 48, 20, V9X_ID_UPDATE_STARTUP);

    v9x_text(V9X_PAGE_INSTALL, v9x_settings_only ? "Saving..." : "Installing...",
             24, 48, w - 48, 18);
    v9x_add(V9X_PAGE_INSTALL, WS_EX_CLIENTEDGE, "LISTBOX", "",
            WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 24, 70, w - 48, 200, V9X_ID_PROGRESS);

    v9x_text(V9X_PAGE_FINISH, "", 24, 52, w - 48, 80);
    v9x_add(V9X_PAGE_FINISH, 0ul, "BUTTON", "Restart Windows now",
            BS_AUTOCHECKBOX | WS_TABSTOP, 24, 150, w - 48, 20, V9X_ID_REBOOT);

    v9x_add(V9X_PAGE_ALL, 0ul, "STATIC", "", SS_ETCHEDHORZ, 16, 374, w - 32, 2, -1);
    v9x_add(V9X_PAGE_ALL, 0ul, "BUTTON", "< Back", BS_PUSHBUTTON | WS_TABSTOP,
            w - 272, 386, 80, 24, V9X_ID_BACK);
    v9x_add(V9X_PAGE_ALL, 0ul, "BUTTON", "Next >", BS_DEFPUSHBUTTON | WS_TABSTOP,
            w - 186, 386, 80, 24, V9X_ID_NEXT);
    v9x_add(V9X_PAGE_ALL, 0ul, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP,
            w - 96, 386, 80, 24, V9X_ID_CANCEL);

    (void)SendMessageA(GetDlgItem(v9x_main, V9X_ID_DISCOVERY), BM_SETCHECK,
                       v9x_settings.discovery ? BST_CHECKED : BST_UNCHECKED, 0);
    (void)SendMessageA(GetDlgItem(v9x_main, V9X_ID_DOS), BM_SETCHECK,
                       v9x_settings.allow_dos_vm ? BST_CHECKED : BST_UNCHECKED, 0);
    (void)SendMessageA(GetDlgItem(v9x_main, V9X_ID_VNC), BM_SETCHECK,
                       v9x_settings.vnc ? BST_CHECKED : BST_UNCHECKED, 0);
    (void)SendMessageA(GetDlgItem(v9x_main, V9X_ID_PORT), EM_LIMITTEXT, 5, 0);
    (void)SendMessageA(GetDlgItem(v9x_main, V9X_ID_BIND), EM_LIMITTEXT, 15, 0);
    (void)SendMessageA(GetDlgItem(v9x_main, V9X_ID_CLIENT), EM_LIMITTEXT, 15, 0);
    (void)SendMessageA(GetDlgItem(v9x_main, V9X_ID_VNCPASS), EM_LIMITTEXT, 8, 0);
    (void)SendMessageA(GetDlgItem(v9x_main, v9x_settings.remote_on ? V9X_ID_REMOTE_ON :
                                                                     V9X_ID_REMOTE_OFF),
                       BM_SETCHECK, BST_CHECKED, 0);
    (void)SendMessageA(GetDlgItem(v9x_main, V9X_ID_UPDATE_STARTUP), BM_SETCHECK,
                       v9x_settings.update_on_startup ? BST_CHECKED : BST_UNCHECKED, 0);
    (void)SendMessageA(GetDlgItem(v9x_main, V9X_ID_REMOTE_NAME), EM_LIMITTEXT, 60, 0);
    (void)SendMessageA(GetDlgItem(v9x_main, V9X_ID_REMOTE_GITHUB), EM_LIMITTEXT, 39, 0);
    (void)SendMessageA(GetDlgItem(v9x_main, V9X_ID_REMOTE_EMAIL), EM_LIMITTEXT, 90, 0);
    {
        /* What the agent last wrote about its check-ins (remote.c). */
        char line[200];
        char value[64];
        line[0] = '\0';
        (void)GetPrivateProfileStringA("remote", "status", "", value, sizeof(value),
                                       v9x_status_ini);
        if (v9x_settings.remote_machine[0] != '\0') {
            v9x_append(line, "This machine is ", sizeof(line));
            v9x_append(line, v9x_settings.remote_machine, sizeof(line));
            v9x_append(line, value[0] != '\0' ? ", " : ".", sizeof(line));
        }
        if (value[0] != '\0') {
            v9x_append(line, line[0] != '\0' ? "status: " : "Status: ", sizeof(line));
            v9x_append(line, value, sizeof(line));
            (void)GetPrivateProfileStringA("remote", "last_contact", "", value,
                                           sizeof(value), v9x_status_ini);
            if (value[0] != '\0') {
                v9x_append(line, ", last contact ", sizeof(line));
                v9x_append(line, value, sizeof(line));
            }
            (void)GetPrivateProfileStringA("remote", "last_command_age", "-1", value,
                                           sizeof(value), v9x_status_ini);
            if (value[0] != '-') {
                v9x_append(line, ". Last command ", sizeof(line));
                v9x_append(line, value, sizeof(line));
                v9x_append(line, " s before that", sizeof(line));
            }
            v9x_append(line, ".", sizeof(line));
        }
        SetWindowTextA(GetDlgItem(v9x_main, V9X_ID_REMOTE_STATUS), line);
    }
}

static void v9x_show_page(int page)
{
    int index;
    HWND focus = 0;
    v9x_page = page;
    for (index = 0; index < v9x_control_count; ++index) {
        int visible = v9x_controls[index].page == V9X_PAGE_ALL ||
                      v9x_controls[index].page == page;
        ShowWindow(v9x_controls[index].window, visible ? SW_SHOW : SW_HIDE);
    }
    EnableWindow(GetDlgItem(v9x_main, V9X_ID_BACK),
                 page == V9X_PAGE_OPTIONS && !v9x_from_tray);
    EnableWindow(GetDlgItem(v9x_main, V9X_ID_NEXT), page != V9X_PAGE_INSTALL);
    EnableWindow(GetDlgItem(v9x_main, V9X_ID_CANCEL), page != V9X_PAGE_INSTALL);
    ShowWindow(GetDlgItem(v9x_main, V9X_ID_CANCEL), page == V9X_PAGE_FINISH ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(v9x_main, V9X_ID_BACK), page == V9X_PAGE_FINISH ? SW_HIDE : SW_SHOW);
    SetWindowTextA(GetDlgItem(v9x_main, V9X_ID_NEXT),
                   page == V9X_PAGE_REMOTE ? "Save" :
                   page == V9X_PAGE_OPTIONS ? (v9x_settings_only ? "Save" : "Install") :
                   page == V9X_PAGE_FINISH ? "Finish" : "Next >");
    if (page == V9X_PAGE_OPTIONS) focus = GetDlgItem(v9x_main, V9X_ID_DISCOVERY);
    if (page == V9X_PAGE_REMOTE) {
        focus = GetDlgItem(v9x_main, v9x_settings.remote_on ? V9X_ID_REMOTE_ON :
                                                              V9X_ID_REMOTE_OFF);
    }
    if (focus == 0) focus = GetDlgItem(v9x_main, V9X_ID_NEXT);
    SetFocus(focus);
}

/* Reads the Options page into v9x_settings; complains and returns 0 when a
   field is not valid. */
static int v9x_collect(void)
{
    unsigned long port;
    char port_text[16];
    char bind[32];
    char client[32];
    GetWindowTextA(GetDlgItem(v9x_main, V9X_ID_PORT), port_text, sizeof(port_text));
    GetWindowTextA(GetDlgItem(v9x_main, V9X_ID_BIND), bind, sizeof(bind));
    GetWindowTextA(GetDlgItem(v9x_main, V9X_ID_CLIENT), client, sizeof(client));
    if (!v9x_parse_port(port_text, &port)) {
        MessageBoxA(v9x_main, "The TCP port must be a number from 1 to 65535.",
                    v9x_title, MB_OK | MB_ICONEXCLAMATION);
        SetFocus(GetDlgItem(v9x_main, V9X_ID_PORT));
        return 0;
    }
    if (!v9x_valid_ipv4(bind)) {
        MessageBoxA(v9x_main, "The listen address must be an IPv4 address such as "
                    "0.0.0.0 or 192.168.1.20.", v9x_title, MB_OK | MB_ICONEXCLAMATION);
        SetFocus(GetDlgItem(v9x_main, V9X_ID_BIND));
        return 0;
    }
    if (client[0] != '\0' && !v9x_valid_ipv4(client)) {
        MessageBoxA(v9x_main, "The allowed client must be blank or an IPv4 address.",
                    v9x_title, MB_OK | MB_ICONEXCLAMATION);
        SetFocus(GetDlgItem(v9x_main, V9X_ID_CLIENT));
        return 0;
    }
    v9x_decimal(v9x_settings.port, port);
    v9x_copy(v9x_settings.bind_address, bind, sizeof(v9x_settings.bind_address));
    v9x_copy(v9x_settings.allowed_client, client, sizeof(v9x_settings.allowed_client));
    v9x_settings.discovery = SendMessageA(GetDlgItem(v9x_main, V9X_ID_DISCOVERY),
                                          BM_GETCHECK, 0, 0) == BST_CHECKED;
    v9x_settings.allow_dos_vm = SendMessageA(GetDlgItem(v9x_main, V9X_ID_DOS),
                                             BM_GETCHECK, 0, 0) == BST_CHECKED;
    v9x_settings.vnc = SendMessageA(GetDlgItem(v9x_main, V9X_ID_VNC),
                                    BM_GETCHECK, 0, 0) == BST_CHECKED;
    /* VNC authentication uses at most 8 characters; the field is limited
       to that, so a longer one cannot be typed. */
    GetWindowTextA(GetDlgItem(v9x_main, V9X_ID_VNCPASS), v9x_settings.vnc_password,
                   sizeof(v9x_settings.vnc_password));
    return 1;
}

/* Reads the remote help page; the name is required to turn it on. */
static int v9x_collect_remote(void)
{
    unsigned long index;
    int at;
    v9x_settings.remote_on = SendMessageA(GetDlgItem(v9x_main, V9X_ID_REMOTE_ON),
                                          BM_GETCHECK, 0, 0) == BST_CHECKED;
    v9x_settings.update_on_startup =
        SendMessageA(GetDlgItem(v9x_main, V9X_ID_UPDATE_STARTUP), BM_GETCHECK, 0, 0) ==
        BST_CHECKED;
    GetWindowTextA(GetDlgItem(v9x_main, V9X_ID_REMOTE_NAME), v9x_settings.remote_name,
                   sizeof(v9x_settings.remote_name));
    GetWindowTextA(GetDlgItem(v9x_main, V9X_ID_REMOTE_GITHUB), v9x_settings.remote_github,
                   sizeof(v9x_settings.remote_github));
    GetWindowTextA(GetDlgItem(v9x_main, V9X_ID_REMOTE_EMAIL), v9x_settings.remote_email,
                   sizeof(v9x_settings.remote_email));
    /* GitHub names are letters, digits and hyphens; drop an @ and spaces. */
    at = 0;
    for (index = 0ul; v9x_settings.remote_github[index] != '\0'; ++index) {
        char c = v9x_settings.remote_github[index];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-') {
            v9x_settings.remote_github[at++] = c;
        }
    }
    v9x_settings.remote_github[at] = '\0';
    if (v9x_settings.remote_on && v9x_settings.remote_name[0] == '\0') {
        MessageBoxA(v9x_main, "Please enter a name so the developer knows whose machine "
                    "this is.", v9x_title, MB_OK | MB_ICONEXCLAMATION);
        SetFocus(GetDlgItem(v9x_main, V9X_ID_REMOTE_NAME));
        return 0;
    }
    return 1;
}

static void v9x_finish_text(void)
{
    const char *text;
    int index;
    if (!v9x_install_ok) {
        text = "Setup could not finish; the step list marked the one that FAILED. "
               "A running agent has not been changed. Check that C:\\ is writable "
               "and run Setup again.";
    } else if (v9x_remote_page) {
        text = v9x_settings.remote_on ?
               "Remote help is on. The agent restarts when you click Finish and then "
               "checks in with the developer's server; the tray icon's menu has "
               "Pause and Chat." :
               "Remote help is off. The agent restarts when you click Finish and does "
               "not contact the server any more.";
    } else if (v9x_from_tray) {
        text = "The settings were saved. The agent restarts when you click Finish "
               "and uses them from then on; a command or file transfer it is "
               "running at that moment is cut off.";
    } else if (v9x_settings_only) {
        text = "The settings were saved. They take effect when the agent next "
               "starts, which is at the next restart.";
    } else if (v9x_existing) {
        text = "The new version is staged. It replaces the running agent when "
               "Windows restarts, keeping this machine's settings and install ID.";
    } else {
        text = "The V9x Remote Agent is installed. It starts when Windows restarts "
               "and logs to C:\\V9XREMOTE\\AGENT.LOG.";
    }
    /* The finish text is the first STATIC created for that page. */
    for (index = 0; index < v9x_control_count; ++index) {
        if (v9x_controls[index].page == V9X_PAGE_FINISH) {
            SetWindowTextA(v9x_controls[index].window, text);
            break;
        }
    }
    (void)SendMessageA(GetDlgItem(v9x_main, V9X_ID_REBOOT), BM_SETCHECK,
                       v9x_install_ok && !v9x_from_tray ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(GetDlgItem(v9x_main, V9X_ID_REBOOT), v9x_install_ok);
}

static void v9x_next(void)
{
    if (v9x_page == V9X_PAGE_REMOTE) {
        if (!v9x_collect_remote()) return;
        v9x_install_ok = v9x_write_remote();
        v9x_exit_code = v9x_install_ok ? 0 : 1;
        v9x_finish_text();
        v9x_show_page(V9X_PAGE_FINISH);
        ShowWindow(GetDlgItem(v9x_main, V9X_ID_REBOOT), SW_HIDE);
        return;
    }
    if (v9x_page == V9X_PAGE_WELCOME) {
        v9x_show_page(V9X_PAGE_OPTIONS);
    } else if (v9x_page == V9X_PAGE_OPTIONS) {
        HCURSOR previous;
        if (!v9x_collect()) return;
        v9x_show_page(V9X_PAGE_INSTALL);
        UpdateWindow(v9x_main);
        previous = SetCursor(LoadCursorA(0, IDC_WAIT));
        v9x_busy = 1;
        v9x_install_ok = v9x_install();
        v9x_busy = 0;
        SetCursor(previous);
        v9x_exit_code = v9x_install_ok ? 0 : 1;
        v9x_finish_text();
        /* Leave the progress list up for a moment so a failure can be read. */
        EnableWindow(GetDlgItem(v9x_main, V9X_ID_NEXT), TRUE);
        SetWindowTextA(GetDlgItem(v9x_main, V9X_ID_NEXT), "Next >");
        SetFocus(GetDlgItem(v9x_main, V9X_ID_NEXT));
    } else if (v9x_page == V9X_PAGE_INSTALL) {
        v9x_show_page(V9X_PAGE_FINISH);
        /* Restarting the agent replaces a reboot. */
        if (v9x_from_tray) ShowWindow(GetDlgItem(v9x_main, V9X_ID_REBOOT), SW_HIDE);
    } else {
        int reboot = SendMessageA(GetDlgItem(v9x_main, V9X_ID_REBOOT), BM_GETCHECK,
                                  0, 0) == BST_CHECKED;
        DestroyWindow(v9x_main);
        if (reboot && v9x_platform_enable_shutdown()) {
            (void)ExitWindowsEx(EWX_REBOOT, 0ul);
        }
    }
}

static LRESULT CALLBACK v9x_window_proc(HWND window, UINT message, WPARAM wparam,
                                        LPARAM lparam)
{
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case V9X_ID_NEXT:
        case IDOK:
            if (IsWindowEnabled(GetDlgItem(window, V9X_ID_NEXT))) v9x_next();
            return 0;
        case V9X_ID_BACK:
            if (v9x_page == V9X_PAGE_OPTIONS) v9x_show_page(V9X_PAGE_WELCOME);
            return 0;
        case V9X_ID_CANCEL:
        case IDCANCEL:
            if (v9x_page == V9X_PAGE_FINISH) {
                DestroyWindow(window);
            } else if (v9x_page != V9X_PAGE_INSTALL) {
                PostMessageA(window, WM_CLOSE, 0, 0);
            }
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        if (v9x_busy) return 0;
        if (v9x_page == V9X_PAGE_INSTALL || v9x_page == V9X_PAGE_FINISH ||
            MessageBoxA(window, (v9x_from_tray || v9x_remote_page) ?
                                "Close without saving the settings?" :
                        "Cancel setup? Nothing has been installed.", v9x_title,
                        MB_YESNO | MB_ICONQUESTION) == IDYES) {
            DestroyWindow(window);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

static int v9x_run_wizard(void)
{
    WNDCLASSA klass;
    RECT frame;
    MSG message;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

    klass.style = 0u;
    klass.lpfnWndProc = v9x_window_proc;
    klass.cbClsExtra = 0;
    klass.cbWndExtra = 0;
    klass.hInstance = v9x_instance;
    klass.hIcon = LoadIconA(0, IDI_APPLICATION);
    klass.hCursor = LoadCursorA(0, IDC_ARROW);
    klass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    klass.lpszMenuName = 0;
    klass.lpszClassName = "V9xRemoteSetup";
    if (RegisterClassA(&klass) == 0) return 1;

    frame.left = 0;
    frame.top = 0;
    frame.right = V9X_SETUP_WIDTH;
    frame.bottom = V9X_SETUP_HEIGHT;
    (void)AdjustWindowRectEx(&frame, style, FALSE, 0ul);
    v9x_main = CreateWindowExA(0ul, klass.lpszClassName, v9x_title, style,
                               (GetSystemMetrics(SM_CXSCREEN) - (frame.right - frame.left)) / 2,
                               (GetSystemMetrics(SM_CYSCREEN) - (frame.bottom - frame.top)) / 2,
                               frame.right - frame.left, frame.bottom - frame.top,
                               0, 0, v9x_instance, 0);
    if (v9x_main == 0) return 1;
    v9x_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    v9x_build();
    v9x_show_page(v9x_remote_page ? V9X_PAGE_REMOTE :
                  v9x_from_tray ? V9X_PAGE_OPTIONS : V9X_PAGE_WELCOME);
    ShowWindow(v9x_main, SW_SHOWNORMAL);
    /* Started by the agent from a tray click, so bring it to the front. */
    (void)SetForegroundWindow(v9x_main);
    UpdateWindow(v9x_main);
    while (GetMessageA(&message, 0, 0u, 0u) > 0) {
        if (IsWindow(v9x_main) && IsDialogMessageA(v9x_main, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return v9x_exit_code;
}

/* ---- entry ---- */

static int v9x_has_switch(const char *line, const char *name)
{
    /* Case-insensitive search for "/name" or "-name" as a whole word. */
    unsigned long length = v9x_len(name);
    for (; *line != '\0'; ++line) {
        unsigned long index;
        if (*line != '/' && *line != '-') continue;
        for (index = 0ul; index < length; ++index) {
            char left = line[1ul + index];
            char right = name[index];
            if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
            if (left != right) break;
        }
        if (index == length &&
            (line[1ul + length] == '\0' || line[1ul + length] == ' ')) {
            return 1;
        }
    }
    return 0;
}

void WINAPI V9xSetupEntry(void)
{
    const char *line = GetCommandLineA();
    char module[MAX_PATH];
    unsigned long length;
    int silent;

    v9x_instance = GetModuleHandleA(0);
    if (line == 0) line = "";
    /* Skip the program name so a path containing "/s" cannot match. */
    if (*line == '"') {
        ++line;
        while (*line != '\0' && *line != '"') ++line;
        if (*line == '"') ++line;
    } else {
        while (*line != '\0' && *line != ' ') ++line;
    }
    silent = v9x_has_switch(line, "s");
    if (v9x_has_switch(line, "uninstall")) ExitProcess((UINT)v9x_uninstall(silent));

    length = GetModuleFileNameA(0, module, sizeof(module));
    if (length == 0ul || length >= sizeof(module)) ExitProcess(1u);
    while (length > 0ul && module[length - 1ul] != '\\') --length;
    module[length > 0ul ? length - 1ul : 0ul] = '\0';
    v9x_copy(v9x_source, module, sizeof(v9x_source));
    v9x_settings_only = lstrcmpiA(v9x_source, v9x_root) == 0;
    v9x_existing = v9x_exists(v9x_agent);
    /* Only the installed copy edits the running agent's settings. */
    v9x_from_tray = v9x_settings_only && v9x_has_switch(line, "settings");
    if (v9x_from_tray) v9x_title = v9x_settings_title;
    if (v9x_has_switch(line, "update")) {
        ExitProcess((UINT)v9x_self_update(v9x_has_switch(line, "child")));
    }
    v9x_remote_page = v9x_settings_only && v9x_has_switch(line, "remote");
    if (v9x_remote_page) {
        v9x_from_tray = 1;
        v9x_title = v9x_remote_title;
    }
    if (!v9x_settings_only) {
        char agent[MAX_PATH];
        char helper[MAX_PATH];
        v9x_path(agent, v9x_source, "V9XAGNT.EXE");
        v9x_path(helper, v9x_source, "V9XSHOT.EXE");
        if (!v9x_exists(agent) || !v9x_exists(helper)) {
            if (!silent) {
                MessageBoxA(0, "The install package is incomplete: V9XAGNT.EXE and "
                            "V9XSHOT.EXE must be next to SETUP.EXE.", v9x_title,
                            MB_OK | MB_ICONSTOP);
            }
            ExitProcess(1u);
        }
    }
    v9x_load_settings();
    if (v9x_has_switch(line, "vnc")) v9x_settings.vnc = 1;
    if (v9x_has_switch(line, "novnc")) v9x_settings.vnc = 0;
    if (silent) ExitProcess(v9x_install() ? 0u : 1u);
    ExitProcess((UINT)v9x_run_wizard());
}
