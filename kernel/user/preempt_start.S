.section .text
.globl _start

/* preempt_main owns the exit path; there is no C runtime to return to. */
_start:
    tail preempt_main
