#ifndef SYSCALL_H
#define SYSCALL_H

/* OSC 2026 Lab 5 ABI. a7 selects the call; a0 holds its result.
 * Keep unimplemented lab calls reserved; the dispatcher returns -1 for them.
 */
#define SYS_getpid        0
#define SYS_uart_read     1
#define SYS_uart_write    2
#define SYS_exec          3
#define SYS_fork          4
#define SYS_waitpid        5
#define SYS_exit           6 /* all user threads in the calling process */
#define SYS_stop           7
#define SYS_display        8
#define SYS_usleep         9
#define SYS_signal        10
#define SYS_sigreturn     11
#define SYS_kill          12

/* P1 extensions follow the lab range. Worker return must exit only itself. */
#define SYS_gettid        13
#define SYS_thread_create 14 /* a0 = entry, a1 = argument, a2 = return-to-exit stub */
#define SYS_thread_exit   15

#endif
