#include "http.h"
#include "platform_win.h"

static int v9x_http_ci_equal(const char *text, const char *lower,
                             unsigned long n)
{
    unsigned long index;
    for (index = 0ul; index < n; ++index) {
        unsigned char ch = (unsigned char)text[index];
        if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch + ('a' - 'A'));
        if (ch != (unsigned char)lower[index]) return 0;
    }
    return 1;
}

static int v9x_http_append(char *target, unsigned long *offset,
                           unsigned long capacity, const char *text)
{
    while (*text != '\0') {
        if (*offset + 1ul >= capacity) return 0;
        target[(*offset)++] = *text++;
    }
    target[*offset] = '\0';
    return 1;
}

static int v9x_http_append_number(char *target, unsigned long *offset,
                                  unsigned long capacity, unsigned long value)
{
    char reverse[11];
    char forward[11];
    unsigned long count = 0ul;
    unsigned long index;
    do {
        reverse[count++] = (char)('0' + (value % 10ul));
        value /= 10ul;
    } while (value != 0ul);
    for (index = 0ul; index < count; ++index) {
        forward[index] = reverse[count - index - 1ul];
    }
    forward[count] = '\0';
    return v9x_http_append(target, offset, capacity, forward);
}

int v9x_http_parse_url(const char *url, char *host, unsigned short *port,
                       char *path, int *is_https)
{
    unsigned long index = 0ul;
    unsigned long host_length = 0ul;
    unsigned long path_length = 0ul;
    unsigned long port_value = 0ul;
    int have_port = 0;
    *is_https = 0;
    if (v9x_http_ci_equal(url, "https://", 8ul)) {
        *is_https = 1;
        return 0;
    }
    if (!v9x_http_ci_equal(url, "http://", 7ul)) return 0;
    index = 7ul;
    while (url[index] != '\0' && url[index] != '/' && url[index] != ':') {
        if (host_length + 1ul >= V9X_HTTP_MAX_HOST) return 0;
        host[host_length++] = url[index++];
    }
    if (host_length == 0ul) return 0;
    host[host_length] = '\0';
    if (url[index] == ':') {
        ++index;
        while (url[index] >= '0' && url[index] <= '9') {
            port_value = port_value * 10ul + (unsigned long)(url[index] - '0');
            if (port_value > 65535ul) return 0;
            ++index;
            have_port = 1;
        }
        if (!have_port) return 0;
    }
    *port = (unsigned short)(have_port ? port_value : 80ul);
    if (url[index] == '\0') {
        path[0] = '/';
        path[1] = '\0';
        return 1;
    }
    if (url[index] != '/') return 0;
    while (url[index] != '\0') {
        if (path_length + 1ul >= V9X_HTTP_MAX_PATH) return 0;
        path[path_length++] = url[index++];
    }
    path[path_length] = '\0';
    return 1;
}

/* Wait until handle is readable (for_write 0) or writable (1). */
static int v9x_http_wait(SOCKET handle, int for_write, DWORD timeout_ms)
{
    fd_set set;
    struct timeval timeout;
    int ready;
    FD_ZERO(&set);
    FD_SET(handle, &set);
    timeout.tv_sec = (long)(timeout_ms / 1000ul);
    timeout.tv_usec = (long)((timeout_ms % 1000ul) * 1000ul);
    ready = select(0, for_write ? 0 : &set, for_write ? &set : 0, 0, &timeout);
    return ready > 0;
}

SOCKET v9x_http_connect(const char *host, unsigned short port,
                        DWORD timeout_ms, DWORD *native_error)
{
    struct sockaddr_in address;
    struct hostent *resolved;
    unsigned long numeric;
    unsigned long mode;
    SOCKET handle;
    unsigned long index;
    int result;
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    numeric = inet_addr(host);
    if (numeric != INADDR_NONE) {
        address.sin_addr.s_addr = numeric;
    } else {
        resolved = gethostbyname(host);
        if (resolved == 0 || resolved->h_addr_list == 0 ||
            resolved->h_addr_list[0] == 0 || resolved->h_length != 4) {
            *native_error = WSAGetLastError();
            return INVALID_SOCKET;
        }
        for (index = 0ul; index < 4ul; ++index) {
            ((unsigned char *)&address.sin_addr)[index] =
                (unsigned char)resolved->h_addr_list[0][index];
        }
    }
    handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET) {
        *native_error = WSAGetLastError();
        return INVALID_SOCKET;
    }
    v9x_platform_no_inherit((HANDLE)handle);
    /* Non-blocking connect bounded by select, then back to blocking: a
       blocking connect to a dead address waits out the stack's own
       timeout, which on Windows 9x is over a minute. */
    mode = 1ul;
    (void)ioctlsocket(handle, FIONBIO, &mode);
    result = connect(handle, (struct sockaddr *)&address, sizeof(address));
    if (result == SOCKET_ERROR) {
        *native_error = WSAGetLastError();
        if (*native_error != WSAEWOULDBLOCK ||
            !v9x_http_wait(handle, 1, timeout_ms)) {
            if (*native_error == WSAEWOULDBLOCK) *native_error = WSAETIMEDOUT;
            closesocket(handle);
            return INVALID_SOCKET;
        }
    }
    mode = 0ul;
    (void)ioctlsocket(handle, FIONBIO, &mode);
    *native_error = 0ul;
    return handle;
}

int v9x_http_send_all(SOCKET handle, const char *data, unsigned long length,
                      DWORD timeout_ms)
{
    unsigned long total = 0ul;
    unsigned long piece;
    int sent;
    while (total < length) {
        if (!v9x_http_wait(handle, 1, timeout_ms)) return 0;
        piece = length - total;
        if (piece > 32768ul) piece = 32768ul;
        sent = send(handle, data + total, (int)piece, 0);
        if (sent <= 0) return 0;
        total += (unsigned long)sent;
    }
    return 1;
}

int v9x_http_recv(SOCKET handle, unsigned char *buffer, unsigned long capacity,
                  DWORD timeout_ms)
{
    int received;
    if (!v9x_http_wait(handle, 0, timeout_ms)) return -1;
    received = recv(handle, (char *)buffer, (int)capacity, 0);
    return received < 0 ? -1 : received;
}

unsigned long v9x_http_find_body(const unsigned char *buffer,
                                 unsigned long length)
{
    unsigned long index;
    if (length < 4ul) return 0ul;
    for (index = 0ul; index + 4ul <= length; ++index) {
        if (buffer[index] == '\r' && buffer[index + 1ul] == '\n' &&
            buffer[index + 2ul] == '\r' && buffer[index + 3ul] == '\n') {
            return index + 4ul;
        }
    }
    return 0ul;
}

unsigned long v9x_http_status_code(const unsigned char *head,
                                   unsigned long length)
{
    unsigned long index = 0ul;
    unsigned long code = 0ul;
    int digits = 0;
    if (length < 5ul || !v9x_http_ci_equal((const char *)head, "http/", 5ul)) {
        return 0ul;
    }
    while (index < length && head[index] != ' ' && head[index] != '\r') {
        ++index;
    }
    while (index < length && head[index] == ' ') ++index;
    while (index < length && head[index] >= '0' && head[index] <= '9') {
        code = code * 10ul + (unsigned long)(head[index] - '0');
        ++index;
        ++digits;
    }
    return digits == 3 ? code : 0ul;
}

unsigned long v9x_http_content_length(const unsigned char *head,
                                      unsigned long length)
{
    static const char name[] = "\r\ncontent-length:";
    unsigned long index;
    unsigned long at;
    unsigned long value;
    int digits;
    for (index = 0ul; index + sizeof(name) - 1ul <= length; ++index) {
        if (!v9x_http_ci_equal((const char *)head + index, name,
                               sizeof(name) - 1ul)) {
            continue;
        }
        at = index + sizeof(name) - 1ul;
        while (at < length && head[at] == ' ') ++at;
        value = 0ul;
        digits = 0;
        while (at < length && head[at] >= '0' && head[at] <= '9') {
            if (value > 429496728ul) return V9X_HTTP_NO_LENGTH;
            value = value * 10ul + (unsigned long)(head[at] - '0');
            ++at;
            ++digits;
        }
        return digits != 0 ? value : V9X_HTTP_NO_LENGTH;
    }
    return V9X_HTTP_NO_LENGTH;
}

int v9x_http_request(const V9xHttpRequest *request, unsigned char *buffer,
                     unsigned long capacity, V9xHttpResponse *response,
                     DWORD *native_error)
{
    char host[V9X_HTTP_MAX_HOST];
    char path[V9X_HTTP_MAX_PATH];
    char head[V9X_HTTP_MAX_PATH + 1024ul];
    unsigned long offset = 0ul;
    unsigned long used = 0ul;
    unsigned long body_start = 0ul;
    unsigned long expected = V9X_HTTP_NO_LENGTH;
    unsigned short port = 80u;
    int is_https = 0;
    int received;
    SOCKET handle;

    *native_error = 0ul;
    response->status = 0ul;
    response->body_offset = 0ul;
    response->body_length = 0ul;
    if (!v9x_http_parse_url(request->url, host, &port, path, &is_https)) {
        *native_error = ERROR_INVALID_PARAMETER;
        return 0;
    }
    if (!v9x_http_append(head, &offset, sizeof(head), request->method) ||
        !v9x_http_append(head, &offset, sizeof(head), " ") ||
        !v9x_http_append(head, &offset, sizeof(head), path) ||
        !v9x_http_append(head, &offset, sizeof(head), " HTTP/1.0\r\nHost: ") ||
        !v9x_http_append(head, &offset, sizeof(head), host) ||
        !v9x_http_append(head, &offset, sizeof(head),
                         "\r\nUser-Agent: v9x-agent\r\nConnection: close\r\n") ||
        (request->headers != 0 &&
         !v9x_http_append(head, &offset, sizeof(head), request->headers))) {
        *native_error = ERROR_INVALID_PARAMETER;
        return 0;
    }
    if (request->body != 0 || v9x_http_ci_equal(request->method, "post", 4ul)) {
        if (!v9x_http_append(head, &offset, sizeof(head),
                             "Content-Type: application/octet-stream\r\n"
                             "Content-Length: ") ||
            !v9x_http_append_number(head, &offset, sizeof(head),
                                    request->body_length) ||
            !v9x_http_append(head, &offset, sizeof(head), "\r\n")) {
            *native_error = ERROR_INVALID_PARAMETER;
            return 0;
        }
    }
    if (!v9x_http_append(head, &offset, sizeof(head), "\r\n")) {
        *native_error = ERROR_INVALID_PARAMETER;
        return 0;
    }

    handle = v9x_http_connect(host, port, request->timeout_ms, native_error);
    if (handle == INVALID_SOCKET) return 0;
    if (!v9x_http_send_all(handle, head, offset, request->timeout_ms) ||
        (request->body_length != 0ul &&
         !v9x_http_send_all(handle, (const char *)request->body,
                            request->body_length, request->timeout_ms))) {
        *native_error = WSAGetLastError();
        if (*native_error == 0ul) *native_error = WSAETIMEDOUT;
        closesocket(handle);
        return 0;
    }
    for (;;) {
        if (used >= capacity) {
            *native_error = ERROR_INSUFFICIENT_BUFFER;
            closesocket(handle);
            return 0;
        }
        received = v9x_http_recv(handle, buffer + used, capacity - used,
                                 request->timeout_ms);
        if (received < 0) {
            *native_error = WSAGetLastError();
            if (*native_error == 0ul) *native_error = WSAETIMEDOUT;
            closesocket(handle);
            return 0;
        }
        if (received == 0) break;
        used += (unsigned long)received;
        if (body_start == 0ul) {
            body_start = v9x_http_find_body(buffer, used);
            if (body_start != 0ul) {
                expected = v9x_http_content_length(buffer, body_start);
            }
        }
        if (body_start != 0ul && expected != V9X_HTTP_NO_LENGTH &&
            used - body_start >= expected) {
            break;
        }
    }
    closesocket(handle);
    if (body_start == 0ul) {
        *native_error = ERROR_INVALID_DATA;
        return 0;
    }
    response->status = v9x_http_status_code(buffer, body_start);
    response->body_offset = body_start;
    response->body_length = used - body_start;
    if (response->status == 0ul ||
        (expected != V9X_HTTP_NO_LENGTH && response->body_length < expected)) {
        *native_error = ERROR_INVALID_DATA;
        return 0;
    }
    if (expected != V9X_HTTP_NO_LENGTH) response->body_length = expected;
    return 1;
}
