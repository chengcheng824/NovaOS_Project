; =========================================================
; NovaOS - Stage 2 loader (fits 1024 bytes / 2 sectors, loaded 0:0x8000)
; =========================================================
[bits 16]
[org 0x8000]
%include "kernel_entry.inc"
; NOTE: the %ifndef guard below is ALWAYS taken - `equ` symbols from
; kernel_entry.inc are not preprocessor macros, so %ifndef can't see
; them and these %defines silently win. Keep KERNEL_BIN_BYTES in sync
; with stage1's KERNEL_SECS budget (LBA 3..128 = 126 sectors = 0xFC00
; bytes); build.ps1 enforces the same limit on kernel.bin.
%ifndef KERNEL_ENTRY_VMA
  %define KERNEL_IMG_BASE    0x00100000
  %define KERNEL_ENTRY_VMA   0x00100240
  %define KERNEL_BIN_BYTES   0x0000FC00
%endif
COM1 equ 0x3F8
TMP  equ  0x00008400
S_TOP equ 0x00200000
VGA  equ 0x000B8000

stage2_start:
    mov [kern_sct], ax
    mov [drv], dl
    call c1i
    mov si, T1
    call c1s
    mov si, SS2
    call prn
    cli

    ; A20 via i8042 + fast-a20 fallback
    call W1
    mov al, 0xAD
    out 0x64, al
    call W1
    mov al, 0xD0
    out 0x64, al
    call W0
    in al, 0x60
    push ax
    call W1
    mov al, 0xD1
    out 0x64, al
    call W1
    pop ax
    or al, 2
    out 0x60, al
    call W1
    mov al, 0xAE
    out 0x64, al
    in al, 0x92
    or al, 2
    out 0x92, al
    mov si, TA
    call c1s

    lgdt [gdt_desc]
    mov si, TG
    call c1s

    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x08:pm_entry

; ---- 16-bit helpers ----
c1i:
    mov dx, COM1+1
    xor al, al
    out dx, al
    mov al, 0x80
    mov dx, COM1+3
    out dx, al
    mov al, 1
    mov dx, COM1+0
    out dx, al
    inc dx
    xor al, al
    out dx, al
    mov al, 3
    mov dx, COM1+3
    out dx, al
    mov al, 7
    mov dx, COM1+2
    out dx, al
    mov al, 3
    mov dx, COM1+4
    out dx, al
    ret
c1c:
    push ax
    mov dx, COM1+5
.w: in al, dx
    test al, 0x20
    jz .w
    pop ax
    mov dx, COM1
    out dx, al
    ret
c1s:
    lodsb
    test al, al
    jz .e
    call c1c
    jmp c1s
.e: ret
W0: in al, 0x64
    test al, 1
    jz W0
    ret
W1: in al, 0x64
    test al, 2
    jnz W1
    ret
prn:
    push bx
    mov ah, 0x0E
    xor bh, bh
    mov bl, 0x0F
.l: lodsb
    test al, al
    jz .e
    int 0x10
    jmp .l
.e: pop bx
    ret

; ---- 32-bit PM ----
[bits 32]
pm_entry:
    mov esi, TP
    call c1s32
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, S_TOP

    xor ecx, ecx
    mov cx, word [kern_sct]
    shl ecx, 9
    mov edx, KERNEL_BIN_BYTES
    cmp ecx, edx
    jna .ok
    mov ecx, edx
.ok:
    ; Zero the WHOLE kernel image area (KERNEL_BIN_BYTES) at
    ; KERNEL_IMG_BASE *before* copying. This guarantees BSS (which lives
    ; after the file-backed .text/.rdata/.data inside the image area) is
    ; zero on every boot, including after a keyboard-controller reset
    ; (reboot) where memory is NOT cleared by the BIOS. Without this,
    ; stale BSS values (kb_shift, cmdlen, vga_row, ...) from the previous
    ; kernel run corrupt the rebooted shell and eventually triple-fault.
    push ecx
    push esi
    mov edi, KERNEL_IMG_BASE
    mov ecx, KERNEL_BIN_BYTES
    xor eax, eax
    cld
    rep stosb
    pop esi
    pop ecx
    ; Now copy the file-backed portion (.text/.rdata/.data) over the
    ; zeroed area.
    mov esi, TMP
    mov edi, KERNEL_IMG_BASE
    cld
    rep movsb

    ; compare first 16 bytes
    mov esi, TMP
    mov edi, KERNEL_IMG_BASE
    mov ecx, 16
    repe cmpsb
    jne bad

    ; VGA row1 grey "[Stage2 PM OK]"
    mov esi, P1
    mov edi, VGA+160
    mov ah, 0x07
    call VP
    ; VGA row2 blue "KCPY OK"
    mov edi, VGA+320
    mov ah, 0x1F
    mov al, 'K'
    stosw
    mov al, 'C'
    stosw
    mov al, 'P'
    stosw
    mov al, 'Y'
    stosw
    mov al, ' '
    stosw
    mov al, 'O'
    stosw
    mov al, 'K'
    stosw
    mov esi, TCP
    call c1s32
    ; VGA row3 green "JMP <hex>"
    mov edi, VGA+480
    mov ah, 0x2F
    mov al, 'J'
    stosw
    mov al, 'M'
    stosw
    mov al, 'P'
    stosw
    mov al, ' '
    stosw
    mov eax, KERNEL_ENTRY_VMA
    call HX

    mov esi, TJ
    call c1s32
    mov eax, KERNEL_ENTRY_VMA
    call HX32
    mov al, 10
    call c1c32

    jmp  0x08:KMAIN_VMA            ; hand off to C kmain at 0x00100000 (never returns)
    hlt
    jmp $

bad:
    mov edi, VGA+320
    mov ah, 0x4F
    mov al, 'C'
    stosw
    mov al, 'P'
    stosw
    mov al, 'Y'
    stosw
    mov al, ' '
    stosw
    mov al, 'B'
    stosw
    mov al, 'A'
    stosw
    mov al, 'D'
    stosw
    mov esi, TCB
    call c1s32
    jmp $

; ---- PM helpers ----
c1c32:
    push edx
    push eax
    mov dx, COM1+5
.w: in al, dx
    test al, 0x20
    jz .w
    pop  eax
    mov dx, COM1
    out dx, al
    pop edx
    ret
c1s32:
    lodsb
    test al, al
    jz .e
    call c1c32
    jmp c1s32
.e: ret
HX32:
    push ebx
    push ecx
    mov ebx, eax
    mov ecx, 8
.l: rol ebx, 4
    mov al, bl
    and al, 0xF
    add al, '0'
    cmp al, '9'
    jna .ok
    add al, 'A'-'9'-1
.ok:push ebx
    push ecx
    call c1c32
    pop ecx
    pop ebx
    loop .l
    pop ecx
    pop ebx
    ret
HX:
    push ebx
    push ecx
    mov ebx, eax
    mov ecx, 8
.l: rol ebx, 4
    mov al, bl
    and al, 0xF
    add al, '0'
    cmp al, '9'
    jna .ok
    add al, 'A'-'9'-1
.ok:stosw
    loop .l
    pop ecx
    pop ebx
    ret
VP: lodsb
    test al, al
    jz .e
    stosw
    jmp VP
.e: ret

; ---- 16-bit data ----
[bits 16]
T1:  db 'S2.', 0
TA:  db 'A20.', 0
TG:  db 'GDT.', 0
TP:  db 'PM.', 0
TCP: db 'CPK.', 0
TCB: db 'CBD.', 0
TJ:  db 'JMP ', 0
P1:  db ' [Stage2 PM OK] ', 0
SS2: db '[Stage2] ', 0
drv:     db 0
kern_sct: dw 0

; ---- GDT ----
align 4, db 0
gdt_start:  dd 0, 0
     dw 0xFFFF, 0
     db 0x00, 10011010b, 11001111b, 0x00
     dw 0xFFFF, 0
     db 0x00, 10010010b, 11001111b, 0x00
gdt_end:
gdt_desc:  dw gdt_end-gdt_start-1
     dd gdt_start
times 1024-($-$$) db 0
