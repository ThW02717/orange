#include <stdint.h>
#include "thread.h"
#include "trap.h"
#include "uart.h"
#include "user.h"

/* Current-aware user-mode entry for the BE2 transition.
 *
 * The old Basic Exercise 1 path used one global user stack and one global
 * "user exited" flag. The new path starts from the currently scheduled task
 * instead: each schedulable user task owns its own kernel stack and its own
 * trap_context location on that stack.
 */

void user_reset_trap_context(struct thread *task)
{
    uint64_t sstatus;
    unsigned int i;
    uint64_t *raw;

    if (task == 0 || task->tc == 0) {
        return;
    }

    raw = (uint64_t *)task->tc;
    for (i = 0; i < (sizeof(*task->tc) / sizeof(uint64_t)); i++) {
        raw[i] = 0;
    }

    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    sstatus &= ~(SSTATUS_SPP | SSTATUS_SIE);
    sstatus |= SSTATUS_SPIE;

    task->tc->sp = task->user_stack_top;
    task->tc->tp = (uint64_t)(uintptr_t)task;
    task->tc->sepc = task->user_entry;
    task->tc->sstatus = sstatus;
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

    trap_init();
    trap_return(task->tc);

    for (;;) {
        asm volatile("wfi");
    }
}

/* Legacy name kept temporarily while the BE2 path is migrating away from the
 * old singleton user state. It now enters the current user task rather than a
 * global one-shot user context.
 */
void enter_user_mode(void)
{
    user_enter_current_task();
}

/* New schedulable user task entry used by thread_create_user(). */
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

/* User exit/fault no longer jumps directly back into the shell loop. Instead,
 * mark the current task dead and let the scheduler switch back to idle. That
 * returns control to the original shell-side caller naturally once the user
 * task is gone.
 */
void user_schedule_after_exit(void)
{
    asm volatile("csrs sstatus, %0" : : "r"(SSTATUS_SIE) : "memory");

    schedule();

    for (;;) {
        asm volatile("wfi");
    }
}
