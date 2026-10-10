#ifndef V9XREMOTE_MDNS_H
#define V9XREMOTE_MDNS_H

/* mDNS / DNS-SD message codec (RFC 6762, RFC 6763), no sockets. The agent
   advertises one service instance:

     _v9xremote._tcp.local             PTR  <instance>._v9xremote._tcp.local
     _services._dns-sd._udp.local      PTR  _v9xremote._tcp.local
     <instance>._v9xremote._tcp.local  SRV  0 0 <port> <host>.local
     <instance>._v9xremote._tcp.local  TXT  id=... ver=... ...
     <host>.local                      A    <address>

   Queries are parsed with compression pointers; responses are written
   without compression, which every resolver must accept. */

#define V9X_MDNS_PORT 5353u
#define V9X_MDNS_GROUP "224.0.0.251"
#define V9X_MDNS_SERVICE "_v9xremote._tcp.local"
#define V9X_MDNS_ENUM "_services._dns-sd._udp.local"
#define V9X_MDNS_MAX_TXT 8
#define V9X_MDNS_PACKET_MAX 1024ul

#define V9X_MDNS_WANT_SERVICE 0x01u  /* PTR for the service type */
#define V9X_MDNS_WANT_ENUM    0x02u  /* PTR for _services._dns-sd._udp */
#define V9X_MDNS_WANT_SRV     0x04u
#define V9X_MDNS_WANT_TXT     0x08u
#define V9X_MDNS_WANT_A       0x10u
#define V9X_MDNS_WANT_ALL     0x1fu

#define V9X_MDNS_TYPE_A    1u
#define V9X_MDNS_TYPE_PTR  12u
#define V9X_MDNS_TYPE_TXT  16u
#define V9X_MDNS_TYPE_SRV  33u
#define V9X_MDNS_TYPE_ANY  255u

typedef struct V9xMdnsService {
    const char *instance;     /* one label, e.g. "WIN98-1a2b3c4d" */
    const char *host;         /* one label, e.g. "v9x-1a2b3c4d" (.local added) */
    unsigned short port;
    unsigned char address[4]; /* IPv4, network order */
    const char *txt[V9X_MDNS_MAX_TXT];
    unsigned int txt_count;
} V9xMdnsService;

typedef struct V9xMdnsQuery {
    unsigned short id;
    unsigned int wants;       /* V9X_MDNS_WANT_* the questions ask for */
    int unicast;              /* any question had the QU bit */
} V9xMdnsQuery;

/* 1 when packet is a well-formed query; wants is 0 when nothing in it is
   about this service. */
int v9x_mdns_parse_query(const unsigned char *packet, unsigned long length,
                         const V9xMdnsService *service, V9xMdnsQuery *query);

/* Builds a response carrying `wants` as answers plus the rest of the
   instance's records as additionals. legacy: reply to a one-shot querier on a
   port other than 5353 (echo id, no cache-flush bit, TTL capped at 10 s).
   goodbye: every TTL 0. Returns the length, or 0 when it does not fit. */
unsigned long v9x_mdns_build_response(const V9xMdnsService *service,
                                      unsigned int wants, unsigned short id,
                                      int legacy, int goodbye,
                                      unsigned char *out,
                                      unsigned long capacity);

#endif
