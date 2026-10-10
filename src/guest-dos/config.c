/* Configuration, boot counter, monotonic clock, and the audit log for the DOS
   agent. The Win9x agent read these from the registry / GetPrivateProfile* and
   used GetTickCount(); here they come from a small INI reader, a BOOT.DAT
   counter file, and clock(). All state lives under C:\V9XREMOT (8.3-safe). */

#include <ctype.h>
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dosagent.h"

static unsigned long v9x_log_boot = 0ul;

/* Trim leading blanks and any trailing CR/LF/blanks in place. */
static char *v9x_trim(char *text)
{
    char *end;
    while (*text == ' ' || *text == '\t') ++text;
    end = text + strlen(text);
    while (end > text &&
           (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' ||
            end[-1] == '\t')) {
        --end;
    }
    *end = '\0';
    return text;
}

static int v9x_key_matches(const char *line, const char *key)
{
    unsigned long index = 0ul;
    while (key[index] != '\0') {
        if (tolower((unsigned char)line[index]) !=
            tolower((unsigned char)key[index])) return 0;
        ++index;
    }
    return line[index] == '=' || line[index] == ' ' ||
           line[index] == '\t';
}

static const char *v9x_value_of(const char *line)
{
    const char *equals = strchr(line, '=');
    if (equals == 0) return "";
    ++equals;
    while (*equals == ' ' || *equals == '\t') ++equals;
    return equals;
}

void v9x_dos_load_config(V9xDos *dos)
{
    FILE *file;
    char line[160];
    dos->listen_port = V9X_DEFAULT_PORT;
    dos->allowed_client[0] = '\0';
    file = fopen(V9X_DOS_CONFIG, "r");
    if (file == 0) return;
    while (fgets(line, sizeof(line), file) != 0) {
        char *trimmed = v9x_trim(line);
        if (trimmed[0] == '\0' || trimmed[0] == ';' || trimmed[0] == '#' ||
            trimmed[0] == '[') continue;
        if (v9x_key_matches(trimmed, "port")) {
            int port = atoi(v9x_value_of(trimmed));
            if (port > 0 && port < 65536) dos->listen_port = (unsigned short)port;
        } else if (v9x_key_matches(trimmed, "allowed_client")) {
            const char *value = v9x_value_of(trimmed);
            unsigned long i = 0ul;
            while (value[i] != '\0' && i + 1ul < sizeof(dos->allowed_client)) {
                dos->allowed_client[i] = value[i];
                ++i;
            }
            dos->allowed_client[i] = '\0';
        } else if (v9x_key_matches(trimmed, "install_id")) {
            const char *value = v9x_value_of(trimmed);
            if (v9x_id_valid(value)) {
                unsigned long i;
                for (i = 0ul; i < V9X_INSTALL_ID_SIZE; ++i) {
                    dos->install_id[i] = (char)tolower((unsigned char)value[i]);
                }
            }
        }
    }
    fclose(file);
}

/* DOS has no random source either. The BIOS tick count, clock(), the time of
   day, the boot counter and the stack address differ between machines and
   boots; the 0040:006C tick is readable directly under DOS/4GW's flat map of
   the first megabyte. */
static void v9x_dos_generate_id(V9xDos *dos)
{
    V9xIdPool pool;
    volatile unsigned long *bios_ticks = (volatile unsigned long *)0x46cul;
    int round;
    v9x_id_begin(&pool);
    v9x_id_mix(&pool, (unsigned long)time(0));
    v9x_id_mix(&pool, dos->boot_counter);
    v9x_id_mix(&pool, (unsigned long)&pool);
    for (round = 0; round < 8; ++round) {
        v9x_id_mix(&pool, *bios_ticks);
        v9x_id_mix(&pool, (unsigned long)clock());
    }
    v9x_id_finish(&pool, dos->install_id);
}

/* Called after v9x_dos_load_config: keeps the configured ID, or generates one
   and appends it to AGENT.INI so it survives the next boot. */
void v9x_dos_ensure_install_id(V9xDos *dos)
{
    FILE *file;
    int existed;
    if (v9x_id_valid(dos->install_id)) return;
    v9x_dos_generate_id(dos);
    file = fopen(V9X_DOS_CONFIG, "r");
    existed = file != 0;
    if (file != 0) fclose(file);
    file = fopen(V9X_DOS_CONFIG, "a");
    if (file == 0) {
        v9x_log_event("install-id-unsaved", dos->install_id);
        return;
    }
    if (!existed) fputs("[agent]\n", file);
    fprintf(file, "\ninstall_id=%s\n", dos->install_id);
    fclose(file);
    v9x_log_event("install-id-created", dos->install_id);
}

unsigned long v9x_dos_increment_boot_counter(void)
{
    FILE *file;
    unsigned long value = 0ul;
    (void)mkdir(V9X_DOS_ROOT);
    (void)mkdir(V9X_DOS_TEMP);
    file = fopen(V9X_DOS_BOOT, "r+b");
    if (file == 0) file = fopen(V9X_DOS_BOOT, "w+b");
    if (file == 0) return 0ul;
    if (fread(&value, sizeof(value), 1, file) != 1) value = 0ul;
    ++value;
    rewind(file);
    (void)fwrite(&value, sizeof(value), 1, file);
    fclose(file);
    return value;
}

unsigned long v9x_dos_now_ms(void)
{
    /* Overflow-safe conversion of clock() ticks to milliseconds, whatever
       CLOCKS_PER_SEC the DOS runtime uses. */
    clock_t ticks = clock();
    if (ticks == (clock_t)-1) return 0ul;
    return (unsigned long)(ticks / CLOCKS_PER_SEC) * 1000ul +
           ((unsigned long)(ticks % CLOCKS_PER_SEC) * 1000ul) / CLOCKS_PER_SEC;
}

void v9x_log_set_boot(unsigned long boot_counter)
{
    v9x_log_boot = boot_counter;
}

void v9x_log_event(const char *event_name, const char *detail)
{
    FILE *file = fopen(V9X_DOS_LOG, "a");
    if (file == 0) return;
    fprintf(file, "boot=%lu %lums %s", v9x_log_boot, v9x_dos_now_ms(),
            event_name);
    if (detail != 0 && detail[0] != '\0') fprintf(file, " %s", detail);
    fputc('\n', file);
    fclose(file);
}

void v9x_log_line(const char *event_name)
{
    v9x_log_event(event_name, "");
}
