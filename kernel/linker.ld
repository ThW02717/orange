ENTRY(_start)

SECTIONS
{
    /* Default load address targets UART-loaded board kernel. */
    . = DEFINED(KERNEL_LOAD_ADDR) ? KERNEL_LOAD_ADDR : 0x20000000;
    _phys_start = .;

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
    _phys_end = .;
}
