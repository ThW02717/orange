ENTRY(_start)

SECTIONS
{
    /* Board: 0x00200000, QEMU virt: 0x80200000 */
    . = DEFINED(BOOTLOADER_LOAD_ADDR) ? BOOTLOADER_LOAD_ADDR : 0x00200000;

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
