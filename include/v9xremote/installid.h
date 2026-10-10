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

#endif
