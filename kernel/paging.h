/* ============================================================
 * NovaOS - Ring 3 user mode (paging + GDT/TSS/IDT + syscalls)
 * ============================================================ */
#ifndef PAGING_H
#define PAGING_H

#include "stdint.h"

/* trampoline page handed to user programs (one 4K page @ 4MB) */
#define NXP_TRAMP_BASE  0x00400000u     /* syscall stubs (code)        */
#define NXP_TRAMP_ARGS  0x00400F00u     /* scratch arg block (32 B)    */
#define NXP_TRAMP_API   0x00400F80u     /* user-visible nxp_api_t copy */

/* one-time init: identity paging, user segments, TSS, IDT, PIC mask */
void ring3_init(const void *api_table);

/* build the trampoline page (call right before entering a program) */
void ring3_setup_tramp(void);

/* asm: drop to Ring 3 via iret; returns when the program exits     */
uint32_t ring3_enter(uint32_t entry_eip, uint32_t user_esp, uint32_t eax_val);

/* asm: restore the saved kernel context (exit / user fault)        */
void ring3_leave(void);

/* address of the exit stub inside the trampoline page              */
extern uint32_t g_nxp_exit_stub;

#endif
