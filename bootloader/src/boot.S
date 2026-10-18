.section .text.boot
.global _start
.global secondary_start

/*
 * Enter the UART bootloader with the firmware's a0/a1 arguments intact.
 * Static state must be zeroed before calling its C entry point.
 */
_start:
    la sp, _stack_top
    la t0, __bss_start     
    la t1, __bss_end    

bss_loop:
    bgeu t0, t1, bss_done
    sb zero, 0(t0)
    addi t0, t0, 1
    j bss_loop

bss_done:
    call kernel_main

/* There is no firmware return path if the bootloader's C entry returns. */
loop:
    wfi
    j loop

/* Secondary entry expects a private stack in the SBI HSM opaque argument. */
secondary_start:
    mv sp, a1
    call secondary_main

secondary_loop:
    wfi
    j secondary_loop
