#ifndef ATA_H
#define ATA_H
#include "stdint.h"
int ata_read(uint32_t lba, uint8_t *buf, uint32_t n);
int ata_write(uint32_t lba, const uint8_t *buf, uint32_t n);
#endif
