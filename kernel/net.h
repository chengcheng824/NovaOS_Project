/* ============================================================
 * NovaOS - minimal TCP/IP stack over the e1000 driver
 *
 *   ARP + IPv4 + ICMP echo + UDP (DHCP/DNS/echo) - all polled,
 *   no threads: every command drives net_poll() in its wait loop.
 *   On-link traffic only (QEMU user networking = one /24), no
 *   gateway routing, no TCP.
 * ============================================================ */
#ifndef NET_H
#define NET_H

#include "stdint.h"

/* probe the NIC (call once from kmain, after ring3_init turned
 * paging on - the e1000 MMIO BAR needs a mapping). 1 = found. */
int  net_init(void);
int  net_present(void);

/* shell commands (also reachable from nsh.nxp via sysop ops 12-17) */
void cmd_netinfo(void);
void cmd_net_dhcp(void);
void cmd_net_ping(const char *args);
void cmd_net_dns(const char *args);
void cmd_net_udpecho(void);
void cmd_wget(const char *args);      /* wget HOST[:PORT] [/PATH] */

#endif
