/*
 * Shell demos only. Kernel mechanisms do not depend on this file.
 * demo_run() selects the test; helpers prepare its input and check output.
 */
#include "string.h"
#include "thread_internal.h"
#include "demo_uart.h"
#include "trace.h"
#include "sched.h"
#include "user_layout.h"
#include "irqflags.h"
#include <stdint.h>
#include "demo.h"
#include "memory.h"
#include "memory_test.h"
#include "exec.h"
#include "trap.h"
#include "thread.h"
#include "timer.h"
#include "uart.h"
#include "bottom_half.h"
#include "custom_work.h"

struct demo_thread_arg {
    char tag;
    unsigned int rounds;
};

static int is_space_local(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int parse_u64_dec_local(const char *s, uint64_t *out)
{
    uint64_t value = 0;
    unsigned int i = 0;

    if (s == 0 || out == 0 || s[0] == '\0') {
        return -1;
    }

    while (s[i] != '\0') {
        char c = s[i];

        if (c < '0' || c > '9') {
            return -1;
        }
        if (value > (UINT64_MAX - (uint64_t)(c - '0')) / 10ULL) {
            return -1;
        }
        value = (value * 10ULL) + (uint64_t)(c - '0');
        i++;
    }

    *out = value;
    return 0;
}

static void split_first_arg_local(char *line, char **cmd, char **arg)
{
    char *p = line;

    while (is_space_local(*p)) {
        p++;
    }
    *cmd = p;
    while (*p != '\0' && !is_space_local(*p)) {
        p++;
    }
    if (*p != '\0') {
        *p++ = '\0';
    }
    while (is_space_local(*p)) {
        p++;
    }
    *arg = (*p == '\0') ? 0 : p;
}

void demo_print_usage(void)
{
    uart_send_string("demo usage:\n");
    uart_send_string("  demo badinst     - run the illegal-instruction user test and return to shell\n");
    uart_send_string("  demo foo         - create threads with thread_create_kernel(foo, ...)\n");
    uart_send_string("  demo fork        - run the user-space fork demo and return to shell\n");
    uart_send_string("  demo preempt     - run two busy user tasks to test timer preemption\n");
    uart_send_string("  demo threads     - test same-process user threads and group exit\n");
    uart_send_string("  demo nested      - print the recommended manual nested-interrupt test flow\n");
    uart_send_string("  demo tasks       - verify IRQ delivery and deferred callback order\n");
    uart_send_string("  demo thread      - run a cooperative RR kernel-thread demo and enter idle\n");
    uart_send_string("  demo test        - run the user-space syscall smoke test and return to shell\n");
    uart_send_string("  demo uart        - run the one-shot interrupt-driven UART demo\n");
    uart_send_string("  demo stress [n]  - queue UART TX markers for interleave testing\n");
    uart_send_string("  demo mem <name>  - run allocator test: basic|boundary|slab|multislab|reclaim|large|buddy|invalid|stress|all\n");
    uart_send_string("  demo trace       - dump recent top/bottom-half trace events\n");
    uart_send_string("  demo trace reset - clear recent trace events\n");
}

/* Manual UART/timer interleave test; inspect the trace afterward. */
static int demo_nested(void)
{
    trace_reset();
    uart_send_string("manual nested-interrupt test:\n");
    uart_send_string("  addtimer 1\n");
    uart_send_string("  addtimer 2\n");
    uart_send_string("  addtimer 3\n");
    uart_send_string("  uartstress\n");
    uart_send_string("expected: [T1]/[T2]/[T3] should appear between [U...] markers\n");
    return 0;
}

/*
 * Keep the fixture alive even if a broken timer path misses the test timeout.
 * A generation token makes a late timer from an earlier run harmless.
 */
static struct {
    unsigned long generation;
    volatile int active;
    volatile int first_done;
    int failed;
    unsigned int count;
    unsigned int events[8];
} task_demo;

static void demo_task_event(unsigned int event)
{
    uint64_t status = local_irq_save();

    if (task_demo.count < 8U) {
        task_demo.events[task_demo.count++] = event;
    } else {
        task_demo.failed = 1;
    }
    local_irq_restore(status);
}

static void demo_task_first(void *arg)
{
    (void)arg;
    demo_task_event(5);
    task_demo.first_done = 1;
}

static void demo_task_second(void *arg)
{
    (void)arg;
    demo_task_event(6);
}

static void demo_task_third(void *arg)
{
    (void)arg;
    demo_task_event(7);
}

static void demo_task_timer(void *arg)
{
    if (!task_demo.active ||
        (unsigned long)(uintptr_t)arg != task_demo.generation) {
        return;
    }
    demo_task_event(4);
    if (defer_callback(demo_task_first, 0) != 0 ||
        defer_callback(demo_task_second, 0) != 0 ||
        defer_callback(demo_task_third, 0) != 0) {
        task_demo.failed = 1;
    }
}

static void demo_task_outer(void *arg)
{
    uint64_t start = timer_read();
    uint64_t delay = timebase_frequency / 100U;

    demo_task_event(1);
    if (add_timer(demo_task_timer, arg, 0) != 0) {
        task_demo.failed = 1;
        return;
    }
    /*
     * Spend a bounded interval doing work with IRQs enabled. The timer top
     * half must arrive, but its callback cannot run until we return.
     */
    while (timer_read() - start < (delay ? delay : 1U)) {
        asm volatile("" ::: "memory");
    }
    if (trace_seen_since(TRACE_TIMER_TOP, start)) {
        demo_task_event(2);
    } else {
        task_demo.failed = 1;
    }
    if (task_demo.first_done) {
        task_demo.failed = 1;
    }
    demo_task_event(3);
}

static int demo_tasks(void)
{
    static const char *const names[] = {
        "?", "outer:start", "timer:top", "outer:resume", "timer:bottom",
        "first", "second", "third"
    };

    if (timebase_frequency == 0) {
        uart_send_string("[tasks] timer unavailable\n");
        return -1;
    }
    task_demo.generation++;
    task_demo.active = 1;
    task_demo.first_done = 0;
    task_demo.failed = 0;
    task_demo.count = 0;
    trace_reset();
    if (defer_callback(demo_task_outer, (void *)(uintptr_t)task_demo.generation) != 0) {
        task_demo.active = 0;
        uart_send_string("[tasks] callback queue full\n");
        return -1;
    }
    run_bottom_halves();
    task_demo.active = 0;

    uart_send_string("[tasks] ");
    for (unsigned int i = 0; i < task_demo.count; i++) {
        unsigned int event = task_demo.events[i];
        if (i != 0) { uart_send_string(" -> "); }
        uart_send_string(event < 8U ? names[event] : "?");
        if (event != i + 1U) {
            task_demo.failed = 1;
        }
    }
    uart_send_string("\n");
    if (task_demo.failed || task_demo.count != 7U || !task_demo.first_done) {
        uart_send_string("[tasks] FAIL\n");
        return -1;
    }
    uart_send_string("[tasks] PASS\n");
    return 0;
}

static int demo_mem(const char *name)
{
    return memory_run_kmtest(name);
}

static int demo_launch_user_program(const char *name, const char *label,
                                    unsigned int instances)
{
    struct exec_result result;

    if (exec_spawn(name, instances, &result) != 0) {
        uart_send_string(label);
        uart_send_string(": failed to load or create user task\n");
        return -1;
    }
    uart_send_string(label);
    uart_send_string(": run user demo\n");
    uart_tx_wait_idle();
    thread_run_until_idle();
    uart_send_string(label);
    uart_send_string(": done\n");
    return 0;
}

/* Cooperative workers yield after each printed round. */
static void foo(void *arg)
{
    unsigned int id = *(unsigned int *)arg;
    unsigned int i;

    for (i = 0; i < 5U; i++) {
        uart_send_string("[foo ");
        uart_send_dec(id);
        uart_send_string(":");
        uart_send_dec(i);
        uart_send_string("]");
        if (id == 3U) {
            uart_send_string("\n");
        } else {
            uart_send_string(" ");
        }
        thread_yield();
    }

    uart_send_string("[foo ");
    uart_send_dec(id);
    uart_send_string(":done]\n");
}

/* Roll back before returning: workers borrow arguments from the caller's stack. */
static int demo_create_workers(void (*entry)(void *), void *args[3])
{
    int tids[3];
    unsigned int i;

    thread_system_bootstrap_init();
    for (i = 0; i < 3; i++) {
        tids[i] = thread_create_kernel(entry, args[i]);
        if (tids[i] < 0) {
            while (i > 0) {
                thread_stop_tid(tids[--i], -1);
            }
            thread_reap_zombies();
            return -1;
        }
    }
    return 0;
}

static int demo_foo(void)
{
    unsigned int ids[3] = { 1, 2, 3 };
    void *args[3] = { &ids[0], &ids[1], &ids[2] };

    if (demo_create_workers(foo, args) != 0) {
        uart_send_string("demo foo: worker creation failed\n");
        return -1;
    }
    uart_send_string("[foo] expect [foo 1:0] [foo 2:0] [foo 3:0] ...\n");
    thread_enter_idle();
    return -1;
}

/* Same entry, separate stack arguments and execution contexts. */
static void demo_thread_worker(void *arg)
{
    struct demo_thread_arg *cfg = (struct demo_thread_arg *)arg;
    unsigned int i;

    for (i = 0; i < cfg->rounds; i++) {
        uart_send_string("[");
        uart_send(cfg->tag);
        uart_send_string(":");
        uart_send_dec(i);
        uart_send_string("]");
        if (cfg->tag == 'C') {
            uart_send_string("\n");
        } else {
            uart_send_string(" ");
        }
        thread_yield();
    }

    uart_send_string("[");
    uart_send(cfg->tag);
    uart_send_string(":done]\n");
}

static int demo_thread(void)
{
    struct demo_thread_arg cfg[3] = { { 'A', 5 }, { 'B', 5 }, { 'C', 5 } };
    void *args[3] = { &cfg[0], &cfg[1], &cfg[2] };

    if (demo_create_workers(demo_thread_worker, args) != 0) {
        uart_send_string("thread demo: worker creation failed\n");
        return -1;
    }
    uart_send_string("[thread] RR: [A:0] [B:0] [C:0] ...\n");
    uart_send_string("[thread] enter idle\n");
    thread_enter_idle();
    return -1;
}

/* Run the raw-image fork demo, then resume the shell. */
static int demo_fork(void)
{
    return demo_launch_user_program("fork", "[fork]", 1U);
}

static int demo_preempt(void)
{
    return demo_launch_user_program("preempt", "[preempt]", 2U);
}

static int demo_test(void)
{
    return demo_launch_user_program("test", "[test]", 1U);
}

static int demo_badinst(void)
{
    return demo_launch_user_program("badinst", "[badinst]", 1U);
}

/* Verify RX/TX IRQ delivery without polling fallback. */
static int demo_uart(void)
{
    struct uart_stats stats;
    char c;
    int pass;

    uart_reset_stats();

    uart_send_string("uartdemo: TX phase\n");
    uart_send_string("uartdemo: TX interrupt-driven output test line\n");

    uart_send_string("uartdemo: RX phase, type one printable key: ");
    c = uart_recv();
    uart_send_string("\n");

    uart_get_stats(&stats);

    uart_send_string("uartdemo: got '");
    uart_send(c);
    uart_send_string("'\n");

    uart_send_string("uartdemo: stats async_ready=");
    uart_send_dec(stats.async_ready);
    uart_send_string(" ext_irq=");
    uart_send_dec(stats.ext_irq_count);
    uart_send_string(" rx_irq=");
    uart_send_dec(stats.rx_irq_count);
    uart_send_string(" tx_irq=");
    uart_send_dec(stats.tx_irq_count);
    uart_send_string(" rx_fallback=");
    uart_send_dec(stats.rx_fallback_count);
    uart_send_string(" tx_poll=");
    uart_send_dec(stats.tx_poll_count);
    uart_send_string("\n");

    pass = (stats.async_ready != 0UL) &&
           (stats.ext_irq_count != 0UL) &&
           (stats.rx_irq_count != 0UL) &&
           (stats.tx_irq_count != 0UL) &&
           (stats.rx_fallback_count == 0UL) &&
           (stats.tx_poll_count == 0UL);

    if (pass) {
        uart_send_string("uartdemo: PASS - RX/TX are interrupt-driven\n");
    } else {
        uart_send_string("uartdemo: FAIL - counters do not match the interrupt-driven path\n");
    }
    return pass ? 0 : -1;
}

static int demo_stress(const char *arg)
{
    uint64_t count = 1024ULL;

    if (arg != 0 && arg[0] != '\0') {
        if (parse_u64_dec_local(arg, &count) != 0 || count == 0ULL) {
            uart_send_string("usage: demo stress [count]\n");
            return -1;
        }
    }

    uart_send_string("uartstress: started ");
    uart_send_dec((unsigned long)count);
    uart_send_string(" TX markers\n");

    if (demo_uart_stress_start(count) != 0) {
        uart_send_string("uartstress: failed to start TX stress\n");
        return -1;
    }
    return 0;
}

/* Shell entry: demo <name> [argument]. */
int demo_run(const char *name)
{
    char buf[64];
    char *cmd;
    char *arg;
    unsigned int i = 0;

    if (name == 0 || name[0] == '\0') {
        demo_print_usage();
        return -1;
    }

    while (name[i] != '\0' && i + 1U < sizeof(buf)) {
        buf[i] = name[i];
        i++;
    }
    buf[i] = '\0';
    split_first_arg_local(buf, &cmd, &arg);

    if (strcmp(cmd, "nested") == 0) {
        return demo_nested();
    }
    if (strcmp(cmd, "tasks") == 0) {
        return demo_tasks();
    }
    if (strcmp(cmd, "foo") == 0) {
        return demo_foo();
    }
    if (strcmp(cmd, "fork") == 0) {
        return demo_fork();
    }
    if (strcmp(cmd, "test") == 0) {
        return demo_test();
    }
    if (strcmp(cmd, "badinst") == 0) {
        return demo_badinst();
    }
    if (strcmp(cmd, "preempt") == 0) {
        return demo_preempt();
    }
    if (strcmp(cmd, "threads") == 0) {
        return demo_launch_user_program("threads", "[threads]", 1U);
    }
    if (strcmp(cmd, "uart") == 0) {
        return demo_uart();
    }
    if (strcmp(cmd, "thread") == 0) {
        return demo_thread();
    }
    if (strcmp(cmd, "stress") == 0) {
        return demo_stress(arg);
    }
    if (strcmp(cmd, "trace") == 0) {
        if (arg != 0 && (strcmp(arg, "reset") == 0)) {
            trace_reset();
            uart_send_string("demo trace: reset\n");
            return 0;
        }
        trace_dump();
        return 0;
    }
    if (strcmp(cmd, "mem") == 0 || strcmp(cmd, "kmtest") == 0 || strcmp(cmd, "memtest") == 0) {
        return demo_mem(arg);
    }

    uart_send_string("demo: unknown scenario\n");
    demo_print_usage();
    return -1;
}
