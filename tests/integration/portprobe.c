/* PORTPROB.EXE: read-only probe for ring-3 port access on Windows 9x.

   Answers one question before the agent's `reset` verb is built: can a
   Win32 program reach the reset control register (port CF9h) and the 8042
   (port 64h) directly, or does VMM trap them? It never writes a port.

   It reports, into PORTPROB.TXT next to the executable:
   - CPL and IOPL of the calling thread (IOPL 3 would mean no trapping);
   - the TSS I/O permission bitmap bit of each port of interest, found
     through SGDT and STR (both unprivileged; the Win9x system arena is
     readable from ring 3). A set bit means VMM traps that port for the
     current (System) VM; a port beyond the TSS limit is trapped as well;
   - for each port, the value of a byte IN and the fastest of 16 reads in
     TSC cycles, so a trapped port shows up as a slow one too.
   Every step runs under _try/_except, so a fault is reported, not fatal.

   GUI subsystem on purpose: a console program would start a DOS VM, which
   some guests (the Velocity9x netbook) must not do.

   Built with the normal Watcom CRT (it is a test tool, not shipped):
     wcl386 -bt=nt -l=nt_win -zq tests\integration\portprobe.c */

#include <stdio.h>
#include <string.h>
#include <windows.h>

unsigned char probe_inb(unsigned short port);
#pragma aux probe_inb = "in al, dx" parm [dx] value [al];

unsigned long probe_ind(unsigned short port);
#pragma aux probe_ind = "in eax, dx" parm [dx] value [eax];

unsigned long probe_tsc(void);
#pragma aux probe_tsc = 0x0f 0x31 value [eax] modify [edx];

unsigned long probe_eflags(void);
#pragma aux probe_eflags = "pushfd" "pop eax" value [eax];

unsigned short probe_cs(void);
#pragma aux probe_cs = "mov ax, cs" value [ax];

unsigned short probe_tr(void);
#pragma aux probe_tr = "str ax" value [ax];

void probe_sgdt(unsigned char *out);
#pragma aux probe_sgdt = "sgdt [eax]" parm [eax];

static FILE *out;

static void line(const char *text)
{
    fputs(text, out);
    fputs("\r\n", out);
    fflush(out);
}

static const unsigned short ports[] = {
    0x0060, 0x0061, 0x0064, 0x0080, 0x0092, 0x00cf8, 0x00cf9, 0x00cfa,
    0x00cfb, 0x00cfc
};

static unsigned long tss_base;
static unsigned long tss_limit;
static unsigned long iopb_offset;
static int have_tss;

static void report_cpu(void)
{
    char text[128];
    unsigned long flags = probe_eflags();
    sprintf(text, "cpl=%u iopl=%lu eflags=%08lX",
            (unsigned)(probe_cs() & 3u), (flags >> 12) & 3ul, flags);
    line(text);
}

static void read_tss(void)
{
    char text[160];
    unsigned char gdtr[6];
    unsigned long gdt_base;
    unsigned short gdt_limit;
    unsigned short tr;
    const unsigned char *desc;
    probe_sgdt(gdtr);
    gdt_limit = (unsigned short)(gdtr[0] | (gdtr[1] << 8));
    gdt_base = (unsigned long)gdtr[2] | ((unsigned long)gdtr[3] << 8) |
               ((unsigned long)gdtr[4] << 16) | ((unsigned long)gdtr[5] << 24);
    tr = probe_tr();
    sprintf(text, "gdt base=%08lX limit=%04X tr=%04X", gdt_base,
            (unsigned)gdt_limit, (unsigned)tr);
    line(text);
    if ((unsigned)(tr & 0xfff8u) + 7u > gdt_limit) {
        line("tss: TR selector beyond GDT limit");
        return;
    }
    desc = (const unsigned char *)(gdt_base + (tr & 0xfff8u));
    tss_base = (unsigned long)desc[2] | ((unsigned long)desc[3] << 8) |
               ((unsigned long)desc[4] << 16) | ((unsigned long)desc[7] << 24);
    tss_limit = (unsigned long)desc[0] | ((unsigned long)desc[1] << 8) |
                ((unsigned long)(desc[6] & 0x0fu) << 16);
    if (desc[6] & 0x80u) tss_limit = (tss_limit << 12) | 0xffful;
    iopb_offset = (unsigned long)*(const unsigned short *)(tss_base + 0x66ul);
    sprintf(text, "tss base=%08lX limit=%08lX type=%02X iopb=%04lX",
            tss_base, tss_limit, (unsigned)(desc[5] & 0x1fu), iopb_offset);
    line(text);
    have_tss = 1;
}

static void report_bitmap(unsigned short port)
{
    char text[96];
    unsigned long byte_offset = iopb_offset + (port >> 3);
    if (byte_offset > tss_limit) {
        sprintf(text, "bitmap %04X: beyond TSS limit (trapped)", (unsigned)port);
    } else {
        unsigned char bits = *(const unsigned char *)(tss_base + byte_offset);
        sprintf(text, "bitmap %04X: %s", (unsigned)port,
                (bits >> (port & 7u)) & 1u ? "trapped" : "direct");
    }
    line(text);
}

static void report_read(unsigned short port, int dword)
{
    char text[96];
    unsigned long best = 0xfffffffful;
    unsigned long value = 0ul;
    unsigned long start;
    unsigned long spent;
    int round;
    for (round = 0; round < 16; ++round) {
        start = probe_tsc();
        value = dword ? probe_ind(port) : (unsigned long)probe_inb(port);
        spent = probe_tsc() - start;
        if (spent < best) best = spent;
    }
    sprintf(text, "in%s %04X: value=%0*lX cycles=%lu", dword ? "d" : "b",
            (unsigned)port, dword ? 8 : 2, value, best);
    line(text);
}

static void report_baseline(void)
{
    char text[64];
    unsigned long best = 0xfffffffful;
    unsigned long start;
    unsigned long spent;
    int round;
    for (round = 0; round < 16; ++round) {
        start = probe_tsc();
        spent = probe_tsc() - start;
        if (spent < best) best = spent;
    }
    sprintf(text, "tsc baseline cycles=%lu", best);
    line(text);
}

static void fault(const char *step, unsigned long code)
{
    char text[96];
    sprintf(text, "FAULT in %s: exception %08lX", step, code);
    line(text);
}

int PASCAL WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command,
                   int show)
{
    char path[MAX_PATH];
    char *dot;
    unsigned index;
    OSVERSIONINFOA version;
    char text[128];
    (void)previous; (void)command; (void)show;
    GetModuleFileNameA(instance, path, sizeof(path) - 4);
    dot = strrchr(path, '.');
    if (dot == NULL) dot = path + strlen(path);
    strcpy(dot, ".TXT");
    out = fopen(path, "wb");
    if (out == NULL) return 2;

    version.dwOSVersionInfoSize = sizeof(version);
    GetVersionExA(&version);
    sprintf(text, "portprobe 1 os=%lu.%lu.%lu platform=%lu",
            version.dwMajorVersion, version.dwMinorVersion,
            version.dwBuildNumber & 0xfffful, version.dwPlatformId);
    line(text);

    _try { report_cpu(); }
    _except (EXCEPTION_EXECUTE_HANDLER) { fault("cpu", GetExceptionCode()); }
    _try { read_tss(); }
    _except (EXCEPTION_EXECUTE_HANDLER) { fault("tss", GetExceptionCode()); }
    if (have_tss) {
        for (index = 0; index < sizeof(ports) / sizeof(ports[0]); ++index) {
            _try { report_bitmap(ports[index]); }
            _except (EXCEPTION_EXECUTE_HANDLER) {
                fault("bitmap", GetExceptionCode());
            }
        }
    }
    _try { report_baseline(); }
    _except (EXCEPTION_EXECUTE_HANDLER) { fault("tsc", GetExceptionCode()); }
    /* Reads only. 60h is skipped: reading it could steal a keystroke. */
    _try { report_read(0x0064, 0); }
    _except (EXCEPTION_EXECUTE_HANDLER) { fault("in 64", GetExceptionCode()); }
    _try { report_read(0x0061, 0); }
    _except (EXCEPTION_EXECUTE_HANDLER) { fault("in 61", GetExceptionCode()); }
    _try { report_read(0x0080, 0); }
    _except (EXCEPTION_EXECUTE_HANDLER) { fault("in 80", GetExceptionCode()); }
    _try { report_read(0x0092, 0); }
    _except (EXCEPTION_EXECUTE_HANDLER) { fault("in 92", GetExceptionCode()); }
    _try { report_read(0x0cf8, 1); }
    _except (EXCEPTION_EXECUTE_HANDLER) { fault("in cf8", GetExceptionCode()); }
    _try { report_read(0x0cf9, 0); }
    _except (EXCEPTION_EXECUTE_HANDLER) { fault("in cf9", GetExceptionCode()); }
    line("done");
    fclose(out);
    return 0;
}
