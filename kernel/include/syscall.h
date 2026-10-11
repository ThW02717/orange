#ifndef SYSCALL_H
#define SYSCALL_H

/* P1 ABI, not Linux syscall numbers. a7 selects the call; a0 holds its result. */
#define SYS_getpid        0
#define SYS_uart_read     1
#define SYS_uart_write    2
#define SYS_exec          3
#define SYS_fork          4
#define SYS_exit          5 /* one thread, including a returning user worker */
#define SYS_stop          6 /* all user threads with the supplied process ID */
#define SYS_gettid        7
#define SYS_thread_create 8 /* a0 = entry, a1 = argument, a2 = return-to-exit stub */
#define SYS_exit_group    9

#endif
