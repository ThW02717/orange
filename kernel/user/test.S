.section .text
.global _start

_start:
    li a7, 0
    ecall

    li a7, 1
    ecall

1:
    j 1b
