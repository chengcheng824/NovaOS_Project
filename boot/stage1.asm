; ============================================================
; NovaOS - Stage 1 / MBR  (512 bytes, loaded at 0000:7C00)
; Hard disk boot via INT 13h AH=42h (LBA mode, drive 0x80)
;
; Layout:
;   Offset  0-445   MBR boot code (this file)
;   Offset 446-509  Partition table (4 x 16 bytes, reserved)
;   Offset 510-511  Boot signature 0xAA55
;
; Disk LBA map:
;   LBA 0          MBR (this sector)
;   LBA 1-2        Stage2 (2 sectors, 1024 bytes -> 0000:8000)
;   LBA 3-82       Kernel (80 sectors reserved, -> 0000:8400)
; ============================================================

[bits 16]
[org 0x7C00]

; --- Load targets ---
STAGE2_OFF   equ 0x8000
KERNEL_OFF   equ 0x8400
STAGE2_LBA   equ 1
STAGE2_SECS  equ 2
KERNEL_LBA   equ 3
KERNEL_SECS  equ 80

start:
    cli
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7C00
    cld
    sti

    ; BIOS passes boot drive in DL (0x80 for first hard disk)
    mov  [drive_num], dl

    ; Clear screen
    mov  ax, 0x0600
    xor  cx, cx
    mov  dx, 0x184F
    mov  bh, 0x0F
    int  0x10

    ; Print banner
    mov  si, msg_boot
    call rm_print

    ; --- Load Stage2 (2 sectors at LBA 1 -> 0000:8000) ---
    mov  si, dap_stage2
    mov  dl, [drive_num]
    mov  ah, 0x42
    int  0x13
    jc   disk_error

    ; --- Load Kernel (72 sectors at LBA 3 -> 0000:8400) ---
    mov  si, dap_kernel
    mov  dl, [drive_num]
    mov  ah, 0x42
    int  0x13
    jc   disk_error

    ; --- Jump to Stage2 ---
    ; Pass DL = drive_num, AX = kernel_sector_count
    mov  dl, [drive_num]
    mov  ax, KERNEL_SECS
    jmp  0x0000:STAGE2_OFF

disk_error:
    mov  si, msg_err
    call rm_print
    cli
.hang:
    hlt
    jmp  .hang

; ---------------------------------------------------------------
; rm_print: print NUL-terminated string at DS:SI via INT10 TTY
; ---------------------------------------------------------------
rm_print:
    push ax
    push bx
    mov  ah, 0x0E
    xor  bh, bh
    mov  bl, 0x0F
.l:  lodsb
    test al, al
    jz   .e
    int  0x10
    jmp  .l
.e:  pop  bx
     pop  ax
     ret

; --- Data ---
msg_boot:   db '[MBR] NovaOS hard disk boot', 0
msg_err:    db 'DISK ERROR', 0
drive_num:  db 0

; --- DAP for Stage2 (2 sectors at LBA 1 -> 0000:8000) ---
dap_stage2:
    db 0x10            ; DAP size (16 bytes)
    db 0               ; reserved
    dw STAGE2_SECS     ; sectors to read
    dw STAGE2_OFF      ; buffer offset
    dw 0x0000          ; buffer segment
    dq STAGE2_LBA      ; starting LBA (64-bit)

; --- DAP for Kernel (72 sectors at LBA 3 -> 0000:8400) ---
dap_kernel:
    db 0x10            ; DAP size (16 bytes)
    db 0               ; reserved
    dw KERNEL_SECS     ; sectors to read
    dw KERNEL_OFF      ; buffer offset
    dw 0x0000          ; buffer segment
    dq KERNEL_LBA      ; starting LBA (64-bit)

; --- Pad code to 446 bytes ---
times 446 - ($ - $$) db 0

; --- Partition table (4 entries x 16 bytes = 64 bytes) ---
; All zeros: bare disk, no partitions (reserved area, not touched)
times 64 db 0

; --- Boot signature ---
dw 0xAA55