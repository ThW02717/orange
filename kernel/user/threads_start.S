#include "syscall.h"

    .section .text
    .globl _start
_start:
    call threads_main
    li a0, 0
    li a7, SYS_exit_group
    ecall
1:  j 1b

    .globl user_thread_return
user_thread_return:
    li a0, 0
    li a7, SYS_exit
    ecall
1:  j 1b
