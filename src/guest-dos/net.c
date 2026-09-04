/* Watt-32 transport for the DOS agent.

   The Win9x agent used Winsock 1.1 (agent.c / protocol.c). DOS has no Winsock,
   so the same socket/bind/listen/accept/recv/send calls are served here by
   Watt-32's BSD-socket API. Build Watt-32 with USE_BSD_API and load a packet
   driver plus a WATTCP.CFG (IP config) before the agent runs; sock_init() reads
   that config and brings the stack up.

   The recv/send helpers keep the byte-exact loop semantics of the original
   v9x_recv_exact / v9x_send_exact: a stream socket can fragment a frame, so the
   caller always asks for an exact byte count. */

#define USE_BSD_API

#include <tcp.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>

#include "dosagent.h"

int v9x_net_init(void)
{
    /* sock_init() returns 0 on success. It aborts the program on a fatal config
       error by default; a nonzero return still means "do not proceed". */
    return sock_init() == 0;
}

int v9x_net_listen(unsigned short port)
{
    int listener;
    struct sockaddr_in address;
    int reuse = 1;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return -1;

    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                     (const char *)&reuse, sizeof(reuse));

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listener, 1) < 0) {
        close_s(listener);
        return -1;
    }
    return listener;
}

int v9x_net_accept(int listener, unsigned long *peer_ip)
{
    struct sockaddr_in client_address;
    int length = sizeof(client_address);
    int client;

    memset(&client_address, 0, sizeof(client_address));
    client = accept(listener, (struct sockaddr *)&client_address, &length);
    if (client < 0) return -1;
    if (peer_ip != 0) *peer_ip = (unsigned long)client_address.sin_addr.s_addr;
    return client;
}

int v9x_net_recv_exact(int sock, unsigned char *target, unsigned long length)
{
    unsigned long offset = 0ul;
    int received;
    while (offset < length) {
        received = recv(sock, (char *)(target + offset),
                        (int)(length - offset), 0);
        if (received <= 0) return 0;
        offset += (unsigned long)received;
    }
    return 1;
}

int v9x_net_send_exact(int sock, const unsigned char *source,
                       unsigned long length)
{
    unsigned long offset = 0ul;
    int sent;
    while (offset < length) {
        sent = send(sock, (const char *)(source + offset),
                    (int)(length - offset), 0);
        if (sent <= 0) return 0;
        offset += (unsigned long)sent;
    }
    return 1;
}

void v9x_net_close(int sock)
{
    if (sock >= 0) close_s(sock);
}

unsigned long v9x_net_local_ip(void)
{
    /* gethostid() returns the configured local IP in host byte order. */
    return (unsigned long)gethostid();
}

unsigned long v9x_net_inet_addr(const char *text)
{
    /* Network byte order, INADDR_NONE (0xffffffff) on a malformed address. */
    return (unsigned long)inet_addr(text);
}
