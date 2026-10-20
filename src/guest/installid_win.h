#ifndef V9XREMOTE_INSTALLID_WIN_H
#define V9XREMOTE_INSTALLID_WIN_H

/* target needs V9X_INSTALL_ID_SIZE bytes. */
void v9x_install_id_generate(char *target);
/* Reads [agent] install_id from ini_path into target (lowercased); when it is
   missing or malformed, generates one and writes it back. Returns 0 when an
   existing ID was kept, 1 when a new one was stored, -1 when a new one was
   generated but could not be written (it is still valid for this run). */
int v9x_install_id_ensure(const char *ini_path, char *target);
/* Rewrites ini_path with v9x_ini_repair (CRLF line ends, one install_id) when
   it is not already in that form, keeping the original as <name>.BAK. Call
   before the first Get/WritePrivateProfile* on the file. Returns 0 when it
   was clean or absent, 1 when it was rewritten, -1 when it needed repair but
   could not be read, backed up or written (or is over 32 KB). */
int v9x_ini_normalize(const char *ini_path);

#endif
