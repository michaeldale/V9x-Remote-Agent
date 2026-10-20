#ifndef V9XREMOTE_HTTP_H
#define V9XREMOTE_HTTP_H

/* A minimal plain-HTTP/1.0 client over Winsock 1.1, shared by the DOWNLOAD
   verb, the remote-help worker and SETUP.EXE /update. No TLS: Windows 9x has
   none worth using, so anything that must be trusted is signed instead
   (docs\remote-help.md). Every socket wait is bounded by select(), so a
   silent server costs a timeout, never a hung thread. Needs WSAStartup. */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock.h>

#define V9X_HTTP_MAX_HOST 256ul
#define V9X_HTTP_MAX_PATH 1024ul
#define V9X_HTTP_NO_LENGTH 0xFFFFFFFFul

/* Split an http:// URL into host, port (default 80) and path ("/" when
   absent). Returns 0 for anything else; *is_https is set for https:// so a
   caller can say why. */
int v9x_http_parse_url(const char *url, char *host, unsigned short *port,
                       char *path, int *is_https);

/* Resolve host (dotted quad or name) and connect, giving up after
   timeout_ms. The socket is returned in blocking mode, not inheritable. */
SOCKET v9x_http_connect(const char *host, unsigned short port,
                        DWORD timeout_ms, DWORD *native_error);

/* send() all of data, waiting at most timeout_ms for each piece. */
int v9x_http_send_all(SOCKET handle, const char *data, unsigned long length,
                      DWORD timeout_ms);

/* recv() whatever is available within timeout_ms: >0 bytes, 0 at end of
   stream, -1 on error or timeout. */
int v9x_http_recv(SOCKET handle, unsigned char *buffer, unsigned long capacity,
                  DWORD timeout_ms);

/* Index just past the CRLF CRLF ending a response head, or 0 if absent. */
unsigned long v9x_http_find_body(const unsigned char *buffer,
                                 unsigned long length);

/* The status of an "HTTP/1.x nnn" line, or 0 if malformed. */
unsigned long v9x_http_status_code(const unsigned char *head,
                                   unsigned long length);

/* Content-Length from a response head, or V9X_HTTP_NO_LENGTH. */
unsigned long v9x_http_content_length(const unsigned char *head,
                                      unsigned long length);

typedef struct V9xHttpRequest {
    const char *method;          /* "GET" or "POST" */
    const char *url;             /* http://host[:port]/path?query */
    const char *headers;         /* extra "Name: value\r\n" lines, or 0 */
    const unsigned char *body;   /* POST body, or 0 */
    unsigned long body_length;
    DWORD timeout_ms;            /* per socket wait */
} V9xHttpRequest;

typedef struct V9xHttpResponse {
    unsigned long status;        /* HTTP status code */
    unsigned long body_offset;   /* where the body starts in the buffer */
    unsigned long body_length;   /* bytes of body received */
} V9xHttpResponse;

/* One request, the whole response read into buffer (head and body). Fails
   (0) if it does not fit capacity, if the connection drops before a
   Content-Length is satisfied, or on any timeout. *native_error is a
   Winsock or Win32 code, ERROR_INSUFFICIENT_BUFFER for an oversized reply. */
int v9x_http_request(const V9xHttpRequest *request, unsigned char *buffer,
                     unsigned long capacity, V9xHttpResponse *response,
                     DWORD *native_error);

#endif
