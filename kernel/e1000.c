/* ============================================================
 * NovaOS - Intel 82540EM (e1000) driver, QEMU's default NIC
 *
 * QEMU pc machine puts the e1000 at PCI 00:03.0 with BAR0 =
 * MMIO registers. Identity paging only covers 0-8MB plus the
 * LFB, so the BAR is mapped through page_map_device() at init
 * (call only AFTER ring3_init turned paging on).
 *
 * Everything is polled: the RX ring is drained one frame per
 * e1000_recv() call and the TX path waits for writeback, so no
 * interrupt handler is needed (IMS stays 0).
 * ============================================================ */
#include "e1000.h"
#include "stdint.h"

/* ---- port I/O (file-local, mirrors kernel.c helpers) ---- */
static inline void outl_(uint16_t p, uint32_t v)
{ __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint32_t inl_(uint16_t p)
{ uint32_t v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

/* ---- PCI config (bus 0 only - all QEMU devices live there) ---- */
static uint32_t pci_read32(uint8_t dev, uint8_t fn, uint8_t off)
{
    outl_(0xCF8, 0x80000000u | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC));
    return inl_(0xCFC);
}
static void pci_write32(uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    outl_(0xCF8, 0x80000000u | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC));
    outl_(0xCFC, v);
}

/* ---- device registers (MMIO, dword stride) ---- */
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMS    0x00D0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_RAL    0x5400
#define REG_RAH    0x5404

/* ring sizes: TX is an 8-deep ring (RDLEN/TDLEN must be multiples of
 * 128 B - QEMU masks the low 7 bits, so 4 descs would truncate to 0);
 * RX keeps 16 buffers so a burst survives the time between two
 * net_poll() passes */
#define TX_N   8
#define RX_N   16
#define FRM_MAX 2048

/* legacy descriptor: 64-bit buffer address, then length/cso/cmd and
 * the writeback dword (status/css/special) - 16 B total */
typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;      /* TX: EOP|IFCS|RS   */
    uint8_t  status;   /* DD = bit0         */
    uint8_t  css;
    uint16_t special;
} desc_t;              /* 16 B, legacy format */

static volatile uint32_t *regs;      /* MMIO after mapping            */
static uint8_t  nic_ok;
static uint8_t  mac[6];
static uint32_t mmio_phys;

static desc_t tx_ring[TX_N]  __attribute__((aligned(16)));
static desc_t rx_ring[RX_N]  __attribute__((aligned(16)));
static uint8_t tx_buf[TX_N][FRM_MAX] __attribute__((aligned(16)));
static uint8_t rx_buf[RX_N][FRM_MAX] __attribute__((aligned(16)));
static uint32_t tx_next, rx_next;

/* kernel memory below 16MB is identity-mapped, so VA == physical addr
 * for every buffer handed to the DMA engine */
#define PHYS(p)  ((uint32_t)(unsigned long)(p))

static inline uint32_t rd(uint32_t r) { return regs[r >> 2]; }
static inline void     wr(uint32_t r, uint32_t v) { regs[r >> 2] = v; }

/* paging.c: map [phys, phys+len) with 4MB PDEs (supervisor, no cache);
 * identity mapping keeps VA == PA so descriptor addresses stay simple */
extern uint32_t page_map_device(uint32_t phys, uint32_t len);

/* tick source for send timeout (paging.c, 10 ms units) */
extern uint32_t proc_ticks(void);

/* ---- EEPROM word read: START(bit0) | word<<2, DONE bit1, data>>16 ---- */
static uint16_t eerd_read(int word)
{
    wr(REG_EERD, 0x1u | ((uint32_t)word << 2));
    for (int i = 0; i < 1000; i++)
        if (rd(REG_EERD) & 0x2u)
            return (uint16_t)(rd(REG_EERD) >> 16);
    return 0xFFFF;
}

static void read_mac(void)
{
    uint16_t w0 = eerd_read(0), w1 = eerd_read(1), w2 = eerd_read(2);
    if (w0 != 0xFFFF) {
        mac[0] = (uint8_t)w0;        mac[1] = (uint8_t)(w0 >> 8);
        mac[2] = (uint8_t)w1;        mac[3] = (uint8_t)(w1 >> 8);
        mac[4] = (uint8_t)w2;        mac[5] = (uint8_t)(w2 >> 8);
        if (w0 | w1 | w2) return;
    }
    /* fallback: the RA0 register always holds the current MAC in QEMU */
    uint32_t lo = rd(REG_RAL), hi = rd(REG_RAH);
    mac[0] = (uint8_t)lo;  mac[1] = (uint8_t)(lo >> 8);
    mac[2] = (uint8_t)(lo >> 16); mac[3] = (uint8_t)(lo >> 24);
    mac[4] = (uint8_t)hi;  mac[5] = (uint8_t)(hi >> 8);
}

/* scan bus 0 for 8086:100E; returns dev<<8 | fn, -1 = none */
static int pci_find_nic(void)
{
    for (int dev = 0; dev < 32; dev++) {
        if (pci_read32((uint8_t)dev, 0, 0x00) == 0xFFFFFFFFu) continue;
        uint32_t id = pci_read32((uint8_t)dev, 0, 0x00);
        if ((id & 0xFFFFu) != 0x8086u || (id >> 16) != 0x100Eu) continue;
        return dev << 8;
    }
    return -1;
}

int e1000_init(void)
{
    nic_ok = 0;
    int hit = pci_find_nic();
    if (hit < 0) return 0;
    uint8_t dev = (uint8_t)(hit >> 8);

    /* enable MEM space + bus mastering (the NIC DMAs into our rings) */
    uint32_t cmd = pci_read32(dev, 0, 0x04);
    cmd |= 0x7u;                      /* IO | MEM | BUSMASTER */
    pci_write32(dev, 0, 0x04, cmd);

    uint32_t bar = pci_read32(dev, 0, 0x10);
    if (bar & 1) return 0;            /* IO BAR - not what we want */
    bar &= 0xFFFFFFF0u;
    if (!bar) return 0;
    mmio_phys = bar;

    regs = (volatile uint32_t *)(unsigned long)page_map_device(bar, 0x20000u);
    if (!regs) return 0;

    wr(REG_IMC, 0xFFFFFFFFu);         /* polled mode: no interrupts */
    wr(REG_ICR, 0xFFFFFFFFu);         /* clear anything pending     */

    read_mac();

    /* link up, reset the ring state */
    wr(REG_CTRL, rd(REG_CTRL) | 0x40u);          /* SLU */

    tx_next = rx_next = 0;
    for (int i = 0; i < TX_N; i++) {
        tx_ring[i].addr   = PHYS(tx_buf[i]);
        tx_ring[i].status = 0x01;                 /* mark idle */
        tx_ring[i].cmd    = 0;
        tx_ring[i].length = 0;
    }
    for (int i = 0; i < RX_N; i++) {
        rx_ring[i].addr   = PHYS(rx_buf[i]);
        rx_ring[i].status = 0;
        rx_ring[i].length = 0;
    }

    /* TX: descriptor ring + standard duplex/timing knobs */
    wr(REG_TDBAL, PHYS(tx_ring));
    wr(REG_TDBAH, 0);
    wr(REG_TDLEN, TX_N * 16);
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    wr(REG_TCTL, 0x2u | 0x8u | (0x10u << 12) | (0x40u << 26));  /* EN|PSP|CT|COLD */

    /* RX: ring ready, accept unicast/multicast/broadcast, 2048B bufs */
    wr(REG_RDBAL, PHYS(rx_ring));
    wr(REG_RDBAH, 0);
    wr(REG_RDLEN, RX_N * 16);
    wr(REG_RDH, 0);
    wr(REG_RDT, RX_N - 1);
    wr(REG_RCTL, 0x2u | 0x8u | 0x10u | 0x8000u);  /* EN|UPE|MPE|BAM */

    nic_ok = 1;
    return 1;
}

int e1000_present(void)   { return nic_ok; }
const uint8_t *e1000_mac(void) { return mac; }
uint32_t e1000_mmio_base(void) { return mmio_phys; }

int e1000_send(const uint8_t *frame, uint32_t len)
{
    if (!nic_ok) return -1;
    if (!frame || len == 0 || len > FRM_MAX) return -1;

    uint32_t i = tx_next;
    /* wait until this descriptor finished its previous job */
    uint32_t t0 = proc_ticks();
    while (!(tx_ring[i].status & 0x01)) {
        if (proc_ticks() - t0 > 200) return -1;   /* 2 s: dead NIC */
    }

    const uint8_t *src = frame;
    uint8_t *dst = tx_buf[i];
    for (uint32_t k = 0; k < len; k++) dst[k] = src[k];

    tx_ring[i].addr   = PHYS(tx_buf[i]);
    tx_ring[i].length = (uint16_t)len;
    tx_ring[i].cso    = 0;
    tx_ring[i].cmd    = 0x0Bu;        /* EOP | IFCS | RS (report status) */
    tx_ring[i].css    = 0;
    tx_ring[i].special  = 0;
    tx_ring[i].status  = 0;

    tx_next = (i + 1) % TX_N;
    wr(REG_TDT, tx_next);

    /* QEMU completes the DMA synchronously; a real NIC needs the wait */
    t0 = proc_ticks();
    while (!(tx_ring[i].status & 0x01)) {
        if (proc_ticks() - t0 > 200) return -1;
    }
    return 0;
}

/* copy out one received frame (0 = ring empty); the descriptor is
 * re-armed immediately and released back to the hardware via RDT.
 * QEMU fills descriptors while RDH != RDT, so RDT = index of the
 * descriptor just consumed: hardware may then refill everything
 * ahead of it (exclusive), one slot behind the software pointer. */
int e1000_recv(uint8_t *frame, uint32_t max)
{
    if (!nic_ok || !frame) return -1;

    uint32_t i = rx_next;
    if (!(rx_ring[i].status & 0x01)) return 0;    /* no DD yet */

    uint32_t len = rx_ring[i].length;
    if (len > FRM_MAX) len = FRM_MAX;
    uint32_t n = (len < max) ? len : max;
    const uint8_t *src = rx_buf[i];
    for (uint32_t k = 0; k < n; k++) frame[k] = src[k];

    rx_ring[i].status = 0;
    rx_ring[i].length = 0;
    wr(REG_RDT, i);
    rx_next = (i + 1) % RX_N;
    return (int)n;
}
