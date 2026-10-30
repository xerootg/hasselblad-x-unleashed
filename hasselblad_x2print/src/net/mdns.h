/* mdns.h — Minimal mDNS/DNS-SD query client (POSIX sockets only; no avahi dependency)
 *
 * Purpose: discover IPP printing services on a printer hotspot network (AirPrint printers
 *          respond to PTR/SRV/TXT/A records for _ipp._tcp.local).
 *
 * The same DNS message encoder/decoder can also query public DNS via mdns_query_unicast,
 * which verifies encoding/decoding in environments where multicast is unavailable (WSL/corporate networks).
 */
#ifndef X2P_MDNS_H
#define X2P_MDNS_H

#ifdef __cplusplus
extern "C" {
#endif

#define MDNS_TXT_MAX 896 /* Maximum concatenated TXT key/value data */

typedef struct {
    char instance[160];   /* Service instance name (PTR rdata, including ._ipp._tcp.local suffix) */
    char host[160];       /* SRV target hostname (usually xxxx.local) */
    char ip[64];          /* IPv4 dotted-decimal from A record */
    int  port;            /* SRV port (typically 631 for IPP) */
    char txt[MDNS_TXT_MAX]; /* Concatenated TXT records "k=v;k=v" (including AirPrint keys such as rp=/ty=/URF) */
    int  universal;       /* 1 = response via _universal._sub._ipp._tcp (AirPrint indicator) */
} mdns_service_t;

/* Multicast discovery for _ipp._tcp.local (also query the
 * _universal._sub._ipp._tcp.local subtype).
 * total_ms: total scan duration (10000 recommended). Retransmit queries every second and
 * send targeted follow-up queries for discovered instances missing SRV/A records. Returns
 * the number of results written to out (0=none found), <0=serious error.
 * Interface selection (ENODEV fix): explicit ifname > wlan0 > other non-loopback > loopback.
 * After joining the multicast group, set IP_MULTICAST_IF to the same interface used to query/receive. */
int mdns_discover_ipp(mdns_service_t *out, int max_out, int total_ms);

/* As above, with an explicit multicast egress interface ifname
 * (NULL=choose automatically by priority; usually pass "wlan0"). */
int mdns_discover_ipp_if(mdns_service_t *out, int max_out, int total_ms, const char *ifname);

/* Unicast DNS query (for integration/encoder-decoder unit tests, e.g.
 * server="8.8.8.8", port=53).
 * qtype: 1=A 28=AAAA 12=PTR 33=SRV 16=TXT。
 * Returns response length (>0 on success), or an error code <0: -1 parameter/network,
 * -2 timeout, -3 response too long/truncated. */
int mdns_query_unicast(const char *server_ip, int port, const char *qname,
                       int qtype, unsigned char *ans, int ans_max, int timeout_ms);

/* ---------- DNS message parsing (shared by multicast/unicast responses) ---------- */

typedef struct {
    const char *name;              /* RR NAME (decompressed, e.g. "_ipp._tcp.local") */
    int  type;                     /* RR TYPE：1/28/12/33/16/5... */
    int  ttl;
    const unsigned char *rdata;    /* Raw RDATA */
    int  rlen;
    char text[256];                /* Text form: A/AAAA→IP, PTR/CNAME/NS/SRV→name, TXT→k=v;... */
    int  srv_port;                 /* SRV only: decoded port */
} mdns_rr_t;

/* Invoke callback for each resource record in answer/authority/additional sections;
 * returns 0=parsed successfully, <0=malformed message. cb may be NULL. */
typedef void (*mdns_rr_cb)(void *ud, const mdns_rr_t *rr);
int mdns_parse_reply(const unsigned char *pkt, int len, mdns_rr_cb cb, void *ud);

/* Constants */
#define MDNS_T_A     1
#define MDNS_T_PTR   12
#define MDNS_T_TXT   16
#define MDNS_T_AAAA  28
#define MDNS_T_SRV   33
#define MDNS_T_CNAME 5

#ifdef __cplusplus
}
#endif
#endif /* X2P_MDNS_H */
