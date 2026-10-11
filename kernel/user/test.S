#include "syscall.h"

.section .text
.global _start

_start:
    li a7, SYS_getpid
    ecall

    /* IDs can exceed nine after several launches. Build the whole decimal
     * message on our stack instead of overwriting one byte in a literal.
     */
    addi sp, sp, -32
    addi t0, sp, 31
    li t2, 10
    sb t2, 0(t0)
    mv t1, a0
    li t3, 10
1:
    remu t2, t1, t3
    addi t2, t2, 48
    addi t0, t0, -1
    sb t2, 0(t0)
    divu t1, t1, t3
    bnez t1, 1b
    addi t0, t0, -4
    li t2, 'p'
    sb t2, 0(t0)
    li t2, 'i'
    sb t2, 1(t0)
    li t2, 'd'
    sb t2, 2(t0)
    li t2, '='
    sb t2, 3(t0)

    mv a0, t0
    addi a1, sp, 32
    sub a1, a1, a0
    li a7, SYS_uart_write
    ecall
    addi sp, sp, 32

    li a0, 0
    li a7, SYS_exit
    ecall

1:
    j 1b
