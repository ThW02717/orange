/*
 * User entry and exit. user_reset_trap_context() seeds a fresh U-mode image;
 * user_task_entry() enters it through trap_return().
 */
#include "irqflags.h"
#include "sched.h"
#include <stdint.h>
#include "thread.h"
#include "trap.h"
#include "uart.h"
#include "user.h"

/*
 * Fresh execution starts in U-mode, with interrupts enabled after sret.
 * Do not inherit kernel-only sstatus bits or the previous program's registers.
 */
void user_reset_trap_context(struct thread *task)
{
    if (task == 0 || task->tc == 0) {
        return;
    }
    *task->tc = (struct trap_context){0};
    task->tc->sp = task->user_stack_top;
    task->tc->tp = (uintptr_t)task;
    task->tc->sepc = task->user_entry;
    task->tc->sstatus = SSTATUS_SPIE;
}

static void user_enter_current_task(void)
{
    struct thread *task;

    task = thread_current();
    if (task == 0 || task->kind != THREAD_USER || task->tc == 0) {
        uart_send_string("[user] no current user task\n");
        for (;;) {
            asm volatile("wfi");
        }
    }

    trap_return(task->tc);

    for (;;) {
        asm volatile("wfi");
    }
}

/* Common bootstrap for a new user process or a same-process user thread. */
void user_task_entry(void *arg)
{
    (void)arg;
    user_enter_current_task();
}

void user_mark_exit(void)
{
    struct thread *task;

    task = thread_current();
    if (task == 0 || task->kind != THREAD_USER) {
        return;
    }

    thread_mark_current_zombie(0);
}

int user_has_exited(void)
{
    struct thread *task;

    task = thread_current();
    if (task == 0 || task->kind != THREAD_USER) {
        return 0;
    }
    return task->state == THREAD_ZOMBIE;
}

/* Switch away from a finished user task. Reclamation must use another stack. */
void user_schedule_after_exit(void)
{
    local_irq_enable();

    schedule();

    for (;;) {
        asm volatile("wfi");
    }
}
