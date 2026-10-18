.section .text
.global _start

_start:
    /* Test user-fault termination independently of the syscall path. */
    .word 0xffffffff

1:
    j 1b
