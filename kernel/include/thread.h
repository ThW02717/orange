#ifndef THREAD_H
#define THREAD_H

#include <stdint.h>

struct trap_context;

/*
 * Single-active-hart scheduler:
 * - cooperative kernel workers
 * - user tasks with separate user/kernel stacks
 * - timer-driven preemption at user trap-return boundaries
 */

enum thread_kind {
    THREAD_KERNEL = 0,
    THREAD_USER,
};

enum thread_state {
    THREAD_RUNNING = 0,
    THREAD_RUNNABLE,
    THREAD_NEW,      /* private construction state; not waiting for I/O */
    THREAD_ZOMBIE,
};

/*
 * Kernel execution context saved/restored by switch_to().
 * Save callee-saved registers plus ra/sp at the function-call boundary.
 * Registers interrupted by a trap are saved separately in trap_context.
 */
struct thread_context {
    uint64_t ra;
    uint64_t sp;
    uint64_t s0;
    uint64_t s1;
    uint64_t s2;
    uint64_t s3;
    uint64_t s4;
    uint64_t s5;
    uint64_t s6;
    uint64_t s7;
    uint64_t s8;
    uint64_t s9;
    uint64_t s10;
    uint64_t s11;
};

/*
 * Thread control block.
 *
 * Keep thread_context first so switch_to.S can treat a
 * `struct thread *` as the base address of the saved context without adding
 * another offset in assembly.
 */
struct thread {
    struct thread_context thread_context; /* first field: assembly context base */
    int32_t tid;  /* unique scheduler identity */
    int32_t tgid; /* process ID; the first thread has tid == tgid */
    enum thread_kind kind;
    enum thread_state state;

    void (*entry)(void *arg);
    void *arg;

    void *kstack_base;
    uint64_t kstack_top;

    struct thread *next;   /* run queue linkage */
    struct thread *znext;  /* zombie list linkage */
    struct thread *all_next; /* global task list linkage */

    int is_idle;

    /* User-only state; kernel workers leave these fields zero. */
    uintptr_t user_entry;
    uintptr_t user_stack_base;
    uintptr_t user_stack_top;
    int exit_status;
    struct trap_context *tc; /* points into this thread's kernel stack */
};

/* Create runnable tasks. Each gets its own kernel stack and saved context. */
int thread_create_kernel(void (*entry)(void *), void *arg);
/* On failure, the caller retains ownership of the supplied user stack. */
int user_process_create(uintptr_t entry, uintptr_t stack_base, uintptr_t stack_top);
/* Same TGID, new TID and stack; return_pc exits the worker after entry returns. */
int thread_create_user(uintptr_t entry, uintptr_t arg, uintptr_t return_pc);

/* Marking exit leaves trap return responsible for switching away. */
void thread_mark_current_zombie(int exit_status);
void thread_exit(void);
int thread_stop_tid(int32_t tid, int exit_status);
int thread_stop_process(int32_t tgid, int exit_status);

int thread_user_group_size(const struct thread *task);
int thread_user_stack_contains(const struct thread *task, uintptr_t start, uintptr_t end);
int thread_user_image_busy(const struct thread *except);

#endif
