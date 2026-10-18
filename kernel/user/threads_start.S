#include "syscall.h"

    .section .text
    .globl _start
/* Returning from the test leader exits the whole process, including workers. */
_start:
    call threads_main
    li a0, 0
    li a7, SYS_exit_group
    ecall
1:  j 1b

    .globl user_thread_return
/* Worker return stub for thread_create_user(); exits only this thread. */
user_thread_return:
    li a0, 0
    li a7, SYS_exit
    ecall
1:  j 1b
