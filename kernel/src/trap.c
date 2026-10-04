#include <stdint.h>
#include "trap.h"
#include "plic.h"
#include "timer.h"
#include "uart.h"
#include "memory.h"
#include "thread.h"
#include "shell.h"
#include "user.h"

/* C-side trap dispatcher for the minimal user-mode path.
 * Assembly is responsible for saving/restoring CPU state. This file only
 * classifies the trap and dispatches to the appropriate handler or ISR.
 */

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

static struct thread *current_user_task(void)
{
    struct thread *task = thread_current();

    if (task == 0 || task->kind != THREAD_USER) {
        return 0;
    }
    return task;
}

static int user_range_ok(const struct thread *task, uintptr_t addr, uint64_t count, int writable)
{
    uintptr_t end;

    if (task == 0) {
        return 0;
    }
    if (count == 0) {
        return 1;
    }
    if (addr == 0) {
        return 0;
    }
    end = addr + count;
    if (end < addr) {
        return 0;
    }

    if (addr >= task->user_stack_base && end <= task->user_stack_top) {
        return 1;
    }
    if (!writable && addr >= USER_CODE_BASE && end <= USER_CODE_LIMIT) {
        return 1;
    }
    return 0;
}

static long sys_getpid(void)
{
    struct thread *task = current_user_task();

    if (task == 0) {
        return -1;
    }
    return task->pid;
}

static long sys_uart_write(const char *buf, long count)
{
    struct thread *task = current_user_task();
    uint64_t sstatus;
    long i;

    if (task == 0 || count < 0) {
        return -1;
    }
    if (!user_range_ok(task, (uintptr_t)buf, (uint64_t)count, 0)) {
        return -1;
    }
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    asm volatile("csrc sstatus, %0" : : "r"(SSTATUS_SIE));
    for (i = 0; i < count; i++) {
        if (buf[i] == '\n') {
            uart_send('\r');
        }
        uart_send(buf[i]);
    }
    asm volatile("csrw sstatus, %0" : : "r"(sstatus));
    return count;
}

static long sys_uart_read(char *buf, long count)
{
    struct thread *task = current_user_task();
    long i;

    if (task == 0 || count < 0) {
        return -1;
    }
    if (!user_range_ok(task, (uintptr_t)buf, (uint64_t)count, 1)) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        buf[i] = uart_recv();
    }
    return count;
}

static long sys_exec(const char *path, struct trap_context *tc)
{
    struct thread *task = current_user_task();
    char name[64];
    uintptr_t entry;
    unsigned long size;
    unsigned int i = 0;

    if (task == 0 || tc == 0 || path == 0) {
        return -1;
    }
    while (i + 1U < sizeof(name)) {
        char c;

        if (!user_range_ok(task, (uintptr_t)(path + i), 1U, 0)) {
            return -1;
        }
        c = path[i];
        name[i] = c;
        if (c == '\0') {
            break;
        }
        i++;
    }
    name[sizeof(name) - 1U] = '\0';
    if (name[0] == '\0') {
        return -1;
    }
    if (shell_load_user_program_named(name, &entry, &size) != 0) {
        return -1;
    }

    task->user_entry = entry;
    (void)size;
    user_reset_trap_context(task);
    tc->a0 = 0;
    return 1;
}

static long sys_fork(struct trap_context *parent_tc)
{
    struct thread *parent = current_user_task();
    struct thread *child;
    uintptr_t child_stack_base;
    uintptr_t child_stack_top;
    uintptr_t parent_sp;
    uintptr_t child_sp;
    uint64_t stack_off;
    unsigned int i;
    uint64_t *dst_tc_words;
    const uint64_t *src_tc_words;
    int child_pid;

    if (parent == 0 || parent_tc == 0) {
        return -1;
    }

    child_stack_base = (uintptr_t)kmalloc(USER_STACK_SIZE);
    if (child_stack_base == 0) {
        return -1;
    }
    child_stack_top = (child_stack_base + USER_STACK_SIZE) & ~0xFUL;

    for (i = 0; i < USER_STACK_SIZE; i++) {
        volatile uint8_t *dst = (volatile uint8_t *)child_stack_base;
        volatile uint8_t *src = (volatile uint8_t *)parent->user_stack_base;
        dst[i] = src[i];
    }

    /* The copied user stack still contains saved frame pointers and other
     * stack-local addresses that point back into the parent's stack image.
     * For this minimal no-MMU process model, remap any 64-bit word that falls
     * inside the parent stack range to the corresponding child stack address.
     */
    for (i = 0; i + sizeof(uint64_t) <= USER_STACK_SIZE; i += sizeof(uint64_t)) {
        uint64_t *slot = (uint64_t *)(uintptr_t)(child_stack_base + i);
        uint64_t v = *slot;

        if ((uintptr_t)v >= parent->user_stack_base &&
            (uintptr_t)v <= parent->user_stack_top) {
            *slot = child_stack_base + (uint64_t)((uintptr_t)v - parent->user_stack_base);
        }
    }

    child_pid = thread_create_user(parent->user_entry, child_stack_base, child_stack_top);
    if (child_pid < 0) {
        kfree((void *)child_stack_base);
        return -1;
    }

    child = thread_find_by_pid(child_pid);
    if (child == 0 || child->tc == 0) {
        thread_stop_pid(child_pid, -1);
        return -1;
    }

    dst_tc_words = (uint64_t *)child->tc;
    src_tc_words = (const uint64_t *)parent_tc;
    for (i = 0; i < (sizeof(*child->tc) / sizeof(uint64_t)); i++) {
        dst_tc_words[i] = src_tc_words[i];
    }
    child->tc->tp = (uint64_t)(uintptr_t)child;
    child->tc->a0 = 0;
    child->tc->sepc += 4;

    parent_sp = (uintptr_t)parent_tc->sp;
    if (parent_sp < parent->user_stack_base || parent_sp > parent->user_stack_top) {
        thread_stop_pid(child_pid, -1);
        return -1;
    }

    stack_off = (uint64_t)(parent_sp - parent->user_stack_base);
    child_sp = child->user_stack_base + stack_off;
    child->tc->sp = child_sp;

    /* Any saved GPR that still points into the parent's user stack must be
     * remapped to the corresponding address in the child's private stack.
     * Otherwise locals accessed via frame pointers (for example &cnt in the
     * fork test) still alias the parent's stack even though sp was fixed.
     */
#define REMAP_STACK_FIELD(field)                                                     \
    do {                                                                             \
        uintptr_t v__ = (uintptr_t)child->tc->field;                                 \
        if (v__ >= parent->user_stack_base && v__ <= parent->user_stack_top) {       \
            child->tc->field = child->user_stack_base +                              \
                               (uint64_t)(v__ - parent->user_stack_base);            \
        }                                                                            \
    } while (0)
    REMAP_STACK_FIELD(ra);
    REMAP_STACK_FIELD(sp);
    REMAP_STACK_FIELD(t0);
    REMAP_STACK_FIELD(t1);
    REMAP_STACK_FIELD(t2);
    REMAP_STACK_FIELD(s0);
    REMAP_STACK_FIELD(s1);
    REMAP_STACK_FIELD(a0);
    REMAP_STACK_FIELD(a1);
    REMAP_STACK_FIELD(a2);
    REMAP_STACK_FIELD(a3);
    REMAP_STACK_FIELD(a4);
    REMAP_STACK_FIELD(a5);
    REMAP_STACK_FIELD(a6);
    REMAP_STACK_FIELD(a7);
    REMAP_STACK_FIELD(s2);
    REMAP_STACK_FIELD(s3);
    REMAP_STACK_FIELD(s4);
    REMAP_STACK_FIELD(s5);
    REMAP_STACK_FIELD(s6);
    REMAP_STACK_FIELD(s7);
    REMAP_STACK_FIELD(s8);
    REMAP_STACK_FIELD(s9);
    REMAP_STACK_FIELD(s10);
    REMAP_STACK_FIELD(s11);
    REMAP_STACK_FIELD(t3);
    REMAP_STACK_FIELD(t4);
    REMAP_STACK_FIELD(t5);
    REMAP_STACK_FIELD(t6);
#undef REMAP_STACK_FIELD

    return child_pid;
}

static long sys_stop(long pid)
{
    if (pid < 0) {
        return -1;
    }
    return thread_stop_pid((int32_t)pid, 0);
}

void trap_init(void)
{
    /* All supervisor traps currently enter through one common assembly stub. */
    asm volatile("csrw stvec, %0" : : "r"(trap_entry));
}

void handle_user_ecall(struct trap_context *tc)
{
    long ret = -1;
    int exec_reset = 0;

    switch (tc->a7) {
    case SYS_getpid:
        ret = sys_getpid();
        break;
    case SYS_uart_read:
        ret = sys_uart_read((char *)(uintptr_t)tc->a0, (long)tc->a1);
        break;
    case SYS_uart_write:
        ret = sys_uart_write((const char *)(uintptr_t)tc->a0, (long)tc->a1);
        break;
    case SYS_exec:
        ret = sys_exec((const char *)(uintptr_t)tc->a0, tc);
        if (ret >= 0) {
            exec_reset = 1;
            ret = 0;
        }
        break;
    case SYS_fork:
        ret = sys_fork(tc);
        break;
    case SYS_exit:
        thread_mark_current_zombie((int)tc->a0);
        return;
    case SYS_stop:
        ret = sys_stop((long)tc->a0);
        if (ret == 0 && (long)tc->a0 == (long)sys_getpid()) {
            return;
        }
        break;
    default:
        ret = -1;
        break;
    }

    tc->a0 = (uint64_t)ret;
    if (!exec_reset) {
        tc->sepc += 4;
    }
}

void handle_user_fault(struct trap_context *tc)
{
    trap_print_tc(tc);
    uart_send_string("[trap] user fault, terminate\n");
    /* Force trap_entry.S to abandon the current user context and re-enter
     * the shell instead of restoring the faulting user state.
     */
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
            if (current_user_task() != 0 && thread_has_runnable_tasks()) {
                thread_request_resched();
            }
            return;
        }

        if (trap_scause_code(tc->scause) == SCAUSE_S_EXT_INT) {
            uint32_t irq;

            irq = plic_claim();
            if (irq == uart_irq_id()) {
                uart_demo_note_external_irq(irq);
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

    /* Basic Exercise 1 only expects U-mode ecall plus a generic user fault
     * fallback for every other exception.
     */
    if (trap_scause_code(tc->scause) == SCAUSE_U_ECALL) {
        handle_user_ecall(tc);
        return;
    }

    handle_user_fault(tc);
}
