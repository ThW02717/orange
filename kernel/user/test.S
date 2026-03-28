.section .text
.global _start

_start:
    li a7, 0
    ecall

    addi a0, a0, 48
    la t0, msg
    sb a0, 4(t0)

    la a0, msg
    li a1, 6
    li a7, 2
    ecall

    li a0, 0
    li a7, 5
    ecall

1:
    j 1b

.section .rodata
msg:
    .ascii "pid=\n\n"
