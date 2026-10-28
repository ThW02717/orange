/*
 * Thread creation and lifetime.
 * Creation: thread_create_kernel(), user_process_create(), thread_create_user().
 * First run: switch_to() enters thread_bootstrap(), then calls entry(arg).
 * Exit: mark a zombie now; thread_reap_zombies() frees it after switching away.
 */
#include "thread.h"
#include "thread_internal.h"
#include "sched.h"
#include "irqflags.h"
#include "memory.h"
#include "trap.h"
#include "user.h"
#include "user_layout.h"

/* Kernel calls and nested traps share this stack. */
#define THREAD_STACK_SIZE 16384UL

static struct thread *zombie_head;
static struct thread *all_threads;
static uint32_t next_tid = 1;

static void thread_bootstrap(void);

static int32_t thread_next_tid(void)
{
    uint64_t status = local_irq_save();
    int32_t tid = -1;

    /* Do not wrap an ID into an existing task or a negative error value. */
    if (next_tid <= INT32_MAX) {
        tid = (int32_t)next_tid++;
    }
    local_irq_restore(status);
    return tid;
}

static void thread_list_add(struct thread *t)
{
    uint64_t sstatus;

    sstatus = local_irq_save();
    t->all_next = all_threads;
    all_threads = t;
    local_irq_restore(sstatus);
}

static void thread_list_remove(struct thread *t)
{
    uint64_t sstatus;
    struct thread *prev = 0;
    struct thread *cur;

    sstatus = local_irq_save();
    cur = all_threads;
    while (cur != 0) {
        if (cur == t) {
            if (prev == 0) {
                all_threads = cur->all_next;
            } else {
                prev->all_next = cur->all_next;
            }
            cur->all_next = 0;
            break;
        }
        prev = cur;
        cur = cur->all_next;
    }
    local_irq_restore(sstatus);
}

void thread_register_boot(struct thread *task)
{
    thread_list_add(task);
}

/* Leave the task private until its entry and context are ready. */
static struct thread *thread_alloc(void)
{
    struct thread *task;
    int32_t tid = thread_next_tid();

    if (tid < 0) {
        return 0;
    }
    task = kmalloc(sizeof(*task));
    if (task == 0) {
        return 0;
    }
    *task = (struct thread){0};
    task->kstack_base = kmalloc(THREAD_STACK_SIZE);
    if (task->kstack_base == 0) {
        kfree(task);
        return 0;
    }
    task->kstack_top = ((uintptr_t)task->kstack_base + THREAD_STACK_SIZE) & ~15UL;
    task->thread_context.ra = (uintptr_t)thread_bootstrap;
    task->thread_context.sp = task->kstack_top;
    task->tid = tid;
    task->tgid = tid;
    task->state = THREAD_NEW;
    return task;
}

/* Publish exactly once, after all state needed by the first run is set. */
void thread_publish(struct thread *task)
{
    uint64_t status = local_irq_save();

    if (task != 0 && task->state == THREAD_NEW) {
        task->state = THREAD_RUNNABLE;
        thread_list_add(task);
        sched_enqueue(task);
    }
    local_irq_restore(status);
}

/* Create a kernel worker; the scheduler decides when it starts. */
int thread_create_kernel(void (*entry)(void *), void *arg)
{
    struct thread *task;

    if (entry == 0) {
        return -1;
    }
    task = thread_alloc();
    if (task == 0) {
        return -1;
    }
    task->kind = THREAD_KERNEL;
    task->entry = entry;
    task->arg = arg;
    thread_publish(task);
    return task->tid;
}

/* The caller owns the supplied user stack on failure. */
struct thread *thread_prepare_user(uintptr_t entry, uintptr_t stack_base,
                                   uintptr_t stack_top)
{
    struct thread *task;

    if (entry < USER_CODE_BASE || entry >= USER_CODE_LIMIT || (entry & 1U) ||
        stack_base == 0 || stack_top <= stack_base || (stack_top & 15U)) {
        return 0;
    }
    task = thread_alloc();
    if (task == 0) {
        return 0;
    }
    task->kind = THREAD_USER;
    task->entry = user_task_entry;
    task->user_entry = entry;
    task->user_stack_base = stack_base;
    task->user_stack_top = stack_top;
    task->tc = (struct trap_context *)(task->kstack_top - TRAP_CONTEXT_ALLOC_SIZE);
    /* Bootstrap calls use the space below the prepared trap context. */
    task->thread_context.sp = (uintptr_t)task->tc;
    user_reset_trap_context(task);
    return task;
}

int user_process_create(uintptr_t entry, uintptr_t stack_base, uintptr_t stack_top)
{
    struct thread *task = thread_prepare_user(entry, stack_base, stack_top);

    if (task == 0) {
        return -1;
    }
    /* This is a new process, so keep the default tgid == tid. */
    thread_publish(task);
    return task->tid;
}

int thread_create_user(uintptr_t entry, uintptr_t arg, uintptr_t return_pc)
{
    struct thread *parent = thread_current();
    struct thread *child;
    uintptr_t stack;

    if (parent == 0 || parent->kind != THREAD_USER || parent->tc == 0 ||
        parent->state == THREAD_ZOMBIE ||
        entry < USER_CODE_BASE || entry >= USER_CODE_LIMIT || (entry & 1U) != 0 ||
        return_pc < USER_CODE_BASE || return_pc >= USER_CODE_LIMIT || (return_pc & 1U) != 0) {
        return -1;
    }
    stack = (uintptr_t)kmalloc(USER_STACK_SIZE);
    if (stack == 0) {
        return -1;
    }
    child = thread_prepare_user(entry, stack, stack + USER_STACK_SIZE);
    if (child == 0) {
        kfree((void *)stack);
        return -1;
    }
    /* Same process, new stack. Only fork copies the parent's stack/context. */
    child->tgid = parent->tgid;
    child->tc->a0 = arg;
    child->tc->ra = return_pc;
    child->tc->gp = parent->tc->gp;
    thread_publish(child);
    return child->tid;
}

/*
 * First entry point for a brand-new thread.
 *
 * Both kernel and user constructors seed thread_context.ra with this address.
 * The first switch_to() returns here, then calls the task's kernel-side entry.
 * A user task proceeds to sret; a returning kernel worker calls thread_exit().
 */
static void thread_bootstrap(void)
{
    struct thread *cur = thread_current();

    /*
     * switch_to runs with IRQs masked. A new task has no suspended schedule()
     * call that could restore its interrupt state, so enable it here.
     */
    local_irq_enable();

    if (cur == 0 || cur->entry == 0) {
        for (;;) {
            asm volatile("wfi");
        }
    }

    cur->entry(cur->arg);
    thread_exit();

    for (;;) {
        asm volatile("wfi");
    }
}

static struct thread *thread_find_by_tid(int32_t tid)
{
    struct thread *cur;
    uint64_t sstatus;

    sstatus = local_irq_save();
    cur = all_threads;
    while (cur != 0) {
        if (cur->tid == tid) {
            break;
        }
        cur = cur->all_next;
    }
    local_irq_restore(sstatus);
    return cur;
}

void thread_mark_current_zombie(int exit_status)
{
    struct thread *cur = thread_current();
    uint64_t sstatus;

    if (cur == 0 || cur->is_idle != 0) {
        return;
    }

    sstatus = local_irq_save();
    if (cur->state == THREAD_ZOMBIE) {
        local_irq_restore(sstatus);
        return;
    }
    cur->state = THREAD_ZOMBIE;
    cur->exit_status = exit_status;
    cur->znext = zombie_head;
    zombie_head = cur;
    local_irq_restore(sstatus);
}

int thread_stop_tid(int32_t tid, int exit_status)
{
    struct thread *target;
    uint64_t sstatus;

    target = thread_find_by_tid(tid);
    if (target == 0 || target->is_idle != 0) {
        return -1;
    }
    if (target == thread_current()) {
        thread_mark_current_zombie(exit_status);
        return 0;
    }

    sstatus = local_irq_save();
    if (target->state == THREAD_ZOMBIE) {
        local_irq_restore(sstatus);
        return 0;
    }
    sched_remove(target);
    target->state = THREAD_ZOMBIE;
    target->exit_status = exit_status;
    target->znext = zombie_head;
    zombie_head = target;
    local_irq_restore(sstatus);
    return 0;
}

int thread_stop_process(int32_t tgid, int exit_status)
{
    uint64_t status = local_irq_save();
    struct thread *cur;
    int found = 0;

    /* Search by group, not by its first thread: the leader may have exited. */
    for (cur = all_threads; cur != 0; cur = cur->all_next) {
        if (cur->kind == THREAD_USER && cur->tgid == tgid) {
            (void)thread_stop_tid(cur->tid, exit_status);
            found = 1;
        }
    }
    local_irq_restore(status);
    return found ? 0 : -1;
}

/*
 * Mark the current worker as dead and hand control back to the scheduler.
 *
 * The exiting thread is pushed onto a zombie list so the idle side can recycle
 * its stack and thread object later, outside the switched-away context.
 */
void thread_exit(void)
{
    if (thread_current() == 0 || thread_current()->is_idle != 0) {
        for (;;) {
            asm volatile("wfi");
        }
    }

    /*
     * After the current worker marks itself ZOMBIE, it must never re-enter the
     * runnable queue. The idle side later frees the detached stack/object.
     */
    thread_mark_current_zombie(0);
    schedule();

    for (;;) {
        asm volatile("wfi");
    }
}

void thread_reap_zombies(void)
{
    for (;;) {
        struct thread *z;
        uint64_t sstatus;

        sstatus = local_irq_save();
        z = zombie_head;
        if (z != 0 && z != thread_current()) {
            zombie_head = z->znext;
        }
        local_irq_restore(sstatus);

        if (z == 0 || z == thread_current()) {
            break;
        }

        if (z->is_idle != 0) {
            continue;
        }
        thread_list_remove(z);
        if (z->kind == THREAD_USER && z->user_stack_base != 0) {
            kfree((void *)(uintptr_t)z->user_stack_base);
        }
        if (z->kstack_base != 0) {
            kfree(z->kstack_base);
        }
        kfree(z);
    }
}

int thread_user_group_size(const struct thread *task)
{
    uint64_t status = local_irq_save();
    struct thread *cur;
    int count = 0;

    if (task != 0 && task->kind == THREAD_USER) {
        for (cur = all_threads; cur != 0; cur = cur->all_next) {
            if (cur->kind == THREAD_USER && cur->tgid == task->tgid &&
                cur->state != THREAD_ZOMBIE) {
                count++;
            }
        }
    }
    local_irq_restore(status);
    return count;
}

int thread_user_stack_contains(const struct thread *task, uintptr_t start, uintptr_t end)
{
    uint64_t status = local_irq_save();
    struct thread *cur;
    int found = 0;

    /*
     * A sibling may pass a pointer into a live thread's stack. Its lifetime
     * still ends with the owner: detached thread stacks are not shared heaps.
     */
    if (task != 0 && task->kind == THREAD_USER && end > start) {
        for (cur = all_threads; cur != 0; cur = cur->all_next) {
            if (cur->kind == THREAD_USER && cur->tgid == task->tgid &&
                cur->state != THREAD_ZOMBIE && start >= cur->user_stack_base &&
                end <= cur->user_stack_top) {
                found = 1;
                break;
            }
        }
    }
    local_irq_restore(status);
    return found;
}

/*
 * This no-MMU kernel has one shared user-code window. Replacing that image
 * is unsafe while another live user task can still execute from it.
 */
int thread_user_image_busy(const struct thread *except)
{
    uint64_t status = local_irq_save();
    struct thread *cur = all_threads;
    int busy = 0;
    while (cur != 0) {
        if (cur != except && cur->kind == THREAD_USER && cur->state != THREAD_ZOMBIE) {
            busy = 1;
            break;
        }
        cur = cur->all_next;
    }
    local_irq_restore(status);
    return busy;
}

#ifdef KERNEL_TEST
struct thread *thread_test_find(int32_t tid)
{
    return thread_find_by_tid(tid);
}

uint32_t thread_test_next_tid(uint32_t next)
{
    uint32_t old = next_tid;
    next_tid = next;
    return old;
}
#endif
