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

/* asm: park the caller's kernel context for ring3_leave to resume  */
void main_checkpoint(void);

/* asm: iret straight into a saved 13-dword user frame (never returns) */
void jmp_user(uint32_t *fr);

/* ---- slot-based processes (4 slots, one address space) ---- */
int  proc_spawn(int slot, const char *name);   /* pid = slot+1, -1 = busy  */
uint32_t *proc_frame(int slot);                /* iret frame for jmp_user  */
uint32_t proc_slot_base(int slot);             /* link base of slot        */
int  proc_list(char *buf, uint32_t max);       /* "pid st name\n" lines    */
int  proc_any(void);                           /* any live process?        */
int  proc_stopped_any(void);                   /* any suspended process?   */
void proc_resume_all(void);                    /* suspended -> ready       */
void proc_kill_all(void);                      /* free every slot          */
int  proc_kill(int pid);                       /* free one pid, -1 = bad   */
int  proc_next(void);                          /* next schedulable, -1     */
void proc_set_current(int slot);               /* mark slot as the running */

/* address of the exit stub inside the trampoline page              */
extern uint32_t g_nxp_exit_stub;

#endif
