; ============================================================
; NexOS - kernel layout constants for Stage2
; ============================================================
; Kernel image is copied to 0x00100000 (1 MiB) and entered
; directly at its very first byte (flat binary, entry = base).
; ============================================================
KERNEL_IMG_BASE    equ 0x00100000
KERNEL_ENTRY_VMA   equ 0x00100000
KERNEL_BIN_BYTES   equ 0x00040000   ; 256 KiB max kernel image
KMAIN_VMA          equ 0x00100000  ; C kmain lives at the image base
