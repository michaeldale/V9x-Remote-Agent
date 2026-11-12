#include "agent.h"
#include "v9xremote/status.h"

#define V9X_REG_VALUE_MAX 1024ul
#ifndef ENUM_CURRENT_SETTINGS
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
#define ENUM_REGISTRY_SETTINGS ((DWORD)-2)
#endif

static int operator_error(V9xConnection *conn, DWORD id, DWORD status,
                           DWORD native, const char *text)
{
    unsigned long offset = 8ul;
    v9x_write_u32(conn->response, status);
    v9x_write_u32(conn->response + 4, native);
    if (!v9x_append_string(conn->response, sizeof(conn->response), &offset, text)) return 0;
    return v9x_send_frame(conn, V9X_MSG_ERROR_RESPONSE, id, conn->response, offset);
}

static int read_string(const unsigned char *p, unsigned long n,
                        unsigned long *at, char *out, unsigned long cap)
{
    unsigned long size, i;
    if (*at > n || n - *at < 2ul) return 0;
    size = v9x_read_u16(p + *at); *at += 2ul;
    if (size >= cap || size > n - *at) return 0;
    for (i = 0; i < size; ++i) {
        if (p[*at + i] == 0u || p[*at + i] > 127u) return 0;
        out[i] = (char)p[*at + i];
    }
    out[size] = 0; *at += size; return 1;
}

static int under(const char *path, const char *prefix)
{
    unsigned long n = (unsigned long)lstrlenA(prefix);
    char part[MAX_PATH];
    if ((unsigned long)lstrlenA(path) < n || n >= sizeof(part)) return 0;
    lstrcpynA(part, path, (int)n + 1);
    return lstrcmpiA(part, prefix) == 0 && (path[n] == 0 || path[n] == '\\');
}

/* Registry equivalents of the filesystem protections: no hive roots,
   system/security/hardware branches or the agent's boot/update entries.
   Config\0001\Display and application settings remain writable. */
static int protected_key(DWORD hive, const char *path, const char *name)
{
    unsigned long i;
    if (path[0] == 0 || path[0] == '\\' || hive == 0x80000005ul || hive == 0x80000006ul) return 1;
    for (i = 0ul; path[i]; ++i)
        if (path[i] == '/' || (path[i] == '\\' && path[i + 1] == '\\')) return 1;
    if (hive == 0x80000002ul &&
        (under(path, "SYSTEM") || under(path, "SECURITY") || under(path, "SAM") ||
         under(path, "HARDWARE"))) return 1;
    if (under(path, "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\V9xRemoteAgent")) return 1;
    if (under(path, "Software\\Microsoft\\Windows\\CurrentVersion") &&
        (lstrcmpiA(name, "V9xRemoteAgent") == 0 ||
         lstrcmpiA(name, "V9xRemoteVnc") == 0 ||
         lstrcmpiA(name, "V9xRemoteUpdate") == 0)) return 1;
    return 0;
}

static int registry_write(V9xConnection *conn, DWORD id,
                            const unsigned char *p, unsigned long n)
{
    char path[MAX_PATH], name[MAX_PATH], line[768], previous[256];
    unsigned long at = 8ul, i, used;
    DWORD hive, op, size, type = 0ul, old_size = V9X_REG_VALUE_MAX;
    DWORD disposition;
    unsigned char *old = conn->file_response + 12;
    HKEY key;
    LONG status;
    int exists;
    if (n < 8ul || !read_string(p, n, &at, path, sizeof(path)) ||
        !read_string(p, n, &at, name, sizeof(name)) || n - at < 4ul)
        return operator_error(conn, id, V9X_STATUS_INVALID_PAYLOAD, 0, "invalid registry write");
    hive = v9x_read_u32(p); op = v9x_read_u32(p + 4);
    size = v9x_read_u32(p + at); at += 4ul;
    if (hive < 0x80000000ul || hive > 0x80000006ul || hive == 0x80000004ul ||
        op < 1ul || op > 3ul || size > V9X_REG_VALUE_MAX || n - at != size ||
        (op == V9X_REG_SET_DWORD && size != 4ul) ||
        (op == V9X_REG_DELETE_VALUE && size != 0ul))
        return operator_error(conn, id, V9X_STATUS_INVALID_PAYLOAD, 0, "invalid registry value");
    if (op == V9X_REG_SET_STRING) {
        if (size == 0 || p[n - 1] != 0u)
            return operator_error(conn, id, V9X_STATUS_INVALID_PAYLOAD, 0, "REG_SZ must be terminated");
        for (i = 0; i + 1ul < size; ++i) {
            if (p[at + i] == 0u || p[at + i] > 127u)
                return operator_error(conn, id, V9X_STATUS_INVALID_PAYLOAD, 0, "REG_SZ must be ASCII without NULs");
        }
    }
    if (protected_key(hive, path, name))
        return operator_error(conn, id, V9X_STATUS_ACCESS_DENIED, ERROR_ACCESS_DENIED, "refused: protected registry key/value");
    if (op == V9X_REG_DELETE_VALUE)
        status = RegOpenKeyExA((HKEY)hive, path, 0ul, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    else
        status = RegCreateKeyExA((HKEY)hive, path, 0ul, 0, REG_OPTION_NON_VOLATILE,
                                 KEY_QUERY_VALUE | KEY_SET_VALUE, 0, &key, &disposition);
    if (status != ERROR_SUCCESS)
        return operator_error(conn, id, V9X_STATUS_IO_FAILED, status, "cannot open registry key for writing");
    status = RegQueryValueExA(key, name, 0, &type, old, &old_size);
    exists = status == ERROR_SUCCESS;
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        RegCloseKey(key);
        return operator_error(conn, id, V9X_STATUS_IO_FAILED, status, "cannot capture old value (limit 1024 bytes); nothing written");
    }
    if (!exists) { type = 0; old_size = 0; }
    if (!exists) lstrcpyA(previous, "(absent)");
    else if (type == REG_DWORD && old_size == 4ul)
        wsprintfA(previous, "REG_DWORD %lu", v9x_read_u32(old));
    else if (type == REG_SZ || type == REG_EXPAND_SZ) {
        used = old_size < 200ul ? old_size : 200ul;
        for (i = 0; i < used; ++i) previous[i] = old[i] >= 32u && old[i] <= 126u ? old[i] : '?';
        if (used && old[used - 1] == 0u) --used;
        previous[used] = 0;
        if (old_size > 200ul) lstrcatA(previous, " [truncated; full old value in response]");
    } else wsprintfA(previous, "type %lu, %lu bytes (full old value in response)", type, old_size);
    wsprintfA(line, "Registry %s: %08lX\\%s [%s]", op == 3ul ? "delete value" : "set value", hive, path, name);
    v9x_event(line, 0);
    v9x_event("Previous registry value: ", previous);
    if (exists && ((type != REG_SZ && type != REG_EXPAND_SZ &&
                    (type != REG_DWORD || old_size != 4ul)) || old_size > 200ul)) {
        static const char hex[] = "0123456789ABCDEF";
        unsigned long start, j;
        /* Keep the complete old bytes even for binary/long strings; event
           lines are bounded and rotated, so split rather than truncate. */
        for (start = 0ul; start < old_size; start += 64ul) {
            used = (unsigned long)wsprintfA(line, "Previous value bytes %04lX: ", start);
            for (j = start; j < old_size && j < start + 64ul; ++j) {
                line[used++] = hex[old[j] >> 4]; line[used++] = hex[old[j] & 15u]; line[used++] = ' ';
            }
            line[used] = 0; v9x_event(line, 0);
        }
    }
    status = op == V9X_REG_DELETE_VALUE ? RegDeleteValueA(key, name) :
        RegSetValueExA(key, name, 0ul, op == V9X_REG_SET_STRING ? REG_SZ : REG_DWORD, p + at, size);
    if (op == V9X_REG_DELETE_VALUE && status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    if (status == ERROR_SUCCESS) status = RegFlushKey(key);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        v9x_event("Registry write/flush failed; inspect the value before retrying", 0);
        return operator_error(conn, id, V9X_STATUS_IO_FAILED, status, "registry write or flush failed");
    }
    v9x_write_u32(conn->file_response, exists ? 1ul : 0ul);
    v9x_write_u32(conn->file_response + 4, type);
    v9x_write_u32(conn->file_response + 8, old_size);
    return v9x_send_frame(conn, V9X_MSG_REG_WRITE_RESPONSE, id, conn->file_response, 12ul + old_size);
}

static int display(V9xConnection *conn, DWORD id, const unsigned char *p, unsigned long n)
{
    DEVMODEA mode;
    DWORD flags, answer = 0ul, i, at, j;
    LONG changed = DISP_CHANGE_SUCCESSFUL;
    if (n != 4ul || (v9x_read_u32(p) & ~1ul) != 0ul)
        return operator_error(conn, id, V9X_STATUS_INVALID_PAYLOAD, 0, "invalid display flags");
    flags = v9x_read_u32(p);
    if (flags & 1ul) {
        if (!v9x_desktop_ready())
            return operator_error(conn, id, V9X_STATUS_BUSY, 0, "desktop not ready for display restore");
        v9x_event("Restoring the desktop's saved display mode", 0);
        changed = ChangeDisplaySettingsA(0, 0ul);
        if (changed != DISP_CHANGE_SUCCESSFUL)
            return operator_error(conn, id, V9X_STATUS_IO_FAILED, (DWORD)changed, "ChangeDisplaySettings failed");
        answer |= 4ul;
    }
    for (i = 0; i < 2ul; ++i) {
        for (j = 0ul; j < sizeof(mode); ++j) ((unsigned char *)&mode)[j] = 0u;
        mode.dmSize = sizeof(mode);
        at = 8ul + i * 16ul;
        for (j = 0ul; j < 16ul; ++j) conn->response[at + j] = 0u;
        if (EnumDisplaySettingsA(0, i == 0 ? ENUM_CURRENT_SETTINGS : ENUM_REGISTRY_SETTINGS, &mode)) {
            answer |= 1ul << i;
            v9x_write_u32(conn->response + at, mode.dmPelsWidth);
            v9x_write_u32(conn->response + at + 4, mode.dmPelsHeight);
            v9x_write_u32(conn->response + at + 8, mode.dmBitsPerPel);
            v9x_write_u32(conn->response + at + 12, mode.dmDisplayFrequency);
        }
    }
    v9x_write_u32(conn->response, answer);
    v9x_write_u32(conn->response + 4, (DWORD)changed);
    return v9x_send_frame(conn, V9X_MSG_DISPLAY_RESPONSE, id, conn->response, 40ul);
}

int v9x_handle_operator(V9xConnection *conn, unsigned short type, unsigned long id,
                        const unsigned char *payload, unsigned long length)
{
    if (type == V9X_MSG_REG_WRITE_REQUEST) return registry_write(conn, id, payload, length);
    return display(conn, id, payload, length);
}
