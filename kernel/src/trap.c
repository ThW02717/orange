#include <stdint.h>
#include "trap.h"
#include "timer.h"
#include "uart.h"
#include "user.h"

/* C-side trap dispatch for the minimal user-mode path.
 * Assembly is responsible for saving/restoring CPU state. This file decides
 * whether the trap is an ecall, a user fault, or an unimplemented interrupt.
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
    /* All supervisor traps currently enter through one common assembly stub. */
    asm volatile("csrw stvec, %0" : : "r"(trap_entry));
}

void handle_user_ecall(struct trapframe *tf)
{
    /* Print the trapped user state before decoding the syscall number in a7. */
    trap_print_tf(tf);

    switch (tf->a7) {
    case SYS_test:
        /* Skip the ecall itself and resume at the next user instruction. */
        tf->sepc += 4;
        return;
    case SYS_exit:
        uart_send_string("[kernel] user exit\n");
        user_mark_exit();
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
    uart_send_string("[trap] user fault, terminate\n");
    /* Force trap_entry.S to abandon the current user context and re-enter
     * the shell instead of restoring the faulting user state.
     */
    user_mark_exit();
}

void do_trap(struct trapframe *tf)
{
    if (tf == 0) {
        uart_send_string("[trap] null trapframe\n");
        return;
    }

    if (trap_is_interrupt(tf->scause)) {
        if (trap_scause_code(tf->scause) == SCAUSE_S_TIMER_INT) {
            timer_handle_interrupt();
            
            return;
        }

        uart_send_string("[trap] unhandled interrupt\n");
        trap_print_tf(tf);
        return;
    }

    /* Basic Exercise 1 only expects U-mode ecall plus a generic user fault
     * fallback for every other exception.
     */
    if (trap_scause_code(tf->scause) == SCAUSE_U_ECALL) {
        handle_user_ecall(tf);
        return;
    }

    handle_user_fault(tf);
}
