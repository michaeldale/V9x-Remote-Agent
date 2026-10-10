#include "agent.h"
#include "platform_win.h"
#include "v9xremote/protocol.h"
#include "v9xremote/status.h"

/* INVENTORY: a schema-free list of typed records built from calls that are
   safe on every Windows 9x release. Payload: u16 record count, then per
   record a string type and a u16 field count followed by (key, value) string
   pairs. Values are forced to printable ASCII; byte counts are decimal text
   so nothing overflows at 32 bits. No GDI (the long-lived agent must not
   import it) and nothing that can pop an "insert disk" dialog.

   On NT (Windows 2000) the 9x registry locations do not exist, so the OS,
   BIOS, display and device records read the NT ones instead. CFGMGR32 and
   EnumDisplaySettingsA are resolved at run time, so on 9x the import table
   and every record are what they were before. */

#define V9X_INV_VALUE_MAX 255
#define V9X_INV_META_RESERVE 160ul
#define V9X_INV_MAX_DEVICES_PER_CLASS 32ul

typedef struct V9xInvWriter {
    unsigned char *buffer;
    unsigned long capacity;
    unsigned long offset;
    unsigned long records;
    unsigned long record_start;
    unsigned long count_offset;
    unsigned long fields;
    unsigned long reserve;
    int in_record;
    int truncated;
} V9xInvWriter;

/* 64-bit unsigned as two 32-bit halves. The agent is built without the C
   runtime, so 64-bit multiply/divide helpers are not available. */
typedef struct V9xU64 {
    unsigned long lo;
    unsigned long hi;
} V9xU64;

typedef BOOL (WINAPI *V9xDiskFreeExFn)(LPCSTR, V9xU64 *, V9xU64 *, V9xU64 *);

/* NT only, all resolved at run time (see the note above). CONFIGRET is a
   DWORD and CR_SUCCESS is 0; DN_HAS_PROBLEM is from cfg.h. */
typedef DWORD (WINAPI *V9xLocateDevNodeFn)(DWORD *, char *, ULONG);
typedef DWORD (WINAPI *V9xDevNodeStatusFn)(ULONG *, ULONG *, DWORD, ULONG);
typedef BOOL (WINAPI *V9xEnumDisplaySettingsFn)(LPCSTR, DWORD, DEVMODEA *);
#define V9X_DN_HAS_PROBLEM 0x00000400ul
#define V9X_ENUM_CURRENT_SETTINGS ((DWORD)-1)

unsigned long v9x_cpuid_supported(void);
#pragma aux v9x_cpuid_supported = \
    "pushfd" \
    "pop eax" \
    "mov ecx,eax" \
    "xor eax,200000h" \
    "push eax" \
    "popfd" \
    "pushfd" \
    "pop eax" \
    "push ecx" \
    "popfd" \
    "xor eax,ecx" \
    "and eax,200000h" \
    value [eax] \
    modify [eax ecx];

void v9x_cpuid(unsigned long leaf, unsigned long *regs);
#pragma aux v9x_cpuid = \
    "push ebx" \
    "xor ecx,ecx" \
    0x0f 0xa2 \
    "mov [edi],eax" \
    "mov [edi+4],ebx" \
    "mov [edi+8],ecx" \
    "mov [edi+12],edx" \
    "pop ebx" \
    parm [eax] [edi] \
    modify [eax ecx edx];

unsigned long v9x_rdtsc_low(void);
#pragma aux v9x_rdtsc_low = \
    0x0f 0x31 \
    value [eax] \
    modify [eax edx];

static unsigned long v9x_inv_u32(char *target, unsigned long value)
{
    char reversed[10];
    unsigned long count = 0ul;
    unsigned long index;
    do {
        reversed[count++] = (char)('0' + (value % 10ul));
        value /= 10ul;
    } while (value != 0ul && count < sizeof(reversed));
    for (index = 0ul; index < count; ++index) {
        target[index] = reversed[count - index - 1ul];
    }
    target[count] = '\0';
    return count;
}

static void v9x_inv_hex32(char *target, unsigned long value, int dash)
{
    static const char digits[] = "0123456789ABCDEF";
    int shift;
    int offset = 0;
    for (shift = 28; shift >= 0; shift -= 4) {
        target[offset++] = digits[(value >> shift) & 0xful];
        if (dash && shift == 16) target[offset++] = '-';
    }
    target[offset] = '\0';
}

static void v9x_u64_set(V9xU64 *value, unsigned long lo)
{
    value->lo = lo;
    value->hi = 0ul;
}

/* value = value * multiplier, computed from 16-bit limbs. */
static void v9x_u64_mul32(V9xU64 *value, unsigned long multiplier)
{
    unsigned long a0 = value->lo & 0xfffful;
    unsigned long a1 = value->lo >> 16;
    unsigned long b0 = multiplier & 0xfffful;
    unsigned long b1 = multiplier >> 16;
    unsigned long p00 = a0 * b0;
    unsigned long p01 = a0 * b1;
    unsigned long p10 = a1 * b0;
    unsigned long p11 = a1 * b1;
    unsigned long middle = (p00 >> 16) + (p01 & 0xfffful) + (p10 & 0xfffful);
    unsigned long lo = (p00 & 0xfffful) | ((middle & 0xfffful) << 16);
    unsigned long hi = p11 + (p01 >> 16) + (p10 >> 16) + (middle >> 16);
    value->hi = value->hi * multiplier + hi;
    value->lo = lo;
}

static void v9x_u64_decimal(char *target, const V9xU64 *source)
{
    unsigned long limbs[4];
    char reversed[21];
    unsigned long count = 0ul;
    unsigned long index;
    unsigned long remainder;
    unsigned long current;
    int limb;
    int nonzero;
    limbs[0] = source->hi >> 16;
    limbs[1] = source->hi & 0xfffful;
    limbs[2] = source->lo >> 16;
    limbs[3] = source->lo & 0xfffful;
    do {
        remainder = 0ul;
        nonzero = 0;
        for (limb = 0; limb < 4; ++limb) {
            current = (remainder << 16) | limbs[limb];
            limbs[limb] = current / 10ul;
            remainder = current % 10ul;
            if (limbs[limb] != 0ul) nonzero = 1;
        }
        reversed[count++] = (char)('0' + remainder);
    } while (nonzero && count < sizeof(reversed) - 1u);
    for (index = 0ul; index < count; ++index) {
        target[index] = reversed[count - index - 1ul];
    }
    target[count] = '\0';
}

/* ---- writer ---------------------------------------------------------- */

static void v9x_inv_begin(V9xInvWriter *writer, const char *type)
{
    writer->in_record = 0;
    if (writer->truncated) return;
    writer->record_start = writer->offset;
    if (!v9x_append_string(writer->buffer,
                           writer->capacity - writer->reserve,
                           &writer->offset, type) ||
        writer->offset + 2ul > writer->capacity - writer->reserve) {
        writer->offset = writer->record_start;
        writer->truncated = 1;
        return;
    }
    writer->count_offset = writer->offset;
    writer->offset += 2ul;
    writer->fields = 0ul;
    writer->in_record = 1;
}

static void v9x_inv_field(V9xInvWriter *writer, const char *key,
                          const char *value)
{
    char clean[V9X_INV_VALUE_MAX + 1];
    int length = 0;
    const unsigned char *source = (const unsigned char *)value;
    if (!writer->in_record) return;
    if (source != 0) {
        while (length < V9X_INV_VALUE_MAX && source[length] != 0u) {
            clean[length] = (source[length] < 0x20u || source[length] > 0x7eu) ?
                            '?' : (char)source[length];
            ++length;
        }
    }
    while (length > 0 && clean[length - 1] == ' ') --length;
    clean[length] = '\0';
    if (!v9x_append_string(writer->buffer,
                           writer->capacity - writer->reserve,
                           &writer->offset, key) ||
        !v9x_append_string(writer->buffer,
                           writer->capacity - writer->reserve,
                           &writer->offset, clean)) {
        writer->offset = writer->record_start;
        writer->in_record = 0;
        writer->truncated = 1;
        return;
    }
    ++writer->fields;
}

/* Skips empty values so records only carry what the machine reported. */
static void v9x_inv_field_opt(V9xInvWriter *writer, const char *key,
                              const char *value)
{
    const char *cursor = value;
    if (value == 0) return;
    while (*cursor == ' ') ++cursor;
    if (*cursor == '\0') return;
    v9x_inv_field(writer, key, cursor);
}

static void v9x_inv_field_u32(V9xInvWriter *writer, const char *key,
                              unsigned long value)
{
    char text[12];
    (void)v9x_inv_u32(text, value);
    v9x_inv_field(writer, key, text);
}

static void v9x_inv_field_u64(V9xInvWriter *writer, const char *key,
                              const V9xU64 *value)
{
    char text[24];
    v9x_u64_decimal(text, value);
    v9x_inv_field(writer, key, text);
}

static void v9x_inv_end(V9xInvWriter *writer)
{
    if (!writer->in_record) return;
    if (writer->fields == 0ul) {
        writer->offset = writer->record_start;
    } else {
        v9x_write_u16(writer->buffer + writer->count_offset,
                      (unsigned short)writer->fields);
        ++writer->records;
    }
    writer->in_record = 0;
}

/* ---- registry -------------------------------------------------------- */

/* Read a string (or DWORD, rendered as decimal) value. Returns 1 when a
   non-empty value was found. `target` always ends up NUL-terminated. */
static int v9x_reg_string(HKEY root, const char *path, const char *name,
                          char *target, DWORD capacity)
{
    HKEY key;
    DWORD type = 0ul;
    DWORD size = capacity - 1ul;
    unsigned char raw[V9X_INV_VALUE_MAX + 1];
    DWORD index;
    target[0] = '\0';
    if (RegOpenKeyExA(root, path, 0ul, KEY_READ, &key) != ERROR_SUCCESS) return 0;
    if (size > V9X_INV_VALUE_MAX) size = V9X_INV_VALUE_MAX;
    if (RegQueryValueExA(key, (LPSTR)name, 0, &type, raw, &size) != ERROR_SUCCESS) {
        (void)RegCloseKey(key);
        return 0;
    }
    (void)RegCloseKey(key);
    if (type == REG_SZ || type == REG_EXPAND_SZ) {
        for (index = 0ul; index < size && index < capacity - 1ul &&
                          raw[index] != 0u; ++index) {
            target[index] = (char)raw[index];
        }
        target[index] = '\0';
    } else if (type == REG_MULTI_SZ) {
        /* NT keeps e.g. SystemBiosVersion as several strings: join them. */
        DWORD at = 0ul;
        for (index = 0ul; index < size && at < capacity - 1ul; ++index) {
            if (raw[index] != 0u) {
                target[at++] = (char)raw[index];
            } else if (index + 1ul < size && raw[index + 1ul] != 0u) {
                if (at + 4ul >= capacity) break;
                if (at != 0ul) {
                    target[at++] = ' '; target[at++] = '/'; target[at++] = ' ';
                }
            } else {
                break;
            }
        }
        target[at] = '\0';
    } else if (type == REG_DWORD && size == 4ul) {
        (void)v9x_inv_u32(target, (unsigned long)raw[0] |
                                  ((unsigned long)raw[1] << 8) |
                                  ((unsigned long)raw[2] << 16) |
                                  ((unsigned long)raw[3] << 24));
    } else {
        return 0;
    }
    return target[0] != '\0';
}

static void v9x_inv_reg_field(V9xInvWriter *writer, const char *key,
                              HKEY root, const char *path, const char *name)
{
    char value[V9X_INV_VALUE_MAX + 1];
    if (v9x_reg_string(root, path, name, value, sizeof(value))) {
        v9x_inv_field_opt(writer, key, value);
    }
}

/* ---- records --------------------------------------------------------- */

static void v9x_inv_os(V9xInvWriter *writer)
{
    static const char current_9x[] = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion";
    static const char current_nt[] = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    const int nt = v9x_platform_is_nt();
    const char *current = nt ? current_nt : current_9x;
    OSVERSIONINFOA version;
    char text[24];
    unsigned long build;
    v9x_inv_begin(writer, "os");
    v9x_inv_reg_field(writer, "product_name", HKEY_LOCAL_MACHINE, current, "ProductName");
    if (nt) {
        /* "5.0.2195", the same shape as 9x's VersionNumber "4.10.2222". */
        char number[64];
        char build_text[24];
        if (v9x_reg_string(HKEY_LOCAL_MACHINE, current, "CurrentVersion", number,
                           sizeof(number)) &&
            v9x_reg_string(HKEY_LOCAL_MACHINE, current, "CurrentBuildNumber",
                           build_text, sizeof(build_text))) {
            lstrcatA(number, ".");
            lstrcatA(number, build_text);
            v9x_inv_field(writer, "version_number", number);
        }
        /* WinNT = Professional, ServerNT / LanmanNT = Server. */
        v9x_inv_reg_field(writer, "product_type", HKEY_LOCAL_MACHINE,
                          "SYSTEM\\CurrentControlSet\\Control\\ProductOptions",
                          "ProductType");
    } else {
        v9x_inv_reg_field(writer, "version_number", HKEY_LOCAL_MACHINE, current, "VersionNumber");
        v9x_inv_reg_field(writer, "sub_version", HKEY_LOCAL_MACHINE, current, "SubVersionNumber");
        v9x_inv_reg_field(writer, "plus_version", HKEY_LOCAL_MACHINE, current, "Plus! VersionNumber");
    }
    version.dwOSVersionInfoSize = sizeof(version);
    if (GetVersionExA(&version)) {
        v9x_inv_field_u32(writer, "major", version.dwMajorVersion);
        v9x_inv_field_u32(writer, "minor", version.dwMinorVersion);
        /* On 9x the high word of dwBuildNumber repeats major.minor. */
        build = version.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS ?
                (version.dwBuildNumber & 0xfffful) : version.dwBuildNumber;
        v9x_inv_field_u32(writer, "build", build);
        v9x_inv_field_u32(writer, "platform", version.dwPlatformId);
        v9x_inv_field_opt(writer, "csd", version.szCSDVersion);
    }
    v9x_inv_reg_field(writer, "registered_owner", HKEY_LOCAL_MACHINE, current, "RegisteredOwner");
    v9x_inv_reg_field(writer, "registered_org", HKEY_LOCAL_MACHINE, current, "RegisteredOrganization");
    v9x_inv_reg_field(writer, "directx", HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\DirectX", "Version");
    v9x_inv_reg_field(writer, "ie_version", HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Internet Explorer", "Version");
    (void)v9x_inv_u32(text, GetSystemMetrics(SM_CLEANBOOT));
    v9x_inv_field(writer, "clean_boot", text);
    v9x_inv_end(writer);
}

/* Measure the TSC rate against the performance counter for about 200 ms.
   All arithmetic stays within 32 bits: a 200 ms window of TSC ticks fits
   below 2^32 for any clock under ~20 GHz. */
static unsigned long v9x_inv_mhz(void)
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER now;
    unsigned long target;
    unsigned long elapsed;
    unsigned long micros;
    unsigned long tsc_start;
    unsigned long tsc_end;
    if (!QueryPerformanceFrequency(&frequency) || frequency.u.HighPart != 0ul ||
        frequency.u.LowPart < 1000ul) return 0ul;
    target = frequency.u.LowPart / 5ul;
    if (!QueryPerformanceCounter(&start)) return 0ul;
    tsc_start = v9x_rdtsc_low();
    do {
        if (!QueryPerformanceCounter(&now)) return 0ul;
        elapsed = now.u.LowPart - start.u.LowPart;
    } while (elapsed < target);
    tsc_end = v9x_rdtsc_low();
    if (frequency.u.LowPart <= 4000000ul) {
        micros = (elapsed / frequency.u.LowPart) * 1000000ul +
                 ((elapsed % frequency.u.LowPart) * 1000ul) /
                 (frequency.u.LowPart / 1000ul);
    } else {
        micros = elapsed / (frequency.u.LowPart / 1000000ul);
    }
    if (micros == 0ul) return 0ul;
    return ((tsc_end - tsc_start) + micros / 2ul) / micros;
}

static void v9x_inv_copy_regs(char *target, const unsigned long *regs,
                              int count)
{
    int reg;
    int byte;
    int offset = 0;
    for (reg = 0; reg < count; ++reg) {
        for (byte = 0; byte < 4; ++byte) {
            target[offset++] = (char)((regs[reg] >> (byte * 8)) & 0xfful);
        }
    }
    target[offset] = '\0';
}

static void v9x_inv_append(char *target, int *offset, int capacity,
                           const char *text)
{
    if (*offset != 0 && *offset < capacity - 1) target[(*offset)++] = ' ';
    while (*text != '\0' && *offset < capacity - 1) target[(*offset)++] = *text++;
    target[*offset] = '\0';
}

static void v9x_inv_cpu(V9xInvWriter *writer)
{
    static const char cpu_key[] = "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    SYSTEM_INFO info;
    unsigned long regs[4];
    unsigned long ordered[3];
    unsigned long brand_regs[12];
    unsigned long max_leaf;
    unsigned long max_ext = 0ul;
    unsigned long signature = 0ul;
    unsigned long features = 0ul;
    unsigned long features_ecx = 0ul;
    unsigned long ext_features = 0ul;
    unsigned long family;
    unsigned long model;
    char vendor[13];
    char brand[49];
    char flags[96];
    int flag_length = 0;
    int leaf;

    v9x_inv_begin(writer, "cpu");
    GetSystemInfo(&info);
    v9x_inv_field_u32(writer, "count", info.dwNumberOfProcessors);
    v9x_inv_field_u32(writer, "type", info.dwProcessorType);
    flags[0] = '\0';
    if (v9x_cpuid_supported()) {
        v9x_cpuid(0ul, regs);
        max_leaf = regs[0];
        ordered[0] = regs[1]; ordered[1] = regs[3]; ordered[2] = regs[2];
        v9x_inv_copy_regs(vendor, ordered, 3);
        v9x_inv_field_opt(writer, "vendor", vendor);
        if (max_leaf >= 1ul) {
            v9x_cpuid(1ul, regs);
            signature = regs[0];
            features_ecx = regs[2];
            features = regs[3];
        }
        v9x_cpuid(0x80000000ul, regs);
        if ((regs[0] & 0x80000000ul) != 0ul && regs[0] <= 0x800000fful) {
            max_ext = regs[0];
        }
        if (max_ext >= 0x80000001ul) {
            v9x_cpuid(0x80000001ul, regs);
            ext_features = regs[3];
        }
        if (max_ext >= 0x80000004ul) {
            for (leaf = 0; leaf < 3; ++leaf) {
                v9x_cpuid(0x80000002ul + (unsigned long)leaf, brand_regs + leaf * 4);
            }
            v9x_inv_copy_regs(brand, brand_regs, 12);
            v9x_inv_field_opt(writer, "brand", brand);
        }
        family = (signature >> 8) & 0xful;
        model = (signature >> 4) & 0xful;
        if (family == 0xful) family += (signature >> 20) & 0xfful;
        if (family >= 6ul) model |= ((signature >> 16) & 0xful) << 4;
        if (signature != 0ul) {
            v9x_inv_field_u32(writer, "family", family);
            v9x_inv_field_u32(writer, "model", model);
            v9x_inv_field_u32(writer, "stepping", signature & 0xful);
        }
        if (features & 0x00000001ul) v9x_inv_append(flags, &flag_length, sizeof(flags), "fpu");
        if (features & 0x00000010ul) v9x_inv_append(flags, &flag_length, sizeof(flags), "tsc");
        if (features & 0x00008000ul) v9x_inv_append(flags, &flag_length, sizeof(flags), "cmov");
        if (features & 0x00800000ul) v9x_inv_append(flags, &flag_length, sizeof(flags), "mmx");
        if (ext_features & 0x00400000ul) v9x_inv_append(flags, &flag_length, sizeof(flags), "mmxext");
        if (ext_features & 0x80000000ul) v9x_inv_append(flags, &flag_length, sizeof(flags), "3dnow");
        if (ext_features & 0x40000000ul) v9x_inv_append(flags, &flag_length, sizeof(flags), "3dnowext");
        if (features & 0x02000000ul) v9x_inv_append(flags, &flag_length, sizeof(flags), "sse");
        if (features & 0x04000000ul) v9x_inv_append(flags, &flag_length, sizeof(flags), "sse2");
        if (features_ecx & 0x00000001ul) v9x_inv_append(flags, &flag_length, sizeof(flags), "sse3");
        v9x_inv_field_opt(writer, "features", flags);
        if (features & 0x00000010ul) {
            v9x_inv_field_u32(writer, "mhz", v9x_inv_mhz());
        }
    } else {
        v9x_inv_field(writer, "vendor", "unknown");
    }
    v9x_inv_reg_field(writer, "reg_vendor", HKEY_LOCAL_MACHINE, cpu_key, "VendorIdentifier");
    v9x_inv_reg_field(writer, "reg_identifier", HKEY_LOCAL_MACHINE, cpu_key, "Identifier");
    v9x_inv_end(writer);
}

static void v9x_inv_memory(V9xInvWriter *writer)
{
    MEMORYSTATUS status;
    status.dwLength = sizeof(status);
    GlobalMemoryStatus(&status);
    v9x_inv_begin(writer, "memory");
    v9x_inv_field_u32(writer, "total_phys", status.dwTotalPhys);
    v9x_inv_field_u32(writer, "avail_phys", status.dwAvailPhys);
    v9x_inv_field_u32(writer, "total_page", status.dwTotalPageFile);
    v9x_inv_field_u32(writer, "avail_page", status.dwAvailPageFile);
    v9x_inv_field_u32(writer, "load", status.dwMemoryLoad);
    v9x_inv_end(writer);
}

static const char *v9x_inv_drive_type(UINT type)
{
    switch (type) {
    case DRIVE_REMOVABLE: return "removable";
    case DRIVE_FIXED: return "fixed";
    case DRIVE_REMOTE: return "network";
    case DRIVE_CDROM: return "cdrom";
    case DRIVE_RAMDISK: return "ramdisk";
    default: return "unknown";
    }
}

static void v9x_inv_drives(V9xInvWriter *writer)
{
    V9xDiskFreeExFn disk_free_ex = 0;
    HMODULE kernel;
    DWORD mask;
    UINT old_mode;
    UINT type;
    int letter;
    char root[4];
    char letter_text[2];
    char label[64];
    char file_system[32];
    char serial_text[12];
    DWORD serial;
    DWORD max_component;
    DWORD fs_flags;
    DWORD sectors_per_cluster;
    DWORD bytes_per_sector;
    DWORD free_clusters;
    DWORD total_clusters;
    V9xU64 available;
    V9xU64 total;
    V9xU64 total_free;

    kernel = GetModuleHandleA("KERNEL32.DLL");
    if (kernel != 0) {
        disk_free_ex = (V9xDiskFreeExFn)GetProcAddress(kernel, "GetDiskFreeSpaceExA");
    }
    old_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    mask = GetLogicalDrives();
    for (letter = 0; letter < 26; ++letter) {
        if ((mask & (1ul << letter)) == 0ul) continue;
        root[0] = (char)('A' + letter); root[1] = ':'; root[2] = '\\'; root[3] = '\0';
        letter_text[0] = root[0]; letter_text[1] = '\0';
        type = GetDriveTypeA(root);
        v9x_inv_begin(writer, "drive");
        v9x_inv_field(writer, "letter", letter_text);
        v9x_inv_field(writer, "type", v9x_inv_drive_type(type));
        /* Only local fixed media are probed: floppies and empty CD drives can
           stall or prompt, and network drives can hang on a dead server. */
        if (type == DRIVE_FIXED || type == DRIVE_RAMDISK) {
            label[0] = '\0';
            file_system[0] = '\0';
            if (GetVolumeInformationA(root, label, sizeof(label), &serial,
                                      &max_component, &fs_flags,
                                      file_system, sizeof(file_system))) {
                v9x_inv_field_opt(writer, "label", label);
                v9x_inv_field_opt(writer, "fs", file_system);
                v9x_inv_hex32(serial_text, serial, 1);
                v9x_inv_field(writer, "serial", serial_text);
            }
            if (disk_free_ex != 0 &&
                disk_free_ex(root, &available, &total, &total_free)) {
                v9x_inv_field_u64(writer, "total", &total);
                v9x_inv_field_u64(writer, "free", &available);
            } else if (GetDiskFreeSpaceA(root, &sectors_per_cluster,
                                         &bytes_per_sector, &free_clusters,
                                         &total_clusters)) {
                v9x_u64_set(&total, total_clusters);
                v9x_u64_mul32(&total, sectors_per_cluster);
                v9x_u64_mul32(&total, bytes_per_sector);
                v9x_u64_set(&available, free_clusters);
                v9x_u64_mul32(&available, sectors_per_cluster);
                v9x_u64_mul32(&available, bytes_per_sector);
                v9x_inv_field_u64(writer, "total", &total);
                v9x_inv_field_u64(writer, "free", &available);
            }
        }
        v9x_inv_end(writer);
    }
    (void)SetErrorMode(old_mode);
}

static void v9x_inv_network(V9xInvWriter *writer, const V9xAgentState *machine)
{
    char host_name[128];
    struct hostent *host;
    struct in_addr address;
    char *text;
    int index;
    v9x_inv_begin(writer, "net");
    if (machine->winsock_version != 0u &&
        gethostname(host_name, sizeof(host_name)) == 0) {
        v9x_inv_field_opt(writer, "hostname", host_name);
        host = gethostbyname(host_name);
        if (host != 0 && host->h_addrtype == AF_INET &&
            host->h_length == sizeof(address) && host->h_addr_list != 0) {
            for (index = 0; index < 8 && host->h_addr_list[index] != 0; ++index) {
                address.s_addr = *(unsigned long *)host->h_addr_list[index];
                text = inet_ntoa(address);
                if (text != 0) v9x_inv_field(writer, "ip", text);
            }
        }
    }
    v9x_inv_end(writer);
}

static void v9x_inv_bios(V9xInvWriter *writer)
{
    static const char bios[] = "Enum\\Root\\*PNP0C01\\0000";
    static const char bios_nt[] = "HARDWARE\\DESCRIPTION\\System";
    v9x_inv_begin(writer, "bios");
    if (v9x_platform_is_nt()) {
        /* NT has no BIOS name; it does keep the video BIOS banner. */
        v9x_inv_reg_field(writer, "date", HKEY_LOCAL_MACHINE, bios_nt, "SystemBiosDate");
        v9x_inv_reg_field(writer, "version", HKEY_LOCAL_MACHINE, bios_nt, "SystemBiosVersion");
        v9x_inv_reg_field(writer, "video_version", HKEY_LOCAL_MACHINE, bios_nt,
                          "VideoBiosVersion");
        v9x_inv_end(writer);
        return;
    }
    v9x_inv_reg_field(writer, "date", HKEY_LOCAL_MACHINE, bios, "BIOSDate");
    v9x_inv_reg_field(writer, "name", HKEY_LOCAL_MACHINE, bios, "BIOSName");
    v9x_inv_reg_field(writer, "version", HKEY_LOCAL_MACHINE, bios, "BIOSVersion");
    v9x_inv_end(writer);
}

static void v9x_inv_join(char *target, const char *first, const char *second,
                         const char *third, int capacity)
{
    int offset = 0;
    while (*first != '\0' && offset < capacity - 1) target[offset++] = *first++;
    while (*second != '\0' && offset < capacity - 1) target[offset++] = *second++;
    while (*third != '\0' && offset < capacity - 1) target[offset++] = *third++;
    target[offset] = '\0';
}

/* Device classes reported, in output order; the System class (motherboard
   resources) is left out as noise. */
static const char *const v9x_inv_classes[] = {
    "Display", "Monitor", "MEDIA", "Net", "hdc", "SCSIAdapter", "CDROM",
    "fdc", "DiskDrive", "Modem", "Ports", "USB", "Mouse", "Keyboard",
    "Printer", "Image"
};
#define V9X_INV_CLASS_COUNT (sizeof(v9x_inv_classes) / sizeof(v9x_inv_classes[0]))
#define V9X_INV_MAX_LIVE 160u

static const char v9x_inv_class_base[] = "System\\CurrentControlSet\\Services\\Class\\";
static const char v9x_inv_live_base[] = "Config Manager\\Enum\\";
static const char v9x_inv_nt_enum[] = "SYSTEM\\CurrentControlSet\\Enum";
static const char v9x_inv_nt_class_base[] = "SYSTEM\\CurrentControlSet\\Control\\Class\\";

/* An NT Enum path is "SYSTEM\CurrentControlSet\Enum\" plus a device
   instance ID of up to 200 characters. */
#define V9X_INV_NT_PATH 256

/* One loaded device node: its class slot and its HKEY_DYN_DATA key name.
   On NT the node is instead the enumeration index of each level of
   Enum\<bus>\<device>\<instance>, and the problem code is kept from the
   scan (NT does not store it in the registry). */
typedef struct V9xLiveDevice {
    unsigned char class_index;
    unsigned char has_problem;
    char node[15];
    unsigned short nt_index[3];
    DWORD problem;
} V9xLiveDevice;

/* The devices Windows has loaded right now, from the Configuration
   Manager's live tree, plus the active display adapter taken from it. */
typedef struct V9xDeviceScan {
    V9xLiveDevice devices[V9X_INV_MAX_LIVE];
    unsigned int count;
    int live;
    char display_desc[V9X_INV_VALUE_MAX + 1];
    char display_provider[V9X_INV_VALUE_MAX + 1];
} V9xDeviceScan;

static int v9x_inv_class_index(const char *name)
{
    unsigned int index;
    for (index = 0u; index < V9X_INV_CLASS_COUNT; ++index) {
        if (lstrcmpiA(name, v9x_inv_classes[index]) == 0) return (int)index;
    }
    return -1;
}

/* A DWORD value; 9x stores some (Problem, Status) as 4-byte REG_BINARY. */
static int v9x_reg_dword(HKEY root, const char *path, const char *name,
                         DWORD *value)
{
    HKEY key;
    DWORD type = 0ul;
    unsigned char raw[4];
    DWORD size = sizeof(raw);
    LONG result;
    if (RegOpenKeyExA(root, path, 0ul, KEY_READ, &key) != ERROR_SUCCESS) return 0;
    result = RegQueryValueExA(key, (LPSTR)name, 0, &type, raw, &size);
    (void)RegCloseKey(key);
    if (result != ERROR_SUCCESS || size != 4ul ||
        (type != REG_DWORD && type != REG_BINARY)) return 0;
    *value = (DWORD)raw[0] | ((DWORD)raw[1] << 8) | ((DWORD)raw[2] << 16) |
             ((DWORD)raw[3] << 24);
    return 1;
}

/* Resolve a live node to its HKLM\Enum key: `enum_path` gets "Enum\<hwkey>".
   Returns the class slot, or -1 when the node is not a reported class. */
static int v9x_inv_node_enum(const char *node, char *enum_path, int capacity)
{
    char node_path[64];
    char hardware_key[200];
    char class_name[64];
    v9x_inv_join(node_path, v9x_inv_live_base, node, "", sizeof(node_path));
    if (!v9x_reg_string(HKEY_DYN_DATA, node_path, "HardWareKey", hardware_key,
                        sizeof(hardware_key))) return -1;
    v9x_inv_join(enum_path, "Enum\\", hardware_key, "", capacity);
    if (!v9x_reg_string(HKEY_LOCAL_MACHINE, enum_path, "Class", class_name,
                        sizeof(class_name))) return -1;
    return v9x_inv_class_index(class_name);
}

/* Description and provider for a device: the driver key's DriverDesc and
   ProviderName when it has a driver, else the Enum key's DeviceDesc/Mfg. */
static void v9x_inv_node_names(const char *enum_path, char *driver_path,
                               char *description, char *provider)
{
    char driver[64];
    driver_path[0] = '\0';
    description[0] = '\0';
    provider[0] = '\0';
    if (v9x_reg_string(HKEY_LOCAL_MACHINE, enum_path, "Driver", driver,
                       sizeof(driver))) {
        v9x_inv_join(driver_path,
                     v9x_platform_is_nt() ? v9x_inv_nt_class_base : v9x_inv_class_base,
                     driver, "", 128);
        (void)v9x_reg_string(HKEY_LOCAL_MACHINE, driver_path, "DriverDesc",
                             description, V9X_INV_VALUE_MAX + 1);
        (void)v9x_reg_string(HKEY_LOCAL_MACHINE, driver_path, "ProviderName",
                             provider, V9X_INV_VALUE_MAX + 1);
    }
    if (description[0] == '\0') {
        (void)v9x_reg_string(HKEY_LOCAL_MACHINE, enum_path, "DeviceDesc",
                             description, V9X_INV_VALUE_MAX + 1);
    }
    if (provider[0] == '\0') {
        (void)v9x_reg_string(HKEY_LOCAL_MACHINE, enum_path, "Mfg", provider,
                             V9X_INV_VALUE_MAX + 1);
    }
}

/* Walk HKEY_DYN_DATA\Config Manager\Enum: one subkey per device node that
   is loaded now. HKLM\...\Services\Class also keeps every device the machine
   has ever had, so reading it alone lists stale adapters and drives and can
   name the wrong display adapter. */
static void v9x_inv_scan_devices(V9xDeviceScan *scan)
{
    HKEY live_key;
    DWORD index;
    DWORD node_length;
    DWORD problem;
    FILETIME written;
    char node[16];
    char node_path[64];
    char enum_path[220];
    char driver_path[128];
    int class_index;
    scan->count = 0u;
    scan->live = 0;
    scan->display_desc[0] = '\0';
    scan->display_provider[0] = '\0';
    if (RegOpenKeyExA(HKEY_DYN_DATA, "Config Manager\\Enum", 0ul, KEY_READ,
                      &live_key) != ERROR_SUCCESS) return;
    scan->live = 1;
    for (index = 0ul; scan->count < V9X_INV_MAX_LIVE; ++index) {
        node_length = sizeof(node);
        if (RegEnumKeyExA(live_key, index, node, &node_length, 0, 0, 0,
                          &written) != ERROR_SUCCESS) break;
        class_index = v9x_inv_node_enum(node, enum_path, sizeof(enum_path));
        if (class_index < 0) continue;
        scan->devices[scan->count].class_index = (unsigned char)class_index;
        lstrcpynA(scan->devices[scan->count].node, node,
                  sizeof(scan->devices[scan->count].node));
        ++scan->count;
        /* The active display adapter: the first working Display node. */
        if (class_index == 0 && scan->display_desc[0] == '\0') {
            v9x_inv_join(node_path, v9x_inv_live_base, node, "", sizeof(node_path));
            if (!v9x_reg_dword(HKEY_DYN_DATA, node_path, "Problem", &problem) ||
                problem == 0ul) {
                v9x_inv_node_names(enum_path, driver_path, scan->display_desc,
                                   scan->display_provider);
            }
        }
    }
    (void)RegCloseKey(live_key);
    if (scan->count == 0u) scan->live = 0;
}

static void v9x_inv_display(V9xInvWriter *writer, const V9xDeviceScan *scan)
{
    static const char adapter[] = "System\\CurrentControlSet\\Services\\Class\\Display\\0000";
    v9x_inv_begin(writer, "display");
    if (scan->display_desc[0] != '\0') {
        v9x_inv_field(writer, "desc", scan->display_desc);
        v9x_inv_field_opt(writer, "provider", scan->display_provider);
    } else {
        /* No live tree: the first installed adapter is a guess (9x only;
           NT has no Services\Class and names it from its own scan). */
        if (!v9x_platform_is_nt()) {
            v9x_inv_reg_field(writer, "desc", HKEY_LOCAL_MACHINE, adapter, "DriverDesc");
            v9x_inv_reg_field(writer, "provider", HKEY_LOCAL_MACHINE, adapter, "ProviderName");
        }
    }
    v9x_inv_field_u32(writer, "width", (unsigned long)GetSystemMetrics(SM_CXSCREEN));
    v9x_inv_field_u32(writer, "height", (unsigned long)GetSystemMetrics(SM_CYSCREEN));
    if (v9x_platform_is_nt()) {
        /* No HKCC\Display\Settings on NT. USER32 reports the current mode
           without GDI; resolved at run time to keep the 9x import table. */
        V9xEnumDisplaySettingsFn enum_settings = (V9xEnumDisplaySettingsFn)
            GetProcAddress(GetModuleHandleA("USER32.DLL"), "EnumDisplaySettingsA");
        DEVMODEA mode;
        unsigned long index;
        for (index = 0ul; index < sizeof(mode); ++index) ((char *)&mode)[index] = 0;
        mode.dmSize = sizeof(mode);
        if (enum_settings != 0 &&
            enum_settings(0, V9X_ENUM_CURRENT_SETTINGS, &mode)) {
            v9x_inv_field_u32(writer, "bpp", mode.dmBitsPerPel);
            if (mode.dmDisplayFrequency > 1ul) {
                v9x_inv_field_u32(writer, "refresh", mode.dmDisplayFrequency);
            }
        }
    } else {
        v9x_inv_reg_field(writer, "bpp", HKEY_CURRENT_CONFIG, "Display\\Settings", "BitsPerPixel");
    }
    v9x_inv_end(writer);
}

static void v9x_inv_device_record(V9xInvWriter *writer, const char *class_name,
                                  const char *description, const char *provider,
                                  const char *driver_date, const char *hardware_id,
                                  DWORD problem, int has_problem)
{
    v9x_inv_begin(writer, "device");
    v9x_inv_field(writer, "class", class_name);
    v9x_inv_field(writer, "desc", description);
    v9x_inv_field_opt(writer, "provider", provider);
    v9x_inv_field_opt(writer, "driver_date", driver_date);
    if (hardware_id != 0) v9x_inv_field_opt(writer, "id", hardware_id);
    /* Configuration Manager problem code, e.g. 22 = disabled, 28 = no driver. */
    if (has_problem && problem != 0ul) v9x_inv_field_u32(writer, "problem", problem);
    v9x_inv_end(writer);
}

/* Loaded devices from the live scan, grouped in class order. */
static void v9x_inv_devices_live(V9xInvWriter *writer, const V9xDeviceScan *scan)
{
    char node_path[64];
    char enum_path[220];
    char driver_path[128];
    char description[V9X_INV_VALUE_MAX + 1];
    char provider[V9X_INV_VALUE_MAX + 1];
    char driver_date[64];
    DWORD problem;
    int has_problem;
    unsigned int class_index;
    unsigned int index;
    for (class_index = 0u; class_index < V9X_INV_CLASS_COUNT; ++class_index) {
        for (index = 0u; index < scan->count; ++index) {
            if (scan->devices[index].class_index != class_index) continue;
            if (v9x_inv_node_enum(scan->devices[index].node, enum_path,
                                  sizeof(enum_path)) != (int)class_index) continue;
            v9x_inv_node_names(enum_path, driver_path, description, provider);
            if (description[0] == '\0') continue;
            driver_date[0] = '\0';
            if (driver_path[0] != '\0') {
                (void)v9x_reg_string(HKEY_LOCAL_MACHINE, driver_path, "DriverDate",
                                     driver_date, sizeof(driver_date));
            }
            v9x_inv_join(node_path, v9x_inv_live_base, scan->devices[index].node, "",
                         sizeof(node_path));
            has_problem = v9x_reg_dword(HKEY_DYN_DATA, node_path, "Problem", &problem);
            v9x_inv_device_record(writer, v9x_inv_classes[class_index], description,
                                  provider, driver_date, enum_path + 5, problem,
                                  has_problem);
        }
    }
}

/* ---- NT devices ------------------------------------------------------ */

/* Device instance ID "<bus>\<device>\<instance>" from the enumeration
   index of each level under HKLM\SYSTEM\CurrentControlSet\Enum. Returns
   the length, or 0 when an index no longer resolves. */
static int v9x_inv_nt_id(const unsigned short *at, char *id, int capacity)
{
    HKEY key;
    HKEY next;
    DWORD length;
    FILETIME written;
    char name[V9X_INV_NT_PATH];
    int offset = 0;
    int level;
    int index;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, v9x_inv_nt_enum, 0ul, KEY_READ,
                      &key) != ERROR_SUCCESS) return 0;
    for (level = 0; level < 3; ++level) {
        length = sizeof(name);
        if (RegEnumKeyExA(key, at[level], name, &length, 0, 0, 0,
                          &written) != ERROR_SUCCESS) {
            (void)RegCloseKey(key);
            return 0;
        }
        if (level != 0 && offset < capacity - 1) id[offset++] = '\\';
        for (index = 0; name[index] != '\0' && offset < capacity - 1; ++index) {
            id[offset++] = name[index];
        }
        if (level < 2) {
            if (RegOpenKeyExA(key, name, 0ul, KEY_READ, &next) != ERROR_SUCCESS) {
                (void)RegCloseKey(key);
                return 0;
            }
            (void)RegCloseKey(key);
            key = next;
        }
    }
    (void)RegCloseKey(key);
    id[offset] = '\0';
    return offset;
}

/* Class slot of an NT device instance, -1 when it is not a reported class.
   `enum_path` gets its full HKLM path. Windows 2000 leaves the Class name
   off many instances (disks, keyboard, mouse) and keeps only ClassGUID, so
   fall back to the class key's own Class value. */
static int v9x_inv_nt_class(const char *id, char *enum_path)
{
    char class_name[64];
    char class_guid[64];
    char class_path[128];
    v9x_inv_join(enum_path, v9x_inv_nt_enum, "\\", id, V9X_INV_NT_PATH);
    if (!v9x_reg_string(HKEY_LOCAL_MACHINE, enum_path, "Class", class_name,
                        sizeof(class_name))) {
        if (!v9x_reg_string(HKEY_LOCAL_MACHINE, enum_path, "ClassGUID", class_guid,
                            sizeof(class_guid))) return -1;
        v9x_inv_join(class_path, v9x_inv_nt_class_base, class_guid, "",
                     sizeof(class_path));
        if (!v9x_reg_string(HKEY_LOCAL_MACHINE, class_path, "Class", class_name,
                            sizeof(class_name))) return -1;
    }
    return v9x_inv_class_index(class_name);
}

/* Walk Enum\<bus>\<device>\<instance>. NT keeps every device the machine
   has ever seen there, so presence and the problem code come from the
   Configuration Manager: CM_Locate_DevNodeA only finds devices that are
   present now. Without CFGMGR32 every instance is listed (scan->live = 0). */
static void v9x_inv_scan_devices_nt(V9xDeviceScan *scan)
{
    HMODULE cfgmgr;
    V9xLocateDevNodeFn locate = 0;
    V9xDevNodeStatusFn status_of = 0;
    HKEY enum_key;
    HKEY bus_key;
    HKEY device_key;
    DWORD length;
    FILETIME written;
    char bus[64];
    char device[V9X_INV_NT_PATH];
    char instance[V9X_INV_NT_PATH];
    char id[V9X_INV_NT_PATH];
    char enum_path[V9X_INV_NT_PATH];
    char driver_path[128];
    DWORD bus_index;
    DWORD device_index;
    DWORD instance_index;
    DWORD node;
    ULONG status;
    ULONG problem;
    int class_index;
    V9xLiveDevice *entry;

    scan->count = 0u;
    scan->live = 0;
    scan->display_desc[0] = '\0';
    scan->display_provider[0] = '\0';
    cfgmgr = LoadLibraryA("CFGMGR32.DLL");
    if (cfgmgr != 0) {
        locate = (V9xLocateDevNodeFn)GetProcAddress(cfgmgr, "CM_Locate_DevNodeA");
        status_of = (V9xDevNodeStatusFn)GetProcAddress(cfgmgr, "CM_Get_DevNode_Status");
        if (locate != 0 && status_of != 0) scan->live = 1;
    }
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, v9x_inv_nt_enum, 0ul, KEY_READ,
                      &enum_key) != ERROR_SUCCESS) {
        if (cfgmgr != 0) (void)FreeLibrary(cfgmgr);
        scan->live = 0;
        return;
    }
    for (bus_index = 0ul; bus_index < 0xfffful; ++bus_index) {
        length = sizeof(bus);
        if (RegEnumKeyExA(enum_key, bus_index, bus, &length, 0, 0, 0,
                          &written) != ERROR_SUCCESS) break;
        if (RegOpenKeyExA(enum_key, bus, 0ul, KEY_READ, &bus_key) != ERROR_SUCCESS) {
            continue;
        }
        for (device_index = 0ul; device_index < 0xfffful; ++device_index) {
            length = sizeof(device);
            if (RegEnumKeyExA(bus_key, device_index, device, &length, 0, 0, 0,
                              &written) != ERROR_SUCCESS) break;
            if (RegOpenKeyExA(bus_key, device, 0ul, KEY_READ,
                              &device_key) != ERROR_SUCCESS) continue;
            for (instance_index = 0ul;
                 instance_index < 0xfffful && scan->count < V9X_INV_MAX_LIVE;
                 ++instance_index) {
                length = sizeof(instance);
                if (RegEnumKeyExA(device_key, instance_index, instance, &length,
                                  0, 0, 0, &written) != ERROR_SUCCESS) break;
                v9x_inv_join(id, bus, "\\", device, sizeof(id));
                v9x_inv_join(id, id, "\\", instance, sizeof(id));
                class_index = v9x_inv_nt_class(id, enum_path);
                if (class_index < 0) continue;
                status = 0ul;
                problem = 0ul;
                if (scan->live) {
                    if (locate(&node, id, 0ul) != 0ul) continue;  /* not present */
                    if (status_of(&status, &problem, node, 0ul) != 0ul) {
                        status = 0ul;
                        problem = 0ul;
                    }
                    if ((status & V9X_DN_HAS_PROBLEM) == 0ul) problem = 0ul;
                }
                entry = &scan->devices[scan->count++];
                entry->class_index = (unsigned char)class_index;
                entry->has_problem = (unsigned char)(scan->live ? 1 : 0);
                entry->node[0] = '\0';
                entry->nt_index[0] = (unsigned short)bus_index;
                entry->nt_index[1] = (unsigned short)device_index;
                entry->nt_index[2] = (unsigned short)instance_index;
                entry->problem = problem;
                /* The active display adapter: the first working Display node. */
                if (class_index == 0 && scan->live && problem == 0ul &&
                    scan->display_desc[0] == '\0') {
                    v9x_inv_node_names(enum_path, driver_path, scan->display_desc,
                                       scan->display_provider);
                }
            }
            (void)RegCloseKey(device_key);
        }
        (void)RegCloseKey(bus_key);
    }
    (void)RegCloseKey(enum_key);
    if (cfgmgr != 0) (void)FreeLibrary(cfgmgr);
}

/* NT devices from the scan, grouped in class order like the 9x live list. */
static void v9x_inv_devices_nt(V9xInvWriter *writer, const V9xDeviceScan *scan)
{
    char id[V9X_INV_NT_PATH];
    char enum_path[V9X_INV_NT_PATH];
    char driver_path[128];
    char description[V9X_INV_VALUE_MAX + 1];
    char provider[V9X_INV_VALUE_MAX + 1];
    char driver_date[64];
    unsigned int class_index;
    unsigned int index;
    for (class_index = 0u; class_index < V9X_INV_CLASS_COUNT; ++class_index) {
        for (index = 0u; index < scan->count; ++index) {
            if (scan->devices[index].class_index != class_index) continue;
            if (v9x_inv_nt_id(scan->devices[index].nt_index, id, sizeof(id)) == 0 ||
                v9x_inv_nt_class(id, enum_path) != (int)class_index) continue;
            v9x_inv_node_names(enum_path, driver_path, description, provider);
            if (description[0] == '\0') continue;
            driver_date[0] = '\0';
            if (driver_path[0] != '\0') {
                (void)v9x_reg_string(HKEY_LOCAL_MACHINE, driver_path, "DriverDate",
                                     driver_date, sizeof(driver_date));
            }
            v9x_inv_device_record(writer, v9x_inv_classes[class_index], description,
                                  provider, driver_date, id,
                                  scan->devices[index].problem,
                                  scan->devices[index].has_problem);
        }
    }
}

/* Fallback without a live tree: every installed driver instance, including
   devices that are no longer present. */
static void v9x_inv_devices_registry(V9xInvWriter *writer)
{
    char class_path[96];
    char instance_path[128];
    char instance[32];
    char description[V9X_INV_VALUE_MAX + 1];
    char provider[V9X_INV_VALUE_MAX + 1];
    char driver_date[64];
    HKEY class_key;
    DWORD index;
    DWORD instance_length;
    FILETIME written;
    unsigned int class_index;
    for (class_index = 0u; class_index < V9X_INV_CLASS_COUNT; ++class_index) {
        v9x_inv_join(class_path, v9x_inv_class_base, v9x_inv_classes[class_index], "",
                     sizeof(class_path));
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, class_path, 0ul, KEY_READ,
                          &class_key) != ERROR_SUCCESS) continue;
        for (index = 0ul; index < V9X_INV_MAX_DEVICES_PER_CLASS; ++index) {
            instance_length = sizeof(instance);
            if (RegEnumKeyExA(class_key, index, instance, &instance_length, 0,
                              0, 0, &written) != ERROR_SUCCESS) break;
            v9x_inv_join(instance_path, class_path, "\\", instance,
                         sizeof(instance_path));
            if (!v9x_reg_string(HKEY_LOCAL_MACHINE, instance_path, "DriverDesc",
                                description, sizeof(description))) continue;
            (void)v9x_reg_string(HKEY_LOCAL_MACHINE, instance_path, "ProviderName",
                                 provider, sizeof(provider));
            (void)v9x_reg_string(HKEY_LOCAL_MACHINE, instance_path, "DriverDate",
                                 driver_date, sizeof(driver_date));
            v9x_inv_device_record(writer, v9x_inv_classes[class_index], description,
                                  provider, driver_date, 0, 0ul, 0);
        }
        (void)RegCloseKey(class_key);
    }
}

int v9x_handle_inventory(V9xConnection *conn, unsigned long request_id)
{
    V9xInvWriter writer;
    V9xDeviceScan scan;
    char detail[32];
    writer.buffer = conn->file_response;
    writer.capacity = sizeof(conn->file_response);
    writer.offset = 2ul;
    writer.records = 0ul;
    writer.record_start = 2ul;
    writer.count_offset = 0ul;
    writer.fields = 0ul;
    writer.reserve = V9X_INV_META_RESERVE;
    writer.in_record = 0;
    writer.truncated = 0;

    v9x_inv_os(&writer);
    v9x_inv_cpu(&writer);
    v9x_inv_memory(&writer);
    v9x_inv_drives(&writer);
    if (v9x_platform_is_nt()) {
        v9x_inv_scan_devices_nt(&scan);
    } else {
        v9x_inv_scan_devices(&scan);
    }
    v9x_inv_display(&writer, &scan);
    v9x_inv_network(&writer, conn->machine);
    v9x_inv_bios(&writer);
    if (v9x_platform_is_nt()) {
        v9x_inv_devices_nt(&writer, &scan);
    } else if (scan.live) {
        v9x_inv_devices_live(&writer, &scan);
    } else {
        v9x_inv_devices_registry(&writer);
    }
    /* Always last. The reserve kept back by every append guarantees it fits,
       whether or not the device list overflowed. */
    detail[0] = (char)(writer.truncated ? '1' : '0');
    detail[1] = '\0';
    writer.truncated = 0;
    writer.reserve = 0ul;
    v9x_inv_begin(&writer, "meta");
    v9x_inv_field(&writer, "devices", scan.live ? "live" : "registry");
    if (detail[0] == '1') v9x_inv_field(&writer, "truncated", "1");
    v9x_inv_field_opt(&writer, "install_id", conn->machine->install_id);
    v9x_inv_field(&writer, "discovery",
                  !conn->machine->discovery ? "off" :
                  v9x_flag_read(&conn->machine->discovery_status) ==
                      V9X_DISCOVERY_ACTIVE ? "on" : "unavailable");
    v9x_inv_end(&writer);
    v9x_write_u16(writer.buffer, (unsigned short)writer.records);
    detail[0] = 'n'; detail[1] = '=';
    (void)v9x_inv_u32(detail + 2, writer.records);
    v9x_log_event("inventory", detail);
    return v9x_send_frame(conn, V9X_MSG_INVENTORY_RESPONSE, request_id,
                          writer.buffer, writer.offset);
}
