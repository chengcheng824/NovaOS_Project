/* NexOS kernel linker script (PE i386, MinGW ld)
   All loadable sections are placed tightly starting at 0x00100000.
   .reloc and debug sections are discarded so objcopy -O binary
   produces a small flat image whose first byte is _start.          */
ENTRY(_start)

SECTIONS
{
    . = 0x00100000;

    .text : {
        *(.text._start)
        *(.text)
        *(.text.*)
    }

    .rdata : { *(.rdata) *(.rdata.*) *(.rodata) *(.rodata.*) }

    .data : { *(.data) *(.data.*) }

    .bss : {
        bss_start = .;
        __bss_start = .;
        _bss_start = .;
        *(.bss) *(.bss.*) *(COMMON)
        bss_end = .;
        __bss_end = .;
        _bss_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.reloc)
        *(.debug*)
        *(.pdata)
        *(.xdata)
    }
}
