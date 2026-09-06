/* ============================================================
 * NovaOS - minimal TCP/IP stack: ARP / IPv4 / ICMP / UDP
 *
 * Design: everything is driven by net_poll() from the command
 * that is waiting (ping/dhcp/dns/udpecho each poll in a ticks-
 * based loop). Frames are built into one static staging buffer;
 * received frames are processed one at a time out of the e1000
 * RX ring, so no queues are needed beyond one deferred UDP echo
 * reply (arp resolution may not complete inside a poll pass).
 *
 * Scope: on-link destinations only (QEMU user networking is a
 * single /24); no IP routing through the gateway, no TCP.
 * ============================================================ */
#include "e1000.h"
#include "net.h"
#include "stdint.h"

/* console output (kernel.c; dec/ip/mac printers are local below) */
extern void kput(char c);
extern void kputs(const char *s);
extern int  shell_kb_poll(void);    /* non-blocking key read, kernel.c */

extern uint32_t proc_ticks(void);   /* paging.c, 10 ms since boot */

void net_poll(void);                /* defined at the bottom */

/* ---------------- little helpers ---------------- */
static void mcpy(uint8_t *d, const uint8_t *s, uint32_t n)
{ while (n--) *d++ = *s++; }
static void mset(uint8_t *d, uint8_t v, uint32_t n)
{ while (n--) *d++ = v; }

static void st16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void st32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static uint16_t ld16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t ld32(const uint8_t *p)
{ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

static uint16_t cksum16(const uint8_t *p, uint32_t n)
{
    uint32_t s = 0;
    while (n > 1) { s += (uint32_t)p[0] << 8 | p[1]; p += 2; n -= 2; }
    if (n) s += (uint32_t)p[0] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

static void pr_dec(uint32_t v)
{
    char b[11]; int i = 0;
    if (!v) { kput('0'); return; }
    while (v) { b[i++] = (char)('0' + v % 10); v /= 10; }
    while (i--) kput(b[i]);
}
/* every IPv4 address lives in a u32 as on the wire: first octet in the
 * TOP byte (10.0.2.2 = 0x0A000202), matching st32/ld32 */
static void pr_ip(uint32_t ip)
{
    pr_dec((ip >> 24) & 0xFF); kput('.');
    pr_dec((ip >> 16) & 0xFF); kput('.');
    pr_dec((ip >> 8) & 0xFF);  kput('.');
    pr_dec(ip & 0xFF);
}
static void pr_mac(const uint8_t *m)
{
    static const char *hx = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        if (i) kput(':');
        kput(hx[m[i] >> 4]); kput(hx[m[i] & 0xF]);
    }
}

/* parse "a.b.c.d" (1 = ok) */
static int parse_ip(const char *s, uint32_t *out)
{
    uint32_t ip = 0; int part = 0, dots = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            part = part * 10 + (*s - '0');
            if (part > 255) return 0;
            s++;
        } else if (*s == '.') {
            if (dots == 3) return 0;
            ip = (ip << 8) | (uint32_t)part;
            part = 0; dots++; s++;
        } else return 0;
    }
    if (dots != 3) return 0;
    ip = (ip << 8) | (uint32_t)part;
    *out = ip;
    return 1;
}

/* ---------------- state ---------------- */
#define ET_IPV4 0x0800
#define ET_ARP  0x0806

static uint8_t  net_ok;
static uint8_t  mac[6];
static uint32_t g_ip, g_mask, g_gw, g_dns;
static uint8_t  g_ip_set;
static uint32_t g_xid   = 0x4E4F5631;   /* 'NOV1' */
static uint16_t g_ipid  = 1;
static uint16_t g_dnsid = 1;

/* one staging area for TX frame building */
static uint8_t tx_frame[1536];          /* eth hdr @0, payload @14 */
static uint8_t rx_frame[1536];

static inline uint8_t *tx_payload(void) { return tx_frame + 14; }

static int tx_go(const uint8_t *dst6, uint16_t etype, uint32_t paylen)
{
    mcpy(tx_frame, dst6, 6);
    mcpy(tx_frame + 6, mac, 6);
    tx_frame[12] = (uint8_t)(etype >> 8);
    tx_frame[13] = (uint8_t)etype;
    return e1000_send(tx_frame, 14 + paylen);
}

/* ---------------- ARP ---------------- */
#define ARP_SLOTS 8
static uint32_t arp_ip[ARP_SLOTS];
static uint8_t  arp_mac[ARP_SLOTS][6];
static uint32_t arp_tick[ARP_SLOTS];

static int arp_find(uint32_t ip, uint8_t *out)
{
    for (int i = 0; i < ARP_SLOTS; i++)
        if (arp_ip[i] == ip) { mcpy(out, arp_mac[i], 6); return 1; }
    return 0;
}
static void arp_put(uint32_t ip, const uint8_t *m)
{
    int slot = 0;
    for (int i = 0; i < ARP_SLOTS; i++) {
        if (arp_ip[i] == ip) { slot = i; goto hit; }
        if (!arp_ip[i] || !arp_tick[i]) { slot = i; goto hit; }
        if (arp_tick[i] < arp_tick[slot]) slot = i;
    }
hit:
    arp_ip[slot] = ip;
    mcpy(arp_mac[slot], m, 6);
    arp_tick[slot] = proc_ticks() + 1;
}

static const uint8_t bcast6[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static void arp_request(uint32_t tpa)
{
    uint8_t *p = tx_payload();
    st16(p, 1); st16(p + 2, ET_IPV4);
    p[4] = 6; p[5] = 4; st16(p + 6, 1);
    mcpy(p + 8, mac, 6);
    st32(p + 14, g_ip_set ? g_ip : 0);
    mset(p + 18, 0, 6);
    st32(p + 24, tpa);
    tx_go(bcast6, ET_ARP, 28);
}

static void arp_reply(uint32_t tpa, const uint8_t *tha)
{
    uint8_t *p = tx_payload();
    st16(p, 1); st16(p + 2, ET_IPV4);
    p[4] = 6; p[5] = 4; st16(p + 6, 2);
    mcpy(p + 8, mac, 6);
    st32(p + 14, tpa);
    mcpy(p + 18, tha, 6);
    st32(p + 24, g_ip);
    tx_go(tha, ET_ARP, 28);
}

static void arp_in(const uint8_t *p)
{
    uint16_t op  = ld16(p + 6);
    uint32_t spa = ld32(p + 14);
    const uint8_t *sha = p + 8;
    uint32_t tpa = ld32(p + 24);
    if (spa) arp_put(spa, sha);
    /* g_ip also holds the OFFERED lease while the DHCP handshake is in
     * flight (g_ip_set still 0): SLIRP unicasts the ACK / re-offers to
     * 10.0.2.15 and ARPs for us first - answer or the reply is dropped */
    if (op == 1 && g_ip && tpa == g_ip) arp_reply(tpa, sha);
}

/* resolve an on-link IP to a MAC (blocking, ~1.2 s worst case) */
static int arp_resolve(uint32_t ip, uint8_t *out)
{
    if (arp_find(ip, out)) return 1;
    if (!net_ok) return 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        arp_request(ip);
        uint32_t t0 = proc_ticks();
        while (proc_ticks() - t0 < 40) {
            net_poll();
            if (arp_find(ip, out)) return 1;
        }
    }
    return 0;
}

/* ---------------- IPv4 ---------------- */
static void icmp_in(const uint8_t *p, uint32_t len, uint32_t src);
static void udp_in(const uint8_t *p, uint32_t len, uint32_t src);
static void tcp_in(const uint8_t *p, uint32_t len, uint32_t src);

static void ip_in(const uint8_t *p, uint32_t len)
{
    if (len < 20) return;
    if ((p[0] >> 4) != 4) return;
    uint32_t ihl  = (uint32_t)(p[0] & 0xF) * 4;
    if (ihl < 20 || len < ihl) return;
    uint32_t tot  = ld16(p + 2);
    if (tot > len) tot = len;
    uint32_t src  = ld32(p + 12), dst = ld32(p + 16);
    uint8_t  proto = p[9];

    int mine = (dst == g_ip) || (dst == 0xFFFFFFFFu);
    int dhcp_ok = (!g_ip_set && proto == 17);   /* DHCP before we own an IP */
    if (!mine && !dhcp_ok) return;

    if (proto == 1)  icmp_in(p + ihl, tot - ihl, src);
    if (proto == 6)  tcp_in(p + ihl, tot - ihl, src);
    if (proto == 17) udp_in(p + ihl, tot - ihl, src);
}

static int ip_send(uint32_t dst, uint8_t proto, const uint8_t *payload, uint16_t len)
{
    uint8_t m6[6];
    if (dst == 0xFFFFFFFFu) mcpy(m6, bcast6, 6);      /* no ARP for broadcast */
    else if (!arp_resolve(dst, m6)) return -1;
    uint8_t *p = tx_payload();
    p[0] = 0x45; p[1] = 0;
    st16(p + 2, (uint16_t)(20 + len));
    st16(p + 4, g_ipid++);
    st16(p + 6, 0);
    p[8] = 64; p[9] = proto;
    st16(p + 10, 0);
    st32(p + 12, g_ip_set ? g_ip : 0);
    st32(p + 16, dst);
    st16(p + 10, cksum16(p, 20));
    mcpy(p + 20, payload, len);
    return tx_go(m6, ET_IPV4, 20 + len);
}

/* ---------------- ICMP ---------------- */
static uint8_t  ping_wait;
static uint32_t ping_ip, ping_seq, ping_t0;
static uint16_t ping_id;
static int32_t  ping_rtt;
static uint8_t  ping_unreach;

static uint8_t icmp_buf[96];

static void icmp_in(const uint8_t *p, uint32_t len, uint32_t src)
{
    if (len < 8) return;
    uint8_t type = p[0];
    if (type == 8 && g_ip_set && len <= sizeof icmp_buf) {
        /* echo request -> reply. staged in icmp_buf because ip_send's
         * payload copy overlaps tx_frame otherwise (dst > src, forward) */
        mcpy(icmp_buf, p, len);
        icmp_buf[0] = 0;
        st16(icmp_buf + 2, 0);
        st16(icmp_buf + 2, cksum16(icmp_buf, len));
        ip_send(src, 1, icmp_buf, (uint16_t)len);
        return;
    }
    if (type == 0 && ping_wait && src == ping_ip &&
        ld16(p + 4) == ping_id && ld16(p + 6) == ping_seq) {
        ping_rtt = (int32_t)(proc_ticks() - ping_t0) * 10;
        ping_wait = 0;
    }
    if (type == 3 && ping_wait && src == ping_ip) {
        ping_unreach = 1;
        ping_wait = 0;
    }
}

static int icmp_echo(uint32_t dst, uint16_t id, uint16_t seq)
{
    uint8_t buf[8 + 32];
    buf[0] = 8; buf[1] = 0;
    st16(buf + 2, 0);
    st16(buf + 4, id);
    st16(buf + 6, seq);
    for (int i = 0; i < 32; i++) buf[8 + i] = (uint8_t)('A' + (i & 0x1F));
    st16(buf + 2, cksum16(buf, sizeof buf));
    return ip_send(dst, 1, buf, sizeof buf);
}

/* ---------------- UDP ---------------- */
static uint8_t udp_seg[1546];

static int udp_send(uint32_t dst, uint16_t sport, uint16_t dport,
                    const uint8_t *payload, uint16_t len)
{
    if (8 + len > sizeof udp_seg) return -1;
    st16(udp_seg, sport);
    st16(udp_seg + 2, dport);
    st16(udp_seg + 4, (uint16_t)(8 + len));
    st16(udp_seg + 6, 0);                 /* checksum 0 = "not computed" */
    mcpy(udp_seg + 8, payload, len);
    return ip_send(dst, 17, udp_seg, (uint16_t)(8 + len));
}

/* ---- DHCP ---- */
#define DHCP_MAGIC 0x63825363u

static uint8_t  dhcp_offer, dhcp_ack;
static uint32_t offer_ip, offer_srv;
static uint32_t ack_ip, ack_mask, ack_gw, ack_dns;

static void dhcp_opt(const uint8_t *p, uint32_t len)
{
    uint32_t i = 4;                                  /* skip magic cookie */
    while (i < len) {
        uint8_t code = p[i++];
        if (code == 0) continue;
        if (code == 255) break;
        if (i >= len) break;
        uint8_t vl = p[i++];
        if (i + vl > len) break;
        if (code == 53 && vl == 1) {
            if (p[i] == 2) dhcp_offer = 1;
            if (p[i] == 5) dhcp_ack = 1;
        } else if (code == 54 && vl == 4) offer_srv = ld32(p + i);
        else if (dhcp_ack) {
            if (code == 1 && vl == 4)  ack_mask = ld32(p + i);
            if (code == 3 && vl == 4)  ack_gw   = ld32(p + i);
            if (code == 6 && vl == 4)  ack_dns  = ld32(p + i);
        }
        i += vl;
    }
}

static void udp_dhcp_in(const uint8_t *p, uint32_t len, uint32_t src)
{
    (void)src;
    if (len < 240) return;
    if (p[0] != 2) return;                           /* BOOTREPLY */
    if (ld32(p + 4) != g_xid) return;
    if (ld32(p + 236) != DHCP_MAGIC) return;
    uint32_t yi = ld32(p + 16);
    dhcp_opt(p + 236, len - 236);
    if (dhcp_offer && !offer_ip) offer_ip = yi;
    if (dhcp_ack) ack_ip = yi;
}

static void dhcp_build(uint8_t msg, uint32_t reqip, uint32_t srvip)
{
    uint8_t body[320];
    mset(body, 0, 300);
    body[0] = 1; body[1] = 1; body[2] = 6; body[3] = 0;   /* BOOTREQUEST, eth */
    st32(body + 4, g_xid);
    st16(body + 10, 0x8000);                          /* broadcast flag */
    mcpy(body + 28, mac, 6);
    st32(body + 236, DHCP_MAGIC);
    uint8_t *o = body + 240;
    *o++ = 53; *o++ = 1; *o++ = msg;
    if (msg == 3) {
        *o++ = 50; *o++ = 4; st32(o, reqip); o += 4;
        *o++ = 54; *o++ = 4; st32(o, srvip); o += 4;
    }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;    /* mask, router, dns */
    *o++ = 255;
    udp_send(0xFFFFFFFFu, 68, 67, body, 300);
}

/* wait for a flag with polling; returns when flag set or timeout */
static int dhcp_wait(uint8_t *flag, uint32_t ticks)
{
    uint32_t t0 = proc_ticks();
    while (proc_ticks() - t0 < ticks) {
        net_poll();
        if (*flag) return 1;
    }
    return 0;
}

static int dhcp_run(void)
{
    if (!net_ok) { kputs("dhcp: no NIC\n"); return -1; }

    for (int attempt = 0; attempt < 2; attempt++) {
        g_xid += 0x01010101u;
        dhcp_offer = dhcp_ack = 0;
        offer_ip = offer_srv = ack_ip = ack_mask = ack_gw = ack_dns = 0;

        dhcp_build(1, 0, 0);                          /* DISCOVER */
        if (dhcp_wait(&dhcp_offer, 150)) break;
        if (attempt) { kputs("dhcp: no offer\n"); return -1; }
    }
    uint32_t yi = offer_ip, srv = offer_srv;
    if (!yi) { kputs("dhcp: no offer\n"); return -1; }
    g_ip = yi;                       /* tentative: answer ARP for it NOW,
                                        else SLIRP can't unicast the ACK */

    for (int attempt = 0; attempt < 2; attempt++) {
        dhcp_offer = dhcp_ack = 0;
        dhcp_build(3, yi, srv);                       /* REQUEST */
        if (dhcp_wait(&dhcp_ack, 150)) break;
        if (attempt) { kputs("dhcp: no ack\n"); return -1; }
    }
    if (!ack_ip) { kputs("dhcp: no ack\n"); return -1; }

    g_ip = ack_ip; g_ip_set = 1;
    g_mask = ack_mask; g_gw = ack_gw; g_dns = ack_dns;
    kputs("dhcp: IP ");    pr_ip(g_ip);
    if (g_mask) { kputs(" mask "); pr_ip(g_mask); }
    if (g_gw)   { kputs(" gw ");   pr_ip(g_gw); }
    if (g_dns)  { kputs(" dns ");  pr_ip(g_dns); }
    kput('\n');
    return 0;
}

/* ---- DNS ---- */
static uint8_t  dns_done;
static uint32_t dns_a;
static uint16_t dns_active_id;      /* id of the query in flight */

static void udp_dns_in(const uint8_t *p, uint32_t len)
{
    if (len < 20) return;
    if (ld16(p) != dns_active_id) return;
    if (!(ld16(p + 2) & 0x8000u)) return;             /* QR=1 */
    if (ld16(p + 2) & 0x000Fu) return;                /* rcode != 0 */
    uint32_t an = ld16(p + 6);
    uint32_t i = 12;
    for (uint32_t q = ld16(p + 4); q && i < len; q--) {   /* skip questions */
        while (i < len && p[i]) i += p[i] + 1;
        i += 5;
    }
    for (uint32_t a = 0; a < an && i + 12 <= len; a++) {
        if (p[i] & 0xC0) i += 2;                      /* compressed name */
        else { while (i < len && p[i]) i += p[i] + 1; i++; }
        uint16_t type = ld16(p + i);
        uint16_t rdlen = ld16(p + i + 8);
        if (type == 1 && rdlen == 4 && i + 10 + 4 <= len) {
            dns_a = ld32(p + i + 10);
            dns_done = 1;
            return;
        }
        i += 10 + rdlen;
    }
}

static int dns_query(const char *name)
{
    /* own buffer: udp_send writes the UDP header into udp_seg, so the
     * payload may never live inside udp_seg itself */
    static uint8_t dns_msg[512];
    uint16_t id = g_dnsid++;
    dns_active_id = id;
    uint8_t *q = dns_msg;
    int namelen = 0;
    while (name[namelen]) namelen++;
    if (12 + namelen + 2 + 4 > (int)sizeof dns_msg) return -1;
    st16(q, id);
    st16(q + 2, 0x0100);                              /* RD */
    st16(q + 4, 1); mset(q + 6, 0, 6);
    uint8_t *o = q + 12;
    const char *s = name;
    while (*s) {
        int l = 0;
        while (s[l] && s[l] != '.') l++;
        if (l == 0 || l > 63) break;
        *o++ = (uint8_t)l;
        for (int k = 0; k < l; k++) *o++ = (uint8_t)s[k];
        s += l;
        if (*s == '.') s++;
    }
    *o++ = 0;
    *o++ = 0; *o++ = 1;                               /* type A */
    *o++ = 0; *o++ = 1;                               /* class IN */
    return udp_send(g_dns, 0x1234, 53, q, (uint16_t)(o - q));
}

/* ---------------- UDP echo server (port 7777) ---------------- */
static uint8_t  echo_have;
static uint32_t echo_ip;
static uint16_t echo_port;
static uint16_t echo_len;
static uint8_t  echo_data[1400];

/* deferred one frame so arp resolution never runs inside net_poll */
static void echo_flush(void)
{
    echo_have = 0;
    if (!g_ip_set) return;
    uint8_t m6[6];
    if (!arp_resolve(echo_ip, m6)) return;
    udp_send(echo_ip, 7777, echo_port, echo_data, echo_len);
    kputs("[udp] echoed "); pr_dec(echo_len); kputs(" B to ");
    pr_ip(echo_ip); kput(':'); pr_dec(echo_port); kput('\n');
}

static void udp_in(const uint8_t *p, uint32_t len, uint32_t src)
{
    if (len < 8) return;
    uint16_t sp = ld16(p), dp = ld16(p + 2);
    uint32_t plen = ld16(p + 4);
    if (plen < 8 || plen > len) plen = len;
    plen -= 8;
    const uint8_t *pay = p + 8;

    if (dp == 68 && sp == 67) { udp_dhcp_in(pay, plen, src); return; }
    if (sp == 53)             { udp_dns_in(pay, plen);       return; }
    if (dp == 7777 && g_ip_set && !echo_have && plen <= sizeof echo_data) {
        echo_have = 1;
        echo_ip = src; echo_port = sp; echo_len = (uint16_t)plen;
        mcpy(echo_data, pay, plen);
    }
}

/* ---------------- poll ---------------- */
static int in_poll;

void net_poll(void)
{
    if (in_poll || !net_ok) return;
    in_poll = 1;
    int n;
    while ((n = e1000_recv(rx_frame, sizeof rx_frame)) > 0) {
        if (n < 14) continue;
        uint16_t et = ld16(rx_frame + 12);
        if (et == ET_ARP && n >= 14 + 28)       arp_in(rx_frame + 14);
        else if (et == ET_IPV4 && n >= 14 + 20) ip_in(rx_frame + 14, (uint32_t)n - 14);
    }
    in_poll = 0;
    if (echo_have) echo_flush();
}

/* ---------------- commands ---------------- */
static void net_ensure_ip(void)
{
    if (net_ok && !g_ip_set) dhcp_run();
}

int net_init(void)
{
    net_ok = 0; g_ip_set = 0;
    if (!e1000_init()) return 0;
    mcpy(mac, e1000_mac(), 6);
    net_ok = 1;
    return 1;
}

int net_present(void) { return net_ok; }

void cmd_netinfo(void)
{
    if (!net_ok) { kputs("netinfo: no NIC detected\n"); return; }
    net_ensure_ip();
    kputs("NIC   : Intel 82540EM (e1000)\n");
    kputs("MAC   : "); pr_mac(mac); kput('\n');
    if (!g_ip_set) { kputs("IP    : not configured (run 'dhcp')\n"); return; }
    kputs("IP    : "); pr_ip(g_ip); kput('\n');
    if (g_mask) { kputs("Mask  : "); pr_ip(g_mask); kput('\n'); }
    if (g_gw)   { kputs("GW    : "); pr_ip(g_gw);   kput('\n'); }
    if (g_dns)  { kputs("DNS   : "); pr_ip(g_dns);  kput('\n'); }
}

void cmd_net_dhcp(void)
{
    dhcp_run();
}

void cmd_net_ping(const char *args)
{
    if (!net_ok) { kputs("ping: no NIC\n"); return; }
    if (!args || !*args) { kputs("ping IP\n"); return; }
    net_ensure_ip();
    uint32_t ip;
    if (!parse_ip(args, &ip)) { kputs("ping: bad address\n"); return; }
    if (!g_ip_set) { kputs("ping: no IP (dhcp failed?)\n"); return; }

    uint8_t m6[6];
    if (!arp_resolve(ip, m6)) { kputs("ping: no ARP reply\n"); return; }

    int got = 0;
    for (uint16_t seq = 1; seq <= 4; seq++) {
        ping_wait = 1; ping_unreach = 0; ping_rtt = -1;
        ping_ip = ip; ping_id = 0x4E4F; ping_seq = seq;
        ping_t0 = proc_ticks();
        icmp_echo(ip, ping_id, seq);
        while (ping_wait && proc_ticks() - ping_t0 < 100) net_poll();
        if (ping_rtt >= 0) {
            got++;
            kputs("Reply from "); pr_ip(ip);
            kputs(": seq="); pr_dec(seq);
            kputs(" time="); pr_dec((uint32_t)ping_rtt); kputs(" ms\n");
        } else if (ping_unreach) {
            kputs("ICMP unreachable from "); pr_ip(ip); kput('\n');
        } else {
            kputs("Request timed out (seq="); pr_dec(seq); kputs(")\n");
        }
    }
    kputs("--- "); pr_dec((uint32_t)got); kputs("/4 received ---\n");
}

/* quiet A-record lookup via the configured DNS server; 0 = ok */
static int dns_resolve(const char *name, uint32_t *out)
{
    if (!net_ok) return -1;
    if (!g_dns) g_dns = 0x0A000203u;                  /* QEMU user-net DNS */
    dns_done = 0; dns_a = 0;
    if (dns_query(name) != 0) return -1;
    uint32_t t0 = proc_ticks();
    while (!dns_done && proc_ticks() - t0 < 600) net_poll();
    if (!dns_done) return -1;
    *out = dns_a;
    return 0;
}

void cmd_net_dns(const char *args)
{
    if (!net_ok) { kputs("dns: no NIC\n"); return; }
    if (!args || !*args) { kputs("dns NAME\n"); return; }
    net_ensure_ip();
    uint32_t ip;
    if (dns_resolve(args, &ip) != 0) { kputs("dns: no reply\n"); return; }
    kputs(args); kputs(" -> "); pr_ip(ip); kput('\n');
}

void cmd_net_udpecho(void)
{
    if (!net_ok) { kputs("udpecho: no NIC\n"); return; }
    net_ensure_ip();
    kputs("UDP echo server on port 7777 - press q to stop\n");
    for (;;) {
        net_poll();
        int c = shell_kb_poll();
        if (c == 'q' || c == 'Q' || c == 27) break;
    }
    kputs("UDP echo off\n");
}

/* ---------------- TCP (minimal client, ONE connection) ----------------
 * Just enough for HTTP: SYN handshake, in-order payload, cumulative
 * ACKs, FIN close, a 500 ms retransmit timer. A single TCB - the shell
 * is single-threaded and wget is its only user. rcv window is kept at
 * 4 KB so SLIRP never outruns the 8 KB receive ring between polls. */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define TCPST_CLOSED   0
#define TCPST_SYN_SENT 1
#define TCPST_ESTAB    2
#define TCPST_FIN_SENT 3          /* our FIN is out, want the last ACK */
#define TCPST_DEAD     4          /* RST, handshake timeout, or done    */

static struct {
    uint8_t  used, state, fin_rx;
    uint32_t rip;
    uint16_t rport, lport;
    uint32_t snd_nxt;             /* next seq we will send            */
    uint32_t snd_acked;           /* highest ackn the peer returned   */
    uint32_t rcv_nxt;             /* next seq we expect from peer     */
    uint32_t fin_end;             /* seq past the peer's FIN          */
} tcb;

static uint8_t tcp_rbuf[16384];   /* received stream (ring)           */
static volatile uint16_t tcp_rh, tcp_rt;

static uint32_t tcp_rpush(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint16_t nx = (uint16_t)((tcp_rh + 1) % sizeof tcp_rbuf);
        if (nx == tcp_rt) break;  /* ring full: leave unacked, peer retries */
        tcp_rbuf[tcp_rh] = p[i];
        tcp_rh = nx;
    }
    return i;
}

static uint32_t tcp_rpop(uint8_t *out, uint32_t max)
{
    uint32_t n = 0;
    while (tcp_rt != tcp_rh && n < max) {
        out[n++] = tcp_rbuf[tcp_rt];
        tcp_rt = (uint16_t)((tcp_rt + 1) % sizeof tcp_rbuf);
    }
    return n;
}

/* checksum over the TCP segment plus the IPv4 pseudo header */
static uint16_t tcp_cksum(const uint8_t *h, uint32_t len)
{
    uint32_t s = (g_ip >> 16) + (g_ip & 0xFFFF) +
                 (tcb.rip >> 16) + (tcb.rip & 0xFFFF) + 6 + len;
    for (uint32_t i = 0; i + 1 < len; i += 2) s += (uint32_t)h[i] << 8 | h[i + 1];
    if (len & 1) s += (uint32_t)h[len - 1] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

static uint8_t tcp_seg[1536];

static int tcp_tx(uint8_t flags, const uint8_t *payload, uint32_t len,
                  uint32_t seq, uint32_t ackn)
{
    if (len > 1400) return -1;
    uint8_t *p = tcp_seg;
    st16(p, tcb.lport); st16(p + 2, tcb.rport);
    st32(p + 4, seq); st32(p + 8, ackn);
    p[12] = 0x50;                              /* data offset: 5 words */
    p[13] = flags;
    st16(p + 14, 4096);                        /* receive window       */
    st16(p + 16, 0); st16(p + 18, 0);
    for (uint32_t i = 0; i < len; i++) p[20 + i] = payload[i];
    st16(p + 16, tcp_cksum(p, 20 + len));
    return ip_send(tcb.rip, 6, p, (uint16_t)(20 + len));
}

static void tcp_in(const uint8_t *p, uint32_t len, uint32_t src)
{
    if (len < 20) return;
    if (!tcb.used || src != tcb.rip) return;
    uint16_t dp = ld16(p + 2);
    if (dp != tcb.lport) return;
    uint32_t seq = ld32(p + 4), ackn = ld32(p + 8);
    uint32_t doff = (uint32_t)(p[12] >> 4) * 4;
    uint8_t  fl = p[13];
    if (doff < 20 || doff > len) return;
    const uint8_t *pay = p + doff;
    uint32_t plen = len - doff;

    if (fl & TCP_RST) { tcb.state = TCPST_DEAD; return; }

    if (tcb.state == TCPST_SYN_SENT) {
        if ((fl & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
            ackn == tcb.snd_nxt) {             /* our SYN is acked     */
            tcb.rcv_nxt = seq + 1;             /* peer SYN consumes 1  */
            tcb.snd_acked = tcb.snd_nxt = ackn;
            tcp_tx(TCP_ACK, 0, 0, tcb.snd_nxt, tcb.rcv_nxt);
            tcb.state = TCPST_ESTAB;
        }
        return;
    }

    if ((fl & TCP_ACK) && (int32_t)(ackn - tcb.snd_acked) > 0)
        tcb.snd_acked = ackn;

    if (plen > 0) {
        /* accept only what fits in the ring and ack exactly that much;
         * the peer retransmits whatever we did not acknowledge (a
         * retransmit may overlap the already-acked prefix) */
        uint32_t take = 0;
        if (seq == tcb.rcv_nxt) {
            take = tcp_rpush(pay, plen);
        } else if ((int32_t)(seq - tcb.rcv_nxt) < 0 &&
                   (int32_t)(seq + plen - tcb.rcv_nxt) > 0) {
            uint32_t skip = tcb.rcv_nxt - seq;
            take = tcp_rpush(pay + skip, plen - skip);
        }
        tcb.rcv_nxt += take;
        tcp_tx(TCP_ACK, 0, 0, tcb.snd_nxt, tcb.rcv_nxt);
    }

    if (fl & TCP_FIN) {
        if (!tcb.fin_rx) tcb.fin_end = seq + plen + 1;
        tcb.fin_rx = 1;
        if (seq + plen + 1 == tcb.rcv_nxt)
            tcp_tx(TCP_ACK, 0, 0, tcb.snd_nxt, tcb.rcv_nxt);
    }

    if (tcb.state == TCPST_FIN_SENT && (fl & TCP_ACK) && ackn == tcb.snd_nxt)
        tcb.state = TCPST_DEAD;
}

static int tcp_connect(uint32_t ip, uint16_t port)
{
    if (!net_ok || !g_ip_set) return -1;
    if (tcb.used) return -1;

    tcb.used = 1; tcb.fin_rx = 0; tcb.fin_end = 0;
    tcb.rip = ip; tcb.rport = port;
    tcb.lport = (uint16_t)(0xC000 + (proc_ticks() & 0x1FFF));
    tcb.snd_nxt = proc_ticks() * 1103515245u + 12345u;
    tcb.snd_acked = 0; tcb.rcv_nxt = 0;
    tcp_rh = tcp_rt = 0;
    tcb.state = TCPST_SYN_SENT;

    tcp_tx(TCP_SYN, 0, 0, tcb.snd_nxt, 0);
    tcb.snd_nxt++;                             /* SYN consumes one seq */
    uint32_t t0 = proc_ticks(), t1 = t0;
    while (proc_ticks() - t0 < 500) {          /* 5 s handshake budget */
        net_poll();
        if (tcb.state == TCPST_ESTAB) return 0;
        if (tcb.state == TCPST_DEAD) { tcb.used = 0; return -1; }
        if (proc_ticks() - t1 >= 50) {         /* retransmit the SYN   */
            t1 = proc_ticks();
            tcp_tx(TCP_SYN, 0, 0, tcb.snd_nxt - 1, 0);
        }
    }
    tcb.used = 0; tcb.state = TCPST_CLOSED;
    return -1;
}

/* one segment, then wait (and retransmit) until fully acked */
static int tcp_send_data(const uint8_t *data, uint32_t len)
{
    if (tcb.state != TCPST_ESTAB || len == 0 || len > 1400) return -1;
    uint32_t seq = tcb.snd_nxt;
    tcb.snd_nxt += len;
    uint32_t t0 = proc_ticks(), t1 = t0;
    int tries = 0;
    tcp_tx(TCP_ACK | TCP_PSH, data, len, seq, tcb.rcv_nxt);
    for (;;) {
        net_poll();
        if ((int32_t)(tcb.snd_acked - (seq + len)) >= 0) return (int)len;
        if (tcb.state == TCPST_DEAD) return -1;
        if (proc_ticks() - t0 > 500) return -1;
        if (proc_ticks() - t1 >= 50) {         /* 500 ms retransmit    */
            t1 = proc_ticks();
            if (++tries > 4) return -1;
            tcp_tx(TCP_ACK | TCP_PSH, data, len, seq, tcb.rcv_nxt);
        }
    }
}

/* pull from the receive ring; >0 = bytes, 0 = idle timeout or EOF,
 * -1 = connection dead */
static int tcp_recv(uint8_t *out, uint32_t max, uint32_t timeout)
{
    uint32_t t0 = proc_ticks();
    for (;;) {
        uint32_t n = tcp_rpop(out, max);
        if (n) return (int)n;
        if (tcb.state == TCPST_DEAD) return -1;
        if (tcb.fin_rx && tcb.rcv_nxt == tcb.fin_end &&
            tcp_rt == tcp_rh) return 0;                        /* clean EOF */
        if (proc_ticks() - t0 >= timeout) return 0;
        net_poll();
    }
}

static void tcp_close(void)
{
    if (!tcb.used) return;
    if (tcb.state == TCPST_ESTAB) {
        uint32_t fseq = tcb.snd_nxt;
        tcb.snd_nxt++;                         /* FIN consumes one seq */
        tcp_tx(TCP_FIN | TCP_ACK, 0, 0, fseq, tcb.rcv_nxt);
        uint32_t t0 = proc_ticks();
        while (tcb.state == TCPST_ESTAB && proc_ticks() - t0 < 200)
            net_poll();
    }
    tcb.used = 0;
    tcb.state = TCPST_CLOSED;
}

extern int fs_write(const char *name, const uint8_t *data, uint32_t len);

/* wget HOST[:PORT] [/PATH] - HTTP/1.0 GET, body saved into NovaFS */
void cmd_wget(const char *args)
{
    if (!net_ok) { kputs("wget: no NIC\n"); return; }
    if (!args || !*args) { kputs("wget HOST[:PORT] [/PATH]\n"); return; }
    net_ensure_ip();
    if (!g_ip_set) { kputs("wget: no IP (dhcp failed?)\n"); return; }

    char host[64];
    int hi = 0;
    const char *s = args;
    while (*s && *s != ' ' && *s != '/' && hi < 63) host[hi++] = *s++;
    host[hi] = 0;
    while (*s == ' ') s++;                       /* tolerate "host path" */
    char path[160];
    int pk = 0;
    if (*s == '/') { while (*s && pk < 158) path[pk++] = *s++; }
    else if (*s) { path[pk++] = '/'; while (*s && pk < 158) path[pk++] = *s++; }
    else path[pk++] = '/';
    path[pk] = 0;
    if (!hi) { kputs("wget: no host\n"); return; }

    uint16_t port = 80;
    char *colon = 0;
    for (int i = 0; host[i]; i++) if (host[i] == ':') colon = host + i;
    if (colon) {
        uint32_t v = 0;
        const char *q = colon + 1;
        while (*q >= '0' && *q <= '9') v = v * 10 + (uint32_t)(*q++ - '0');
        if (*q || !v || v > 65535) { kputs("wget: bad port\n"); return; }
        port = (uint16_t)v;
        *colon = 0;
    }

    uint32_t ip;
    if (!parse_ip(host, &ip) && dns_resolve(host, &ip) != 0) {
        kputs("wget: cannot resolve host\n");
        return;
    }

    kputs("wget: connect "); pr_ip(ip); kput(':'); pr_dec(port); kput('\n');
    if (tcp_connect(ip, port) != 0) { kputs("wget: no SYN-ACK\n"); return; }

    static char req[384];
    char *e = req;
    {   /* build the request head */
        const char *l;
        l = "GET ";       while (*l) *e++ = *l++;
        l = path;         while (*l && e - req < 200) *e++ = *l++;
        l = " HTTP/1.0\r\nHost: "; while (*l && e - req < 240) *e++ = *l++;
        l = host;         while (*l && e - req < 300) *e++ = *l++;
        l = "\r\nUser-Agent: NovaOS\r\nConnection: close\r\n\r\n";
        while (*l && e - req < 380) *e++ = *l++;
        *e = 0;
    }
    if (tcp_send_data((const uint8_t *)req, (uint32_t)(e - req)) < 0) {
        kputs("wget: send failed\n");
        tcp_close();
        return;
    }

    static uint8_t resp[16384];
    uint32_t total = 0, t0 = proc_ticks();
    for (;;) {
        int n = tcp_recv(resp + total, (uint32_t)sizeof resp - total, 300);
        if (n > 0) {
            total += (uint32_t)n;
            t0 = proc_ticks();
            if (total >= sizeof resp) break;   /* buffer full */
        } else if (n < 0) break;               /* reset / dead */
        else if (tcb.fin_rx && tcb.rcv_nxt == tcb.fin_end &&
                 tcp_rt == tcp_rh) break;      /* whole stream is here */
        else if (total && proc_ticks() - t0 >= 150) break;   /* 1.5s idle */
        else if (!total && proc_ticks() - t0 >= 600) break;  /* 6s no data */
    }
    tcp_close();

    if (!total) { kputs("wget: no data\n"); return; }

    uint32_t hlen = 0;
    while (hlen + 4 <= total &&
           !(resp[hlen] == '\r' && resp[hlen + 1] == '\n' &&
             resp[hlen + 2] == '\r' && resp[hlen + 3] == '\n')) hlen++;
    if (hlen + 4 > total) hlen = total;
    kputs("HTTP: ");
    for (uint32_t i = 0; i < hlen && resp[i] >= ' '; i++) kput((char)resp[i]);
    kput('\n');

    uint32_t boff = (hlen + 4 <= total) ? hlen + 4 : total;
    uint32_t blen = total - boff;
    if (!blen) { kputs("wget: empty body\n"); return; }

    char name[24];
    const char *last = path;
    for (const char *q = path; *q; q++) if (*q == '/') last = q + 1;
    int nk = 0;
    while (last[nk] && last[nk] != '?' && nk < 23) name[nk] = last[nk], nk++;
    name[nk] = 0;
    if (!name[0]) { name[0]='i'; name[1]='n'; name[2]='d'; name[3]='e'; name[4]='x';
                    name[5]='.'; name[6]='h'; name[7]='t'; name[8]='m'; name[9]=0; }

    kputs("wget: "); pr_dec(blen); kputs(" B body -> ");
    if (fs_write(name, resp + boff, blen) >= 0) { kputs(name); kput('\n'); }
    else kputs("(fs write failed)\n");
}
