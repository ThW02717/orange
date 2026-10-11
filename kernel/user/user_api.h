#ifndef USER_API_H
#define USER_API_H

#include <stdint.h>
#include "syscall.h"

static inline long user_syscall(long nr, long arg0, long arg1, long arg2)
{
    register long a0 asm("a0") = arg0;
    register long a1 asm("a1") = arg1;
    register long a2 asm("a2") = arg2;
    register long a7 asm("a7") = nr;

    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline long getpid(void) { return user_syscall(SYS_getpid, 0, 0, 0); }
static inline long gettid(void) { return user_syscall(SYS_gettid, 0, 0, 0); }
static inline long fork(void) { return user_syscall(SYS_fork, 0, 0, 0); }
static inline long exec(const char *name) { return user_syscall(SYS_exec, (long)name, 0, 0); }
static inline long stop(long pid) { return user_syscall(SYS_stop, pid, 0, 0); }
static inline long uart_write(const char *buf, long size)
{
    return user_syscall(SYS_uart_write, (long)buf, size, 0);
}

static inline __attribute__((noreturn)) void thread_exit(int status)
{
    (void)user_syscall(SYS_exit, status, 0, 0);
    for (;;) {}
}

static inline __attribute__((noreturn)) void exit_group(int status)
{
    (void)user_syscall(SYS_exit_group, status, 0, 0);
    for (;;) {}
}

void user_thread_return(void);

/* The kernel sets ra to this user stub. Returning from entry exits only the
 * new thread, without jumping to the parent's call site or a kernel address.
 */
static inline long thread_create_user(void (*entry)(void *), void *arg)
{
    return user_syscall(SYS_thread_create, (long)entry, (long)arg, (long)user_thread_return);
}

#endif
