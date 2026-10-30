#include "syscall.h"

#ifndef USER_ENTRY
#define USER_ENTRY threads_main
#endif

    .section .text
    .globl _start
/* Returning from the test leader exits the whole process, including workers. */
_start:
    call USER_ENTRY
    li a0, 0
    li a7, SYS_exit
    ecall
1:  j 1b

    .globl user_thread_return
/* Worker return stub for thread_create_user(); exits only this thread. */
user_thread_return:
    li a0, 0
    li a7, SYS_thread_exit
    ecall
1:  j 1b
