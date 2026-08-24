; ============================================================
; NovaOS - 32-bit kernel entry stub (NASM win32 / PE-COFF)
; Placed at the very start of the kernel image at 0x00100000.
; Plus: Ring3 enter/leave trampolines and IDT stubs.
; ============================================================
[bits 32]
[extern _kmain]
[extern _syscall_dispatch]
[extern _fault_dispatch]
[global __start]
[global _start]
[global _ring3_enter]
[global _ring3_leave]
[global _isr_syscall]
[global _isr_fault_err]
[global _isr_fault_noerr]

section .text start=0x100000
_start:
__start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x00200000
    cld
    call _kmain
.hang:
    cli
    hlt
    jmp .hang

; ---- Ring 3 enter: cdecl ring3_enter(entry_eip, user_esp, eax_val) ----
; saves the kernel context, builds an iret frame, drops to CPL 3.
_ring3_enter:
    mov  ecx, [esp+4]              ; user entry eip
    mov  edx, [esp+8]              ; user esp
    mov  eax, [esp+12]             ; value for user EAX (api table)
    mov  [k3_ebx], ebx
    mov  [k3_esi], esi
    mov  [k3_edi], edi
    mov  [k3_ebp], ebp
    mov  [k3_esp], esp             ; kernel stack, [ret-to-caller] on top

    ; --- BEFORE iret: load user data selectors (0x20|RPL3 = 0x23) into
    ; DS/ES/FS/GS. iretd only restores CS/SS/EIP/ESP/EFLAGS. If we leave
    ; them as kernel selectors (0x10, DPL=0) while CPL becomes 3, the
    ; first memory access will #GP because CPL(3) > descriptor DPL(0).
    ; NOTE: can NOT use AX or DX directly: AX clobbers EAX (api ptr
    ; passed to user), DX clobbers EDX (user ESP). Use stack to save. ---
    push eax
    push edx
    mov  ax, 0x23
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    pop  edx
    pop  eax

    push dword 0x23                ; user SS (0x20|3)
    push edx                       ; user ESP
    push dword 0x00000002          ; EFLAGS (bit1 always 1, IF=0 to prevent
                                   ; any spurious IRQ from uninitialized PIC
                                   ; state; user IOPL=0 so no IO anyway)
    push dword 0x1B                ; user CS (0x18|3)
    push ecx                       ; user EIP
    iretd

; ---- Ring 3 leave: restore kernel context, return from ring3_enter ----
_ring3_leave:
    ; --- Back in CPL 0: restore kernel selectors before touching anything
    ; (segment regs still hold user 0x23 from Ring3 run). ---
    mov  dx, 0x10
    mov  ds, dx
    mov  es, dx
    mov  fs, dx
    mov  gs, dx
    mov  ebx, [k3_ebx]
    mov  esi, [k3_esi]
    mov  edi, [k3_edi]
    mov  ebp, [k3_ebp]
    ; Intel manual: after MOV SS, interrupts are inhibited until after the
    ; NEXT instruction.  So place MOV ESP immediately after MOV SS so the
    ; SS:ESP pair is updated atomically with respect to interrupts/NMIs.
    mov  ss, dx
    mov  esp, [k3_esp]
    ret

; ---- int 0x80 syscall gate (DPL 3 trap gate) ----
_isr_syscall:
    ; CPU entered from Ring3 -> SS/ESP switched via TSS to kernel stack,
    ; but DS/ES/FS/GS still hold user selectors. Switch them first so
    ; kernel C code can access kernel data with DPL=0 descriptors.
    push edx
    mov  dx, 0x10
    mov  ds, dx
    mov  es, dx
    mov  fs, dx
    mov  gs, dx
    pop  edx
    pushad
    push esp
    call _syscall_dispatch
    add  esp, 4
    popad
    iretd

; ---- exception gates: never return (kill program / halt kernel) ----
_isr_fault_err:                    ; CPU pushed an error code
    push edx
    mov  dx, 0x10
    mov  ds, dx
    mov  es, dx
    mov  fs, dx
    mov  gs, dx
    pop  edx
    pushad
    push esp
    call _fault_dispatch
.hang1: jmp .hang1

_isr_fault_noerr:                  ; synthesize a dummy error code
    push edx
    mov  dx, 0x10
    mov  ds, dx
    mov  es, dx
    mov  fs, dx
    mov  gs, dx
    pop  edx
    push dword 0
    pushad
    push esp
    call _fault_dispatch
.hang2: jmp .hang2

; kernel context saved while a user program runs
k3_ebx: dd 0
k3_esi: dd 0
k3_edi: dd 0
k3_ebp: dd 0
k3_esp: dd 0
