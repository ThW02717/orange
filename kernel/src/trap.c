#include <stdint.h>
#include "trap.h"
#include "uart.h"

/* Minimal trap subsystem skeleton for Basic Exercise 1.
 * TODO:
 * 1. trap_init(): program stvec to point at trap_entry.
 * 2. enter_user_mode() path: prepare an initial trapframe and sret into U-mode.
 * 3. trap.S: save x1-x31 and CSR state into struct trapframe, then call do_trap().
 * 4. handle_user_ecall(): decode a7, advance sepc for SYS_test, and finish SYS_exit.
 * 5. handle_user_fault(): terminate the current user program and return to shell.
 */

static void trap_print_tf(const struct trapframe *tf)
{
    uart_send_string("[trap] scause=");
    uart_send_hex((unsigned long)tf->scause);
    uart_send_string(" sepc=");
    uart_send_hex((unsigned long)tf->sepc);
    uart_send_string(" stval=");
    uart_send_hex((unsigned long)tf->stval);
    uart_send_string("\n");
}

void trap_init(void)
{
    asm volatile("csrw stvec, %0" : : "r"(trap_entry));
}

void handle_user_ecall(struct trapframe *tf)
{
    trap_print_tf(tf);

    switch (tf->a7) {
    case SYS_test:
        /* TODO: add any temporary diagnostics you want for the SYS_test path. */
        tf->sepc += 4;
        return;
    case SYS_exit:
        /* TODO: mark the current user program as finished and return to shell. */
        uart_send_string("[trap] SYS_exit not implemented yet\n");
        tf->sepc += 4;
        return;
    default:
        uart_send_string("[trap] unknown syscall id=");
        uart_send_dec((unsigned long)tf->a7);
        uart_send_string("\n");
        /* Skip the ecall to avoid immediately trapping on the same instruction. */
        tf->sepc += 4;
        return;
    }
}

void handle_user_fault(struct trapframe *tf)
{
    trap_print_tf(tf);
    uart_send_string("[trap] unhandled user fault\n");
    /* TODO: stop returning to this user context and hand control back to the shell. */
}

void do_trap(struct trapframe *tf)
{
    if (tf == 0) {
        uart_send_string("[trap] null trapframe\n");
        return;
    }

    if (trap_is_interrupt(tf->scause)) {
        uart_send_string("[trap] interrupt path not implemented yet\n");
        trap_print_tf(tf);
        return;
    }

    if (trap_scause_code(tf->scause) == SCAUSE_U_ECALL) {
        handle_user_ecall(tf);
        return;
    }

    handle_user_fault(tf);
}
