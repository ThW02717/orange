#include <stdint.h>
#include "demo.h"
#include "memory.h"
#include "memory_test.h"
#include "shell.h"
#include "trap.h"
#include "thread.h"
#include "timer.h"
#include "uart.h"

#define DEMO_TRACE_CAPACITY 128U

/* =========================
 * Demo Trace Support
 *
 * Records recent top-half / bottom-half events so nested-interrupt behavior
 * can be checked without relying only on UART output interleaving.
 * ========================= */
struct demo_trace_event {
    uint64_t now;
    uint32_t aux;
    uint8_t kind;
};

static struct demo_trace_event g_demo_trace[DEMO_TRACE_CAPACITY];
static unsigned int g_demo_trace_head = 0;
static unsigned int g_demo_trace_count = 0;
static int g_demo_nested_active = 0;

struct demo_thread_arg {
    char tag;
    unsigned int rounds;
};

static uint64_t demo_trace_irq_save(void)
{
    uint64_t sstatus;

    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    asm volatile("csrc sstatus, %0" : : "r"(0x2UL));
    return sstatus;
}

static void demo_trace_irq_restore(uint64_t sstatus)
{
    asm volatile("csrw sstatus, %0" : : "r"(sstatus));
}

static const char *demo_trace_kind_name(enum demo_trace_kind kind)
{
    switch (kind) {
    case DEMO_TRACE_TIMER_TOP:      return "timer_top";
    case DEMO_TRACE_TIMER_BOTTOM:   return "timer_bottom";
    case DEMO_TRACE_UART_TOP_RX:    return "uart_top_rx";
    case DEMO_TRACE_UART_TOP_TX:    return "uart_top_tx";
    case DEMO_TRACE_UART_RX_BOTTOM: return "uart_rx_bottom";
    case DEMO_TRACE_UART_TX_BOTTOM: return "uart_tx_bottom";
    case DEMO_TRACE_RUN_TIMER:      return "run_timer";
    case DEMO_TRACE_RUN_UART_RX:    return "run_uart_rx";
    case DEMO_TRACE_RUN_UART_TX:    return "run_uart_tx";
    default:                        return "unknown";
    }
}

static int streq_local(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

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
    uart_send_string("  demo foo         - create threads with thread_create(foo, ...)\n");
    uart_send_string("  demo fork        - run the user-space fork demo and return to shell\n");
    uart_send_string("  demo preempt     - run two busy user tasks to test timer preemption\n");
    uart_send_string("  demo nested      - print the recommended manual nested-interrupt test flow\n");
    uart_send_string("  demo thread      - run a cooperative RR kernel-thread demo and enter idle\n");
    uart_send_string("  demo test        - run the user-space syscall smoke test and return to shell\n");
    uart_send_string("  demo uart        - run the one-shot interrupt-driven UART demo\n");
    uart_send_string("  demo stress [n]  - queue UART TX markers for interleave testing\n");
    uart_send_string("  demo mem <name>  - run allocator test: basic|boundary|slab|multislab|reclaim|large|buddy|invalid|stress|all\n");
    uart_send_string("  demo trace       - dump recent top/bottom-half trace events\n");
    uart_send_string("  demo trace reset - clear recent trace events\n");
}

void demo_trace_reset(void)
{
    uint64_t sstatus;

    sstatus = demo_trace_irq_save();
    g_demo_trace_head = 0U;
    g_demo_trace_count = 0U;
    demo_trace_irq_restore(sstatus);
}

void demo_trace_record(enum demo_trace_kind kind, uint32_t aux)
{
    uint64_t sstatus;
    unsigned int slot;

    sstatus = demo_trace_irq_save();
    slot = g_demo_trace_head;
    g_demo_trace[slot].now = timer_read();
    g_demo_trace[slot].aux = aux;
    g_demo_trace[slot].kind = (uint8_t)kind;
    g_demo_trace_head = (slot + 1U) % DEMO_TRACE_CAPACITY;
    if (g_demo_trace_count < DEMO_TRACE_CAPACITY) {
        g_demo_trace_count++;
    }
    demo_trace_irq_restore(sstatus);
}

void demo_trace_dump(void)
{
    struct demo_trace_event snapshot[DEMO_TRACE_CAPACITY];
    uint64_t sstatus;
    unsigned int count;
    unsigned int head;
    unsigned int start;
    unsigned int i;

    sstatus = demo_trace_irq_save();
    count = g_demo_trace_count;
    head = g_demo_trace_head;
    start = (head + DEMO_TRACE_CAPACITY - count) % DEMO_TRACE_CAPACITY;
    for (i = 0; i < count; i++) {
        snapshot[i] = g_demo_trace[(start + i) % DEMO_TRACE_CAPACITY];
    }
    demo_trace_irq_restore(sstatus);

    uart_send_string("demo trace: ");
    uart_send_dec(count);
    uart_send_string(" events\n");
    for (i = 0; i < count; i++) {
        uint64_t ms = 0U;

        if (g_timebase_freq != 0U) {
            ms = ((snapshot[i].now - g_boot_time) * 1000ULL) / g_timebase_freq;
        }
        uart_send_string("trace[");
        uart_send_dec(i);
        uart_send_string("] t=");
        uart_send_dec((unsigned long)ms);
        uart_send_string("ms kind=");
        uart_send_string(demo_trace_kind_name((enum demo_trace_kind)snapshot[i].kind));
        uart_send_string(" aux=");
        uart_send_dec((unsigned long)snapshot[i].aux);
        uart_send_string("\n");
    }
}

int demo_nested_active(void)
{
    return g_demo_nested_active;
}

/* =========================
 * Nested Interrupt Demo
 *
 * This does not run the full test automatically. It prints the recommended
 * manual sequence so the user can combine addtimer + uartstress and then
 * check whether timer markers appear between UART markers.
 * ========================= */
static int demo_nested(void)
{
    demo_trace_reset();
    g_demo_nested_active = 0;
    uart_send_string("manual nested-interrupt test:\n");
    uart_send_string("  addtimer 1\n");
    uart_send_string("  addtimer 2\n");
    uart_send_string("  addtimer 3\n");
    uart_send_string("  uartstress\n");
    uart_send_string("expected: [T1]/[T2]/[T3] should appear between [U...] markers\n");
    return 0;
}

static int demo_mem(const char *name)
{
    return memory_run_kmtest(name);
}

static int demo_launch_user_program(const char *name,
                                    const char *label,
                                    unsigned int instances)
{
    uintptr_t entry;
    unsigned long size;
    unsigned int i;

    if (shell_load_user_program_named(name, &entry, &size) != 0) {
        uart_send_string("demo ");
        uart_send_string(name);
        uart_send_string(": failed to load bin/");
        uart_send_string(name);
        uart_send_string(".bin\n");
        return -1;
    }

    if (!thread_system_active()) {
        uart_send_string(label);
        uart_send_string(": bootstrap shell -> idle\n");
        thread_system_bootstrap_init();
    }

    for (i = 0; i < instances; i++) {
        uintptr_t user_stack_base;
        uintptr_t user_stack_top;

        user_stack_base = (uintptr_t)kmalloc(USER_STACK_SIZE);
        if (user_stack_base == 0) {
            uart_send_string("demo ");
            uart_send_string(name);
            uart_send_string(": failed to allocate user stack\n");
            return -1;
        }
        user_stack_top = (user_stack_base + USER_STACK_SIZE) & ~0xFUL;
        if (thread_create_user(entry, user_stack_base, user_stack_top) < 0) {
            kfree((void *)(uintptr_t)user_stack_base);
            uart_send_string("demo ");
            uart_send_string(name);
            uart_send_string(": failed to create user task\n");
            return -1;
        }
    }

    uart_send_string(label);
    uart_send_string(": run user demo\n");
    uart_tx_wait_idle();
    thread_run_until_idle();
    uart_send_string(label);
    uart_send_string(": done\n");
    (void)size;
    return 0;
}

/* =========================
 * demo foo
 *
 * Minimal thread-create example that mirrors the spec most directly:
 * thread_create(foo, ...). The goal is only to show that a plain function
 * name can be passed as the thread entry.
 * ========================= */
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

/* Bootstrap current shell flow as idle, create three foo workers, then hand
 * control to the cooperative scheduler by entering the idle loop. */
static int demo_foo(void)
{
    unsigned int ids[3];
    int tid;

    ids[0] = 1U;
    ids[1] = 2U;
    ids[2] = 3U;

    uart_send_string("[foo] bootstrap shell -> idle\n");
    thread_system_bootstrap_init();

    tid = thread_create(foo, &ids[0], 0);
    if (tid < 0) {
        uart_send_string("demo foo: failed to create foo #1\n");
        return -1;
    }
    tid = thread_create(foo, &ids[1], 0);
    if (tid < 0) {
        uart_send_string("demo foo: failed to create foo #2\n");
        return -1;
    }
    tid = thread_create(foo, &ids[2], 0);
    if (tid < 0) {
        uart_send_string("demo foo: failed to create foo #3\n");
        return -1;
    }

    uart_send_string("[foo] using thread_create(foo, ...)\n");
    uart_send_string("[foo] expect [foo 1:0] [foo 2:0] [foo 3:0] ...\n");
    thread_enter_idle();
    return -1;
}

/* =========================
 * demo thread
 *
 * Main cooperative RR scheduler demo. Three worker threads share the same
 * entry function but receive different arguments so the output shows
 * interleaving like A/B/C -> A/B/C -> ...
 * ========================= */
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

/* Bootstrap current shell flow as idle, create three tagged workers, then
 * enter the idle loop permanently. This demo intentionally does not return
 * to the shell. */
static int demo_thread(void)
{
    struct demo_thread_arg args[3];
    int tid;

    args[0].tag = 'A';
    args[0].rounds = 5U;
    args[1].tag = 'B';
    args[1].rounds = 5U;
    args[2].tag = 'C';
    args[2].rounds = 5U;

    uart_send_string("[thread] bootstrap shell -> idle\n");
    thread_system_bootstrap_init();

    tid = thread_create(demo_thread_worker, &args[0], 0);
    if (tid < 0) {
        uart_send_string("thread demo: failed to create worker A\n");
        return -1;
    }
    tid = thread_create(demo_thread_worker, &args[1], 0);
    if (tid < 0) {
        uart_send_string("thread demo: failed to create worker B\n");
        return -1;
    }
    tid = thread_create(demo_thread_worker, &args[2], 0);
    if (tid < 0) {
        uart_send_string("thread demo: failed to create worker C\n");
        return -1;
    }

    uart_send_string("[thread] RR: [A:0] [B:0] [C:0] ...\n");
    uart_send_string("[thread] enter idle\n");
    thread_enter_idle();
    return -1;
}

/* =========================
 * demo fork
 *
 * Launch the user-space fork smoke test, let the scheduler run until all user
 * work is gone, then return control to the shell.
 * ========================= */
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

/* =========================
 * UART Interrupt Demo
 *
 * Verifies the one-shot interrupt-driven UART RX/TX path and prints the
 * counters used to judge whether polling was avoided.
 * ========================= */
static int demo_uart(void)
{
    struct uart_demo_stats stats;
    char c;
    int pass;

    uart_demo_reset_stats();

    uart_send_string("uartdemo: TX phase\n");
    uart_send_string("uartdemo: TX interrupt-driven output test line\n");

    uart_send_string("uartdemo: RX phase, type one printable key: ");
    c = uart_recv();
    uart_send_string("\n");

    uart_demo_get_stats(&stats);

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

    if (uart_stress_start(count) != 0) {
        uart_send_string("uartstress: failed to start TX stress\n");
        return -1;
    }
    return 0;
}

/* =========================
 * Demo Command Dispatch
 *
 * Parses `demo <name> [arg]` and routes to the corresponding test entry.
 * ========================= */
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

    if (streq_local(cmd, "nested")) {
        return demo_nested();
    }
    if (streq_local(cmd, "foo")) {
        return demo_foo();
    }
    if (streq_local(cmd, "fork")) {
        return demo_fork();
    }
    if (streq_local(cmd, "test")) {
        return demo_test();
    }
    if (streq_local(cmd, "badinst")) {
        return demo_badinst();
    }
    if (streq_local(cmd, "preempt")) {
        return demo_preempt();
    }
    if (streq_local(cmd, "uart")) {
        return demo_uart();
    }
    if (streq_local(cmd, "thread")) {
        return demo_thread();
    }
    if (streq_local(cmd, "stress")) {
        return demo_stress(arg);
    }
    if (streq_local(cmd, "trace")) {
        if (arg != 0 && streq_local(arg, "reset")) {
            demo_trace_reset();
            uart_send_string("demo trace: reset\n");
            return 0;
        }
        demo_trace_dump();
        return 0;
    }
    if (streq_local(cmd, "mem") || streq_local(cmd, "kmtest") || streq_local(cmd, "memtest")) {
        return demo_mem(arg);
    }

    uart_send_string("demo: unknown scenario\n");
    demo_print_usage();
    return -1;
}
