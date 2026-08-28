#ifndef ATA_H
#define ATA_H
#include "stdint.h"
int ata_read(uint32_t lba, uint8_t *buf, uint32_t n);
int ata_write(uint32_t lba, const uint8_t *buf, uint32_t n);
/* select master (0) or slave (1) of the primary bus for subsequent I/O */
void ata_set_slave(int on);
#endif
