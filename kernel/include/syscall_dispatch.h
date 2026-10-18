#ifndef SYSCALL_DISPATCH_H
#define SYSCALL_DISPATCH_H

struct trap_context;

/* Kernel-only ecall entry. Shared syscall numbers stay in syscall.h. */
void handle_user_ecall(struct trap_context *tc);

#endif
