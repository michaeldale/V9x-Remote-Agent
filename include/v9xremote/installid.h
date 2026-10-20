#ifndef V9XREMOTE_INSTALLID_H
#define V9XREMOTE_INSTALLID_H

/* Install ID: an RFC 4122 version 4 UUID generated once per install and kept
   in AGENT.INI [agent] install_id, so a machine can be recognised when its IP
   address changes. Neither 9x nor DOS has a usable random source without
   OLE32/RPCRT4, so the caller feeds whatever varies between machines and
   boots (TSC, timers, volume serial, names) into a small mixing pool. */

#define V9X_INSTALL_ID_LENGTH 36
#define V9X_INSTALL_ID_SIZE 37

typedef struct V9xIdPool {
    unsigned long words[4];
    unsigned long count;
} V9xIdPool;

void v9x_id_begin(V9xIdPool *pool);
void v9x_id_mix(V9xIdPool *pool, unsigned long value);
void v9x_id_mix_text(V9xIdPool *pool, const char *text);
/* Writes 36 lowercase characters plus a terminator, with the version (4) and
   variant (10xx) bits set. */
void v9x_id_finish(V9xIdPool *pool, char *target);
/* 1 when text is exactly a canonical 8-4-4-4-12 hex UUID (either case). */
int v9x_id_valid(const char *text);

/* AGENT.INI repair. The Windows 9x profile writer, asked to add a key to a
   section whose lines end in a bare LF, writes it as CR key=value CR; the
   reader then sees the key as "\rkey" and never finds it, so every start
   appended another install_id. This rewrites an INI image with every line
   ending in CRLF (CRLF, bare LF and bare CR all count as one line end), runs
   of blank lines collapsed to one, trailing blank lines dropped, and only the
   first unique_key= line (case-insensitive) kept in each section, which is
   the one GetPrivateProfileString returns. unique_key may be 0. A file that
   is already clean comes out byte for byte the same. Returns 1 with the
   image in out and its size in *out_length, 0 when it does not fit in
   capacity (2 * length + 2 always fits). */
int v9x_ini_repair(const char *text, unsigned long length, char *out,
                   unsigned long capacity, const char *unique_key,
                   unsigned long *out_length);

#endif
