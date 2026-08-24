/* ============================================================
 * NovaOS - minimal ACPI support (shutdown only)
 *   RSDP -> RSDT/XSDT -> FADT -> DSDT/SSDT
 *   + tiny AML walker that finds Name(\_S5, Package(a,b))
 * ============================================================ */
#ifndef ACPI_H
#define ACPI_H

#include "stdint.h"

typedef struct {
    uint32_t rsdp;            /* RSDP physical address, 0 = not found */
    uint32_t sdt;             /* RSDT or XSDT address */
    uint8_t  xsdt;            /* 1 = sdt is an XSDT */
    uint32_t fadt;            /* FADT address, 0 = not found */
    uint32_t dsdt;            /* DSDT address */
    uint32_t pm1a_cnt;        /* PM1a control block IO port */
    uint32_t pm1b_cnt;        /* PM1b control block IO port (may be 0) */
    uint16_t slp_typa;        /* \_S5 SLP_TYP for PM1a */
    uint16_t slp_typb;        /* \_S5 SLP_TYP for PM1b */
    uint8_t  s5_found;        /* 1 = \_S5 package parsed from AML */
    uint32_t aml_err;         /* offset where the AML walk gave up,
                                 0xFFFFFFFF = every table walked cleanly */
} acpi_info_t;

extern acpi_info_t g_acpi;

int  acpi_init(void);      /* 0 = tables found (see g_acpi for details) */
void acpi_poweroff(void);  /* write S5 to PM1_CNT; no-op without tables */

#endif
