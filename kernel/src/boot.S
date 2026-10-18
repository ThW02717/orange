.section .text.boot
.global _start
.global secondary_start

/*
 * Establish the primary stack and zero BSS before C code uses static state.
 * Leave the incoming boot arguments in a0-a3 for kernel_main.
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

/* There is no firmware return path if kernel_main returns. */
loop:
    wfi
    j loop

/*
 * SBI HSM entry: a0 = hart ID, a1 = private stack top (the opaque argument).
 * The primary hart has already cleared BSS.
 */
secondary_start:
    mv sp, a1
    call secondary_main

secondary_loop:
    wfi
    j secondary_loop
