/* ============================================================
 * NovaOS - Intel 82540EM (QEMU e1000) NIC driver
 *
 *   - PCI scan for 8086:100E on bus 0 (QEMU default NIC slot)
 *   - MMIO BAR mapped through paging.c (4MB PDE, cache-disabled)
 *   - legacy descriptors, polled RX (no interrupts, IMS=0)
 *   - one frame per recv() call; send() waits for completion
 * ============================================================ */
#ifndef E1000_H
#define E1000_H

#include "stdint.h"

int  e1000_init(void);        /* 1 = NIC up, 0 = not found             */
int  e1000_present(void);
const uint8_t *e1000_mac(void);              /* 6 bytes                */
uint32_t e1000_mmio_base(void);              /* debug: mapped MMIO VA  */
int  e1000_send(const uint8_t *frame, uint32_t len);   /* 0 = ok       */
int  e1000_recv(uint8_t *frame, uint32_t max);         /* len or 0     */

#endif
