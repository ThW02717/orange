ENTRY(_start)

SECTIONS
{
    /* Keep image away from low boot-reserved memory. */
    . = 0x00200000;

    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    .rodata : {
        . = ALIGN(8);
        *(.rodata .rodata.*)
    }

    .data : {
        . = ALIGN(8);
        *(.data .data.*)
    }

    /* Symbols consumed by boot.S for manual BSS clear. */
    .bss : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* 16 KiB boot stack, top grows downward. */
    . = ALIGN(16);
    . += 0x4000;
    _stack_top = .;
}
