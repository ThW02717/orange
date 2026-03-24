.section .text
.global _start

_start:
    /* Deliberately execute an invalid instruction so the user fault path
     * can be verified without involving the ecall syscall path.
     */
    .word 0xffffffff

1:
    j 1b
