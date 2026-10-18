.section .text
.global _start

/* fork_main owns the exit path; there is no C runtime to return to. */
_start:
    tail fork_main
