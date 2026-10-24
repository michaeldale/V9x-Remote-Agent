/* SETUP.EXE /update: check the update server for a newer agent, and on the
   owner's word download, verify and stage it (docs\remote-help.md).

   The installed C:\V9XREMOTE\SETUP.EXE cannot be replaced while it runs, and
   the new package's own SETUP.EXE /S copies itself over it, so the copy the
   tray starts first copies itself to C:\V9XREMOTE\TEMP\SETUPUPD.EXE and runs
   that with /update /child; the child does the work.

   Nothing from the network is trusted until the Ed25519 signature on the
   release's SIGNED.TXT verifies under include\v9xremote\release_key.h; the
   zip is then accepted only if its size and SHA-256 match what that file
   says, and only a version newer than this one is installed. The verified
   package is unpacked into C:\V9XREMOTE\UPDATE and its SETUP.EXE /S stages
   it exactly as a manual upgrade does (the new agent takes over at the next
   restart), keeping AGENT.INI and the install ID.

   Exit code: 0 an update was staged, 1 failed, 2 cancelled, 3 nothing newer
   (or nothing published) for this agent. None of them makes the agent restart.

   SETUP.EXE /update /S (0.13.1+) is the same flow with nobody at the machine,
   for hosts that trigger it over the protocol (EXEC): no windows and no
   questions (yes is assumed), never a reboot, and the installed copy waits
   for the child so the exit code reaches the caller. Every outcome is also
   written to C:\V9XREMOTE\TEMP\UPDATE.INI, [result] status= (staged,
   current, none or failed), installed=, latest= and message=. The host then
   applies the staged V9XNEW.EXE / V9XSNEW.EXE with UPDATE_REQUEST or reboots;
   the signature and hash checks are exactly the same. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock.h>
#include "v9xremote/version.h"
#include "v9xremote/update_proto.h"
#include "v9xremote/update_release.h"
#include "v9xremote/release_key.h"
#include "v9xremote/sha256.h"
#include "v9xremote/zipread.h"
#include "../guest/http.h"
#include "../guest/platform_win.h"

static const char v9x_su_ini[] = "C:\\V9XREMOTE\\AGENT.INI";
static const char v9x_su_root[] = "C:\\V9XREMOTE";
static const char v9x_su_temp[] = "C:\\V9XREMOTE\\TEMP";
static const char v9x_su_child[] = "C:\\V9XREMOTE\\TEMP\\SETUPUPD.EXE";
static const char v9x_su_unpack_dir[] = "C:\\V9XREMOTE\\UPDATE";
static const char v9x_su_title[] = "V9x Remote Agent " V9X_AGENT_VERSION " - Update";
static const char v9x_su_result[] = "C:\\V9XREMOTE\\TEMP\\UPDATE.INI";

#define V9X_SU_TIMEOUT_MS 30000ul
#define V9X_SU_TEXT_MAX 65536ul
#define V9X_SU_ZIP_MAX (16ul * 1024ul * 1024ul)

static HWND v9x_su_status;
static int v9x_su_quiet;
static char v9x_su_latest[24];

/* /S: record the outcome for the host instead of showing it. */
static void v9x_su_record(const char *status, const char *message)
{
    (void)CreateDirectoryA(v9x_su_temp, 0);
    (void)WritePrivateProfileStringA("result", "status", status, v9x_su_result);
    (void)WritePrivateProfileStringA("result", "installed", V9X_AGENT_VERSION, v9x_su_result);
    (void)WritePrivateProfileStringA("result", "latest", v9x_su_latest, v9x_su_result);
    (void)WritePrivateProfileStringA("result", "message", message, v9x_su_result);
    /* Windows 9x caches the last INI written; flush it so a read sees it now. */
    (void)WritePrivateProfileStringA(0, 0, 0, v9x_su_result);
}

static void v9x_su_copy(char *target, const char *source, unsigned long capacity)
{
    lstrcpynA(target, source, (int)capacity);
}

static void v9x_su_append(char *target, const char *source, unsigned long capacity)
{
    unsigned long at = (unsigned long)lstrlenA(target);
    while (*source != '\0' && at + 1ul < capacity) target[at++] = *source++;
    target[at] = '\0';
}

/* A small borderless "please wait" window, so a 30 s network wait does not
   look like a hang. */
static void v9x_su_show(const char *text)
{
    MSG message;
    if (v9x_su_quiet) return;
    if (v9x_su_status == 0) {
        int width = 360;
        int height = 60;
        v9x_su_status = CreateWindowExA(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, "STATIC", text,
                                        WS_POPUP | WS_VISIBLE | SS_CENTER | WS_BORDER,
                                        (GetSystemMetrics(SM_CXSCREEN) - width) / 2,
                                        (GetSystemMetrics(SM_CYSCREEN) - height) / 2,
                                        width, height, 0, 0, GetModuleHandleA(0), 0);
    } else {
        (void)SetWindowTextA(v9x_su_status, text);
    }
    if (v9x_su_status != 0) UpdateWindow(v9x_su_status);
    while (PeekMessageA(&message, 0, 0u, 0u, PM_REMOVE)) DispatchMessageA(&message);
}

static void v9x_su_hide(void)
{
    if (v9x_su_status != 0) DestroyWindow(v9x_su_status);
    v9x_su_status = 0;
}

static int v9x_su_fail(const char *text)
{
    v9x_su_hide();
    if (v9x_su_quiet) {
        v9x_su_record("failed", text);
        return 1;
    }
    MessageBoxA(0, text, v9x_su_title, MB_OK | MB_ICONEXCLAMATION);
    return 1;
}

/* GET url into buffer; the body is at *body with *length bytes and a NUL
   after it. */
static int v9x_su_get(const char *url, unsigned char *buffer, unsigned long capacity,
                      const unsigned char **body, unsigned long *length)
{
    V9xHttpRequest request;
    V9xHttpResponse response;
    DWORD native_error = 0ul;
    request.method = "GET";
    request.url = url;
    request.headers = 0;
    request.body = 0;
    request.body_length = 0ul;
    request.timeout_ms = V9X_SU_TIMEOUT_MS;
    if (!v9x_http_request(&request, buffer, capacity - 1ul, &response, &native_error) ||
        response.status != 200ul) {
        return 0;
    }
    buffer[response.body_offset + response.body_length] = '\0';
    *body = buffer + response.body_offset;
    *length = response.body_length;
    return 1;
}

static int v9x_su_write_file(const char *path, const unsigned char *data,
                             unsigned long length)
{
    HANDLE file;
    DWORD written = 0ul;
    int ok;
    (void)SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    file = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(file, data, length, &written, 0) && written == length;
    CloseHandle(file);
    return ok;
}

/* Unpack every plain file of the verified zip into C:\V9XREMOTE\UPDATE. */
static int v9x_su_unpack(const unsigned char *zip_data, unsigned long zip_length,
                         int *found_setup)
{
    static struct v9x_inflate_state inflate_state;
    struct v9x_zip zip;
    struct v9x_zip_entry entry;
    char name[MAX_PATH];
    char path[MAX_PATH];
    unsigned char *output;
    v9x_u32 written;
    v9x_u16 index;
    unsigned long k;

    *found_setup = 0;
    if (v9x_zip_open(&zip, zip_data, zip_length) != V9X_ZIP_OK) return 0;
    (void)CreateDirectoryA(v9x_su_unpack_dir, 0);
    for (index = 0u; index < zip.entry_count; ++index) {
        if (v9x_zip_entry_at(&zip, index, &entry) != V9X_ZIP_OK) return 0;
        /* Folders (a trailing slash) are skipped; anything else must be one
           safe file name. */
        if (entry.name_length != 0u && entry.name[entry.name_length - 1u] == '/') continue;
        if (!v9x_zip_name_is_safe(entry.name, entry.name_length, 64u)) return 0;
        for (k = 0ul; k < entry.name_length; ++k) name[k] = (char)entry.name[k];
        name[entry.name_length] = '\0';
        output = (unsigned char *)VirtualAlloc(0, entry.uncompressed_size + 1ul,
                                               MEM_COMMIT, PAGE_READWRITE);
        if (output == 0) return 0;
        if (v9x_zip_extract(&zip, index, &inflate_state, output,
                            entry.uncompressed_size + 1ul, &written) != V9X_ZIP_OK) {
            VirtualFree(output, 0ul, MEM_RELEASE);
            return 0;
        }
        v9x_su_copy(path, v9x_su_unpack_dir, sizeof(path));
        v9x_su_append(path, "\\", sizeof(path));
        v9x_su_append(path, name, sizeof(path));
        if (!v9x_su_write_file(path, output, written)) {
            VirtualFree(output, 0ul, MEM_RELEASE);
            return 0;
        }
        VirtualFree(output, 0ul, MEM_RELEASE);
        if (lstrcmpiA(name, "SETUP.EXE") == 0) *found_setup = 1;
    }
    return 1;
}

static int v9x_su_run_wait(const char *exe, const char *arguments, DWORD *code)
{
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    char command[MAX_PATH + 64];
    command[0] = '"';
    command[1] = '\0';
    v9x_su_append(command, exe, sizeof(command));
    v9x_su_append(command, "\" ", sizeof(command));
    v9x_su_append(command, arguments, sizeof(command));
    GetStartupInfoA(&startup);
    if (!CreateProcessA(exe, command, 0, 0, FALSE, 0ul, 0, v9x_su_root, &startup, &process)) {
        return 0;
    }
    (void)WaitForSingleObject(process.hProcess, INFINITE);
    *code = 1ul;
    (void)GetExitCodeProcess(process.hProcess, code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 1;
}

static int v9x_su_work(void)
{
    static const v9x_u8 public_key[32] = V9X_RELEASE_PUBLIC_KEY;
    WSADATA wsa;
    unsigned char *buffer;
    unsigned char *zip_buffer;
    const unsigned char *body;
    unsigned long body_length;
    char server[128];
    char url[V9X_HTTP_MAX_PATH];
    char status[24];
    char latest[24];
    char signed_url[V9X_HTTP_MAX_PATH];
    char notes_url[V9X_HTTP_MAX_PATH];
    char package_url[V9X_HTTP_MAX_PATH];
    char package_file[96];
    char package_sha[80];
    char notes[1600];
    char question[2400];
    char *signed_text;
    v9x_u32 covered;
    struct v9x_release_info info;
    struct v9x_sha256 hash;
    v9x_u8 digest[32];
    unsigned long index;
    DWORD code;
    int found_setup;

    if (WSAStartup(MAKEWORD(1, 1), &wsa) != 0) {
        return v9x_su_fail("Windows networking (Winsock) is not available.");
    }
    buffer = (unsigned char *)VirtualAlloc(0, V9X_SU_TEXT_MAX, MEM_COMMIT, PAGE_READWRITE);
    signed_text = (char *)VirtualAlloc(0, V9X_SU_TEXT_MAX, MEM_COMMIT, PAGE_READWRITE);
    zip_buffer = (unsigned char *)VirtualAlloc(0, V9X_SU_ZIP_MAX + 16384ul, MEM_COMMIT,
                                               PAGE_READWRITE);
    if (buffer == 0 || signed_text == 0 || zip_buffer == 0) {
        return v9x_su_fail("Not enough memory to check for updates.");
    }

    /* 1. Ask. */
    (void)GetPrivateProfileStringA("remote", "server", V9X_UPDATE_SERVER, server,
                                   sizeof(server), v9x_su_ini);
    v9x_su_copy(url, server, sizeof(url));
    v9x_su_append(url, "/index.php?bt_type=v9xu_check&app=" V9X_UPDATE_APP
                       "&family=" V9X_UPDATE_FAMILY "&version=" V9X_AGENT_VERSION,
                  sizeof(url));
    v9x_su_show("Checking for updates...");
    if (!v9x_su_get(url, buffer, V9X_SU_TEXT_MAX, &body, &body_length)) {
        return v9x_su_fail("The update server could not be reached. Check that this "
                           "computer can open web sites (plain http) and try again.");
    }
    if (!v9x_update_ini_value((const char *)body, body_length, "update", "status",
                              status, sizeof(status))) {
        return v9x_su_fail("The update server sent a reply this version does not understand.");
    }
    if (lstrcmpA(status, "update") != 0) {
        int current = lstrcmpA(status, "current") == 0;
        const char *text = current ? "This is the newest version of the V9x Remote Agent." :
                                     "No update is available for this agent right now.";
        v9x_su_hide();
        if (v9x_su_quiet) {
            v9x_su_record(current ? "current" : "none", text);
            return 3;
        }
        MessageBoxA(0, text, v9x_su_title, MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    if (!v9x_update_ini_value((const char *)body, body_length, "update", "latest",
                              latest, sizeof(latest)) ||
        !v9x_update_ini_value((const char *)body, body_length, "update", "signed",
                              signed_url, sizeof(signed_url)) ||
        !v9x_update_ini_value((const char *)body, body_length, "package", "url",
                              package_url, sizeof(package_url)) ||
        !v9x_update_ini_value((const char *)body, body_length, "package", "file",
                              package_file, sizeof(package_file)) ||
        !v9x_update_ini_value((const char *)body, body_length, "package", "sha256",
                              package_sha, sizeof(package_sha))) {
        return v9x_su_fail("The update server announced an update without a signed "
                           "release file, so it cannot be checked. Try again later.");
    }
    if (!v9x_update_ini_value((const char *)body, body_length, "update", "notes",
                              notes_url, sizeof(notes_url))) {
        notes_url[0] = '\0';
    }
    v9x_su_copy(v9x_su_latest, latest, sizeof(v9x_su_latest));

    /* 2. The signed release file decides everything that follows. */
    v9x_su_show("Checking the release signature...");
    if (!v9x_su_get(signed_url, buffer, V9X_SU_TEXT_MAX, &body, &body_length) ||
        body_length >= V9X_SU_TEXT_MAX) {
        return v9x_su_fail("The signed release file could not be downloaded.");
    }
    for (index = 0ul; index < body_length; ++index) signed_text[index] = (char)body[index];
    signed_text[body_length] = '\0';
    if (!v9x_release_verify(signed_text, body_length, public_key, &covered) ||
        !v9x_release_read(signed_text, covered, V9X_UPDATE_APP, V9X_UPDATE_FAMILY, &info)) {
        return v9x_su_fail("The release file's signature is not valid. Nothing was "
                           "downloaded or changed.");
    }
    if (lstrcmpA(info.version, latest) != 0 ||
        v9x_update_version_compare(info.version, V9X_AGENT_VERSION) != 1 ||
        lstrcmpiA(info.file, package_file) != 0) {
        return v9x_su_fail("The signed release does not match what the server offered, "
                           "or is not newer than this version. Nothing was changed.");
    }

    /* 3. Release notes, best effort, then ask. */
    notes[0] = '\0';
    if (notes_url[0] != '\0' &&
        v9x_su_get(notes_url, buffer, V9X_SU_TEXT_MAX, &body, &body_length)) {
        v9x_su_copy(notes, (const char *)body, sizeof(notes) - 4ul);
        if (body_length >= sizeof(notes) - 4ul) v9x_su_append(notes, "...", sizeof(notes));
    }
    v9x_su_hide();
    v9x_su_copy(question, "Version ", sizeof(question));
    v9x_su_append(question, info.version, sizeof(question));
    v9x_su_append(question, " is available (this is " V9X_AGENT_VERSION ").\r\n\r\n",
                  sizeof(question));
    if (notes[0] != '\0') {
        v9x_su_append(question, notes, sizeof(question));
        v9x_su_append(question, "\r\n\r\n", sizeof(question));
    }
    v9x_su_append(question, "Download and install it now? The new version takes over "
                            "when Windows next restarts; settings and the install ID "
                            "are kept.", sizeof(question));
    if (!v9x_su_quiet &&
        MessageBoxA(0, question, v9x_su_title, MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return 2;
    }

    /* 4. Download and check against the signed size and SHA-256. */
    if (info.size > V9X_SU_ZIP_MAX) {
        return v9x_su_fail("The update is larger than this version can install.");
    }
    v9x_su_show("Downloading the update...");
    if (!v9x_su_get(package_url, zip_buffer, V9X_SU_ZIP_MAX + 16384ul, &body, &body_length)) {
        return v9x_su_fail("The update could not be downloaded. Nothing was changed.");
    }
    v9x_sha256_init(&hash);
    v9x_sha256_update(&hash, body, body_length);
    v9x_sha256_final(&hash, digest);
    for (index = 0ul; index < 32ul && digest[index] == info.sha256[index]; ++index) {
    }
    if (body_length != info.size || index != 32ul) {
        return v9x_su_fail("The downloaded update does not match the signed release "
                           "(size or SHA-256). Nothing was changed.");
    }

    /* 5. Unpack and stage with the new package's own setup. */
    v9x_su_show("Installing the update...");
    if (!v9x_su_unpack(body, body_length, &found_setup) || !found_setup) {
        return v9x_su_fail("The update package could not be unpacked. Nothing was changed.");
    }
    {
        char setup[MAX_PATH];
        v9x_su_copy(setup, v9x_su_unpack_dir, sizeof(setup));
        v9x_su_append(setup, "\\SETUP.EXE", sizeof(setup));
        if (!v9x_su_run_wait(setup, "/S", &code) || code != 0ul) {
            return v9x_su_fail("The new version's setup reported a problem. The running "
                               "agent has not been changed; see C:\\V9XREMOTE\\UPDATE.");
        }
    }
    v9x_su_hide();
    if (v9x_su_quiet) {
        v9x_su_record("staged", "The update is staged (V9XNEW.EXE, V9XSNEW.EXE) and takes over "
                                "at the next restart or hot update.");
        return 0;
    }
    if (MessageBoxA(0, "The update is installed and takes over when Windows restarts.\r\n\r\n"
                       "Restart Windows now?", v9x_su_title,
                    MB_YESNO | MB_ICONQUESTION) == IDYES &&
        v9x_platform_enable_shutdown()) {
        (void)ExitWindowsEx(EWX_REBOOT, 0ul);
    }
    return 0;
}

int v9x_self_update(int child, int quiet)
{
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    char self[MAX_PATH];
    char command[MAX_PATH + 32];
    DWORD code;
    v9x_su_quiet = quiet;
    if (child) return v9x_su_work();
    if (quiet) (void)DeleteFileA(v9x_su_result);
    /* The installed copy hands over to a temporary one (see the top) and
       exits at once, so nothing holds C:\V9XREMOTE\SETUP.EXE open. */
    if (GetModuleFileNameA(0, self, sizeof(self)) == 0ul) return 1;
    (void)CreateDirectoryA(v9x_su_temp, 0);
    (void)SetFileAttributesA(v9x_su_child, FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileA(self, v9x_su_child, FALSE)) return v9x_su_work();
    v9x_su_copy(command, "\"", sizeof(command));
    v9x_su_append(command, v9x_su_child, sizeof(command));
    v9x_su_append(command, quiet ? "\" /update /child /S" : "\" /update /child", sizeof(command));
    GetStartupInfoA(&startup);
    if (!CreateProcessA(v9x_su_child, command, 0, 0, FALSE, 0ul, 0, v9x_su_root,
                        &startup, &process)) {
        return v9x_su_work();
    }
    CloseHandle(process.hThread);
    /* Interactive: hand over and exit, as before. /S: the caller (an EXEC
       request) is waiting for the outcome, so wait and pass it on. */
    code = 1ul;
    if (quiet) {
        (void)WaitForSingleObject(process.hProcess, INFINITE);
        (void)GetExitCodeProcess(process.hProcess, &code);
    }
    CloseHandle(process.hProcess);
    return (int)code;
}
