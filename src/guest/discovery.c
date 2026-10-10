#include "agent.h"
#include "platform_win.h"
#include "v9xremote/mdns.h"
#include "v9xremote/version.h"

/* mDNS / DNS-SD responder: advertises this agent as _v9xremote._tcp on the
   LAN so a host can find it (and find it again by install ID after its
   address changes) without knowing the IP. Winsock 1.1 multicast only; no
   Bonjour or other third-party software on either side.

   Names carry the first 8 characters of the install ID, so two agents never
   claim the same name and the RFC 6762 probe step is skipped. The thread
   calls WSAStartup itself (Winsock reference-counts it), so the accept loop's
   WSACleanup-and-retry never pulls the socket out from under it. */

#define V9X_MDNS_RETRY_MS 30000ul
#define V9X_MDNS_POLL_MS 1000l

static V9xAgentState *v9x_mdns_state;
static volatile SOCKET v9x_mdns_socket = INVALID_SOCKET;
static CRITICAL_SECTION v9x_mdns_lock;
static V9xMdnsService v9x_mdns_service;
static char v9x_mdns_instance[64];
static char v9x_mdns_host[16];
static char v9x_mdns_txt[6][64];
static unsigned char v9x_mdns_in[V9X_MDNS_PACKET_MAX];
static unsigned char v9x_mdns_out[V9X_MDNS_PACKET_MAX];

static unsigned long v9x_mdns_append(char *target, unsigned long at,
                                     unsigned long capacity, const char *text)
{
    while (*text != '\0' && at + 1ul < capacity) target[at++] = *text++;
    target[at] = '\0';
    return at;
}

static unsigned long v9x_mdns_decimal(char *target, unsigned long at,
                                      unsigned long capacity, unsigned long value)
{
    char digits[10];
    unsigned long count = 0ul;
    do {
        digits[count++] = (char)('0' + (value % 10ul));
        value /= 10ul;
    } while (value != 0ul);
    while (count != 0ul && at + 1ul < capacity) target[at++] = digits[--count];
    target[at] = '\0';
    return at;
}

/* Instance "<COMPUTERNAME>-<id8>", host "v9x-<id8>". A computer name may hold
   characters DNS-SD allows but some browsers show badly; keep letters, digits
   and hyphens. */
static void v9x_mdns_describe(V9xAgentState *state)
{
    char name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD length = sizeof(name);
    DWORD version = GetVersion();
    unsigned long at = 0ul;
    unsigned long index;
    char id8[9];

    for (index = 0ul; index < 8ul; ++index) id8[index] = state->install_id[index];
    id8[8] = '\0';
    if (!GetComputerNameA(name, &length)) name[0] = '\0';
    name[sizeof(name) - 1] = '\0';
    for (index = 0ul; name[index] != '\0' && at < 40ul; ++index) {
        char ch = name[index];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-')) {
            ch = '-';
        }
        v9x_mdns_instance[at++] = ch;
    }
    if (at == 0ul) at = v9x_mdns_append(v9x_mdns_instance, 0ul, 40ul, "V9X");
    v9x_mdns_instance[at++] = '-';
    v9x_mdns_instance[at] = '\0';
    (void)v9x_mdns_append(v9x_mdns_instance, at, sizeof(v9x_mdns_instance), id8);

    at = v9x_mdns_append(v9x_mdns_host, 0ul, sizeof(v9x_mdns_host), "v9x-");
    (void)v9x_mdns_append(v9x_mdns_host, at, sizeof(v9x_mdns_host), id8);

    at = v9x_mdns_append(v9x_mdns_txt[0], 0ul, 64ul, "id=");
    (void)v9x_mdns_append(v9x_mdns_txt[0], at, 64ul, state->install_id);
    at = v9x_mdns_append(v9x_mdns_txt[1], 0ul, 64ul, "ver=");
    (void)v9x_mdns_append(v9x_mdns_txt[1], at, 64ul, V9X_AGENT_VERSION);
    at = v9x_mdns_append(v9x_mdns_txt[2], 0ul, 64ul, "build=");
    (void)v9x_mdns_append(v9x_mdns_txt[2], at, 64ul, V9X_BUILD_ID);
    (void)v9x_mdns_append(v9x_mdns_txt[3], 0ul, 64ul,
                          state->allow_dos_vm ? "dos=1" : "dos=0");
    at = v9x_mdns_append(v9x_mdns_txt[4], 0ul, 64ul, "os=");
    at = v9x_mdns_decimal(v9x_mdns_txt[4], at, 64ul, version & 0xfful);
    at = v9x_mdns_append(v9x_mdns_txt[4], at, 64ul, ".");
    (void)v9x_mdns_decimal(v9x_mdns_txt[4], at, 64ul, (version >> 8) & 0xfful);
    at = v9x_mdns_append(v9x_mdns_txt[5], 0ul, 64ul, "name=");
    (void)v9x_mdns_append(v9x_mdns_txt[5], at, 64ul, name);

    v9x_mdns_service.instance = v9x_mdns_instance;
    v9x_mdns_service.host = v9x_mdns_host;
    v9x_mdns_service.port = state->listen_port;
    for (index = 0ul; index < 6ul; ++index) {
        v9x_mdns_service.txt[index] = v9x_mdns_txt[index];
    }
    v9x_mdns_service.txt_count = 6u;
}

static void v9x_mdns_set_address(unsigned long address)
{
    unsigned char *bytes = (unsigned char *)&address;
    v9x_mdns_service.address[0] = bytes[0];
    v9x_mdns_service.address[1] = bytes[1];
    v9x_mdns_service.address[2] = bytes[2];
    v9x_mdns_service.address[3] = bytes[3];
}

static int v9x_mdns_send(SOCKET sock, unsigned int wants, unsigned short id,
                         int legacy, int goodbye, const struct sockaddr_in *to)
{
    unsigned long length;
    int sent = 0;
    EnterCriticalSection(&v9x_mdns_lock);
    length = v9x_mdns_build_response(&v9x_mdns_service, wants, id, legacy,
                                     goodbye, v9x_mdns_out, sizeof(v9x_mdns_out));
    if (length != 0ul) {
        sent = sendto(sock, (const char *)v9x_mdns_out, (int)length, 0,
                      (const struct sockaddr *)to, sizeof(*to)) == (int)length;
    }
    LeaveCriticalSection(&v9x_mdns_lock);
    return sent;
}

static void v9x_mdns_group(struct sockaddr_in *group)
{
    group->sin_family = AF_INET;
    group->sin_port = htons(V9X_MDNS_PORT);
    group->sin_addr.s_addr = inet_addr(V9X_MDNS_GROUP);
}

static SOCKET v9x_mdns_open(void)
{
    SOCKET sock;
    struct sockaddr_in local;
    struct ip_mreq membership;
    int yes = 1;
    int ttl = 255;
    char detail[24];
    const char *step;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        step = "socket";
        goto failed;
    }
    v9x_platform_no_inherit((HANDLE)sock);
    (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    local.sin_family = AF_INET;
    local.sin_port = htons(V9X_MDNS_PORT);
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) == SOCKET_ERROR) {
        step = "bind";
        goto failed;
    }
    membership.imr_multiaddr.s_addr = inet_addr(V9X_MDNS_GROUP);
    membership.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&membership,
                   sizeof(membership)) == SOCKET_ERROR) {
        step = "join";
        goto failed;
    }
    (void)setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));
    return sock;

failed:
    {
        unsigned long at = v9x_mdns_append(detail, 0ul, sizeof(detail), step);
        at = v9x_mdns_append(detail, at, sizeof(detail), " wsa=");
        (void)v9x_mdns_decimal(detail, at, sizeof(detail),
                               (unsigned long)WSAGetLastError());
    }
    v9x_log_event("mdns-unavailable", detail);
    if (sock != INVALID_SOCKET) closesocket(sock);
    return INVALID_SOCKET;
}

static int v9x_mdns_usable(unsigned long address)
{
    unsigned char *bytes = (unsigned char *)&address;
    return address != 0ul && address != INADDR_NONE && bytes[0] != 127u;
}

static void v9x_mdns_serve(V9xAgentState *state, SOCKET sock)
{
    struct sockaddr_in group;
    struct sockaddr_in from;
    unsigned long address = 0ul;
    unsigned long current;
    int announcements = 0;
    DWORD last_announce = 0ul;
    V9xMdnsQuery query;

    v9x_mdns_group(&group);
    for (;;) {
        fd_set readable;
        struct timeval wait;
        int ready;

        if (v9x_primary_address(state, &current) && v9x_mdns_usable(current) &&
            current != address) {
            EnterCriticalSection(&v9x_mdns_lock);
            v9x_mdns_set_address(current);
            LeaveCriticalSection(&v9x_mdns_lock);
            address = current;
            announcements = 2;                   /* RFC 6762 8.3: at least two */
            last_announce = GetTickCount() - 1000ul;
            v9x_log_event("mdns-active", v9x_mdns_instance);
        }
        if (announcements > 0 && GetTickCount() - last_announce >= 1000ul) {
            (void)v9x_mdns_send(sock, V9X_MDNS_WANT_ALL, 0u, 0, 0, &group);
            last_announce = GetTickCount();
            --announcements;
        }

        FD_ZERO(&readable);
        FD_SET(sock, &readable);
        wait.tv_sec = 0l;
        wait.tv_usec = V9X_MDNS_POLL_MS * 1000l;
        ready = select(0, &readable, 0, 0, &wait);
        if (ready == SOCKET_ERROR) return;
        if (ready == 0) continue;
        {
            int from_length = sizeof(from);
            int received = recvfrom(sock, (char *)v9x_mdns_in, sizeof(v9x_mdns_in), 0,
                                    (struct sockaddr *)&from, &from_length);
            int legacy;
            if (received == SOCKET_ERROR) {
                /* WSAECONNRESET after an ICMP unreachable is not fatal. */
                if (WSAGetLastError() == WSAECONNRESET) continue;
                return;
            }
            if (address == 0ul) continue;       /* nothing to answer with yet */
            EnterCriticalSection(&v9x_mdns_lock);
            ready = v9x_mdns_parse_query(v9x_mdns_in, (unsigned long)received,
                                         &v9x_mdns_service, &query);
            LeaveCriticalSection(&v9x_mdns_lock);
            if (!ready || query.wants == 0u) continue;
            legacy = ntohs(from.sin_port) != V9X_MDNS_PORT;
            (void)v9x_mdns_send(sock, query.wants, query.id, legacy, 0,
                                (legacy || query.unicast) ? &from : &group);
        }
    }
}

static DWORD WINAPI v9x_mdns_thread(LPVOID parameter)
{
    V9xAgentState *state = (V9xAgentState *)parameter;
    WSADATA data;
    SOCKET sock;
    while (WSAStartup(MAKEWORD(1, 1), &data) != 0) Sleep(2000ul);
    /* Wait for the accept loop: v9x_primary_address needs its Winsock. */
    while (state->winsock_version == 0u) Sleep(1000ul);
    for (;;) {
        sock = v9x_mdns_open();
        if (sock == INVALID_SOCKET) {
            InterlockedExchange(&state->discovery_status, V9X_DISCOVERY_UNAVAILABLE);
            Sleep(V9X_MDNS_RETRY_MS);
            continue;
        }
        InterlockedExchange(&state->discovery_status, V9X_DISCOVERY_ACTIVE);
        v9x_mdns_socket = sock;
        v9x_mdns_serve(state, sock);
        v9x_mdns_socket = INVALID_SOCKET;
        closesocket(sock);
        InterlockedExchange(&state->discovery_status, V9X_DISCOVERY_UNAVAILABLE);
        v9x_log_line("mdns-socket-lost");
        Sleep(V9X_MDNS_RETRY_MS);
    }
}

int v9x_mdns_start(V9xAgentState *state)
{
    HANDLE thread;
    DWORD thread_id;                      /* Win9x rejects a NULL lpThreadId */
    v9x_mdns_state = state;
    InitializeCriticalSection(&v9x_mdns_lock);
    v9x_mdns_describe(state);
    thread = CreateThread(0, 16384ul, v9x_mdns_thread, state, 0ul, &thread_id);
    if (thread == 0) return 0;
    CloseHandle(thread);
    return 1;
}

/* Tell caches to drop us (TTL 0) before the process exits for a hot update
   or a reboot. Best effort: nothing waits for it. */
void v9x_mdns_goodbye(void)
{
    struct sockaddr_in group;
    SOCKET sock = v9x_mdns_socket;
    if (v9x_mdns_state == 0 || sock == INVALID_SOCKET) return;
    v9x_mdns_group(&group);
    (void)v9x_mdns_send(sock, V9X_MDNS_WANT_ALL, 0u, 0, 1, &group);
}
