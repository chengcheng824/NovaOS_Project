/* ============================================================
 * NovaOS - ATA/IDE PIO driver (LBA28, primary master)
 * ============================================================ */
#include "stdint.h"
#include "ata.h"

#define ATA_DATA     0x1F0
#define ATA_ERROR    0x1F1
#define ATA_COUNT    0x1F2
#define ATA_LBA_LO   0x1F3
#define ATA_LBA_MID  0x1F4
#define ATA_LBA_HI   0x1F5
#define ATA_DRIVE    0x1F6
#define ATA_CMD      0x1F7
#define ATA_STATUS   0x1F7
#define ATA_ALTSTAT  0x3F6     /* Alternate Status (read) / Device Ctrl (write) */
#define ATA_DCR      0x3F6

#define ATA_CMD_READ  0x20
#define ATA_CMD_WRITE 0x30

#define ATA_SR_ERR   0x01
#define ATA_SR_DRQ   0x08
#define ATA_SR_BSY   0x80

#define ATA_DCR_SRST 0x04      /* Software Reset */
#define ATA_DCR_nIEN 0x02      /* Disable IRQ (we poll, so always set) */

static inline void outb(uint16_t p, uint8_t v){ __asm__ volatile ("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v){ __asm__ volatile ("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t  inb(uint16_t p){ uint8_t v; __asm__ volatile ("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p){ uint16_t v; __asm__ volatile ("inw %1,%0":"=a"(v):"Nd"(p)); return v; }

/* 400ns delay (ATA spec): two reads of Alternate Status */
static inline void ata_delay400(void){
    (void)inb(ATA_ALTSTAT);
    (void)inb(ATA_ALTSTAT);
}

/* bounded waits: never hang the kernel on a wedged drive (~1s each) */
#define ATA_TIMEOUT 500000     /* each poll already has ata_delay400 inside */

static int ata_wait_bsy(void){
    uint32_t t = ATA_TIMEOUT;
    while(1) {
        uint8_t s = inb(ATA_ALTSTAT);
        if(!(s & ATA_SR_BSY)) return 0;
        if(--t == 0) return -1;
        ata_delay400();
    }
}

static int ata_wait_drq(void){
    uint32_t t = ATA_TIMEOUT;
    while(1) {
        uint8_t s = inb(ATA_ALTSTAT);
        if(s & (ATA_SR_DRQ | ATA_SR_ERR)) return (s & ATA_SR_ERR) ? -1 : 0;
        if(--t == 0) return -1;
        ata_delay400();
    }
}

static int g_ata_inited = 0;

/* NovaFS lives on the primary SLAVE drive (a separate data disk), so
 * rebuilding the boot image never touches user accounts or files. */
static int g_slave = 0;
void ata_set_slave(int on)
{
    g_slave = on ? 1 : 0;
    g_ata_inited = 0;              /* force re-select + re-reset on next access */
}
static inline uint8_t ata_dev(void){ return (uint8_t)(g_slave ? 0xF0 : 0xE0); }

/* ATA soft-reset + select the current drive.  Required after a warm boot
 * in some emulators (QEMU PIIX) otherwise BSY stays stuck forever. */
static void ata_soft_reset(void){
    outb(ATA_DCR, ATA_DCR_nIEN | ATA_DCR_SRST);
    ata_delay400();
    outb(ATA_DCR, ATA_DCR_nIEN);            /* clear SRST to release reset */
    ata_delay400();
    (void)ata_wait_bsy();                   /* reset clears BSY within ~1ms */
    outb(ATA_DRIVE, ata_dev());              /* selected drive, LBA addressing */
    ata_delay400();
    (void)ata_wait_bsy();
}

static inline void ata_ensure_ready(void){
    if(g_ata_inited) return;
    ata_soft_reset();
    g_ata_inited = 1;
}

/* Read `n` sectors starting at LBA `lba` into buf. buf must be n*512 bytes. */
int ata_read(uint32_t lba, uint8_t *buf, uint32_t n){
    if(n == 0) return 0;
    ata_ensure_ready();
    if(ata_wait_bsy() < 0) return -1;
    outb(ATA_DRIVE, ata_dev() | ((lba >> 24) & 0x0F));
    outb(ATA_COUNT, (uint8_t)n);
    outb(ATA_LBA_LO, (uint8_t)(lba));
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI, (uint8_t)(lba >> 16));
    outb(ATA_CMD, ATA_CMD_READ);
    ata_delay400();

    for(uint32_t s = 0; s < n; s++){
        if(ata_wait_bsy() < 0) return -1;
        if(ata_wait_drq() < 0) return -1;
        uint16_t *w = (uint16_t*)(buf + s*512);
        for(int i = 0; i < 256; i++) w[i] = inw(ATA_DATA);
    }
    return 0;
}

/* Write `n` sectors from buf to LBA `lba`. */
int ata_write(uint32_t lba, const uint8_t *buf, uint32_t n){
    if(n == 0) return 0;
    ata_ensure_ready();
    if(ata_wait_bsy() < 0) return -1;
    outb(ATA_DRIVE, ata_dev() | ((lba >> 24) & 0x0F));
    outb(ATA_COUNT, (uint8_t)n);
    outb(ATA_LBA_LO, (uint8_t)(lba));
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI, (uint8_t)(lba >> 16));
    outb(ATA_CMD, ATA_CMD_WRITE);
    ata_delay400();

    for(uint32_t s = 0; s < n; s++){
        if(ata_wait_bsy() < 0) return -1;
        if(ata_wait_drq() < 0) return -1;
        const uint16_t *w = (const uint16_t*)(buf + s*512);
        for(int i = 0; i < 256; i++) outw(ATA_DATA, w[i]);
    }
    /* wait for the data phase to finish before issuing FLUSH */
    if(ata_wait_bsy() < 0) return -1;
    outb(ATA_CMD, 0xE7);
    if(ata_wait_bsy() < 0) return -1;
    return 0;
}
