/* MDNSQ.EXE: guest-side probe for the agent's mDNS responder.

   Under 86Box SLiRP the host cannot see guest multicast, so `v9xctl
   discover` has nothing to hear. This runs inside the guest instead: it sends
   one PTR query for _v9xremote._tcp.local from an ephemeral port (so the
   responder answers by unicast) and prints each reply as one hex line, which
   tests\integration\Test-GuestDiscovery.ps1 decodes with the host codec.

     MDNSQ <address> [port] [seconds]     e.g. MDNSQ 224.0.0.251
                                               MDNSQ 10.0.2.15 5353

   Built with the normal Watcom CRT (it is a test tool, not shipped):
     wcl386 -bt=nt -l=nt -zq tests\integration\mdnsq.c wsock32.lib */

#include <stdio.h>
#include <stdlib.h>
#include <winsock.h>

static const unsigned char query[] = {
    0x00, 0x42, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    10, '_', 'v', '9', 'x', 'r', 'e', 'm', 'o', 't', 'e',
    4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0,
    0x00, 0x0c, 0x00, 0x01
};

int main(int argc, char **argv)
{
    WSADATA data;
    SOCKET sock;
    struct sockaddr_in to;
    struct sockaddr_in from;
    unsigned char reply[1500];
    int seconds = 3;
    int replies = 0;
    DWORD start;

    if (argc < 2) {
        fprintf(stderr, "usage: MDNSQ <address> [port] [seconds]\n");
        return 2;
    }
    if (argc > 3) seconds = atoi(argv[3]);
    if (WSAStartup(MAKEWORD(1, 1), &data) != 0) return 3;
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) return 3;
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)(argc > 2 ? atoi(argv[2]) : 5353));
    to.sin_addr.s_addr = inet_addr(argv[1]);
    if (sendto(sock, (const char *)query, sizeof(query), 0,
               (struct sockaddr *)&to, sizeof(to)) != (int)sizeof(query)) {
        fprintf(stderr, "sendto failed: %d\n", WSAGetLastError());
        return 3;
    }
    start = GetTickCount();
    while (GetTickCount() - start < (DWORD)seconds * 1000ul) {
        fd_set readable;
        struct timeval wait;
        int length;
        int from_length = sizeof(from);
        int index;
        FD_ZERO(&readable);
        FD_SET(sock, &readable);
        wait.tv_sec = 0;
        wait.tv_usec = 200000;
        if (select(0, &readable, 0, 0, &wait) <= 0) continue;
        length = recvfrom(sock, (char *)reply, sizeof(reply), 0,
                          (struct sockaddr *)&from, &from_length);
        if (length <= 0) continue;
        printf("%s ", inet_ntoa(from.sin_addr));
        for (index = 0; index < length; ++index) printf("%02x", reply[index]);
        printf("\n");
        ++replies;
    }
    closesocket(sock);
    WSACleanup();
    return replies > 0 ? 0 : 1;
}
