#include "sched.h"
#include <stdint.h>
#include "trap.h"
#include "syscall_dispatch.h"
#include "plic.h"
#include "timer.h"
#include "uart.h"
#include "thread.h"
#include "user.h"

/*
 * Assembly saves/restores CPU state. This file classifies the trap and
 * forwards user ecalls to syscall.c, interrupts to their handlers, and faults
 * to the kernel/user fault paths.
 */

static int trace_ecalls;
static int32_t ecall_demo_tid = -1;

void trap_set_ecall_demo(int32_t tid)
{
    ecall_demo_tid = tid;
}

int trap_trace_ecalls(int enabled)
{
    int previous = trace_ecalls;
    trace_ecalls = enabled != 0;
    return previous;
}

static void trap_print_tc(const struct trap_context *tc)
{
    uart_send_string("[trap] scause=");
    uart_send_hex((unsigned long)tc->scause);
    uart_send_string(" sepc=");
    uart_send_hex((unsigned long)tc->sepc);
    uart_send_string(" stval=");
    uart_send_hex((unsigned long)tc->stval);
    uart_send_string("\n");
}

void trap_init(void)
{
    /* All supervisor traps currently enter through one common assembly stub. */
    asm volatile("csrw stvec, %0" : : "r"(trap_entry));
    asm volatile("csrw sscratch, zero");
}

void handle_user_fault(struct trap_context *tc)
{
    trap_print_tc(tc);
    uart_send_string("[trap] user fault, terminate\n");
    /* Abandon this user context; idle reclaims it after the scheduler switch. */
    user_mark_exit();
}

void trap_dispatch(struct trap_context *tc)
{
    if (tc == 0) {
        uart_send_string("[trap] null trap_context\n");
        return;
    }

    if (trap_is_interrupt(tc->scause)) {
        if (trap_scause_code(tc->scause) == SCAUSE_S_TIMER_INT) {
            timer_irq_top();
            if ((tc->sstatus & SSTATUS_SPP) == 0) {
                struct thread *task = thread_current();

                if (task != 0 && task->kind == THREAD_USER && thread_has_runnable_tasks()) {
                    thread_request_resched();
                }
            }
            return;
        }

        if (trap_scause_code(tc->scause) == SCAUSE_S_EXT_INT) {
            uint32_t irq;

            irq = plic_claim();
            if (irq == uart_irq_id()) {
                uart_irq_top();
            }
            if (irq != 0U) {
                plic_complete(irq);
            }
            return;
        }

        uart_send_string("[trap] unhandled interrupt\n");
        trap_print_tc(tc);
        return;
    }

    /* U-mode ecall is a syscall; other user exceptions terminate the task. */
    if (trap_scause_code(tc->scause) == SCAUSE_U_ECALL) {
        struct thread *task = thread_current();

        /*
         * The supplied Lab 4 image counts down a7, before any syscall ABI is
         * defined. Only its explicitly selected task uses this exercise path.
         */
        if (task != 0 && task->tid == ecall_demo_tid) {
            trap_print_tc(tc);
            uart_send_string("[trap] lab ecall a7=");
            uart_send_dec((unsigned long)tc->a7);
            uart_send_string("\n");
            tc->sepc += 4;
            return;
        }
        if (trace_ecalls) {
            trap_print_tc(tc);
        }
        handle_user_ecall(tc);
        return;
    }

    if ((tc->sstatus & SSTATUS_SPP) != 0) {
        /*
         * Retrying a faulting kernel instruction would trap forever. A
         * kernel fault is not an ordinary user-task termination.
         */
        asm volatile("csrci sstatus, 2" ::: "memory");
        uart_send_string("[trap] fatal kernel fault\n");
        trap_print_tc(tc);
        for (;;) {
            asm volatile("wfi");
        }
    }
    handle_user_fault(tc);
}
