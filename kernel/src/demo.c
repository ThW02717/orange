#include <stdint.h>
#include "demo.h"
#include "memory.h"
#include "memory_test.h"
#include "timer.h"
#include "uart.h"

#define DEMO_TRACE_CAPACITY 128U

struct demo_trace_event {
    uint64_t now;
    uint32_t aux;
    uint8_t kind;
};

static struct demo_trace_event g_demo_trace[DEMO_TRACE_CAPACITY];
static unsigned int g_demo_trace_head = 0;
static unsigned int g_demo_trace_count = 0;
static int g_demo_nested_active = 0;

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

static void demo_timer_callback(void *arg)
{
    unsigned long id = (unsigned long)(uintptr_t)arg;

    demo_trace_record(DEMO_TRACE_TIMER_BOTTOM, (uint32_t)id);
    if (id == 8UL) {
        g_demo_nested_active = 0;
        memory_set_allocator_log_enabled(1);
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
    uart_send_string("  demo nested      - schedule T1..T8 at 0.25s steps and start UART stress\n");
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

static int demo_nested(void)
{
    static const unsigned int timer_count = 8U;
    uint64_t step_ticks;
    unsigned int i;

    if (g_timebase_freq == 0U) {
        uart_send_string("demo nested: timer frequency unavailable\n");
        return -1;
    }
    step_ticks = (uint64_t)g_timebase_freq / 4ULL;
    if (step_ticks == 0U) {
        step_ticks = 1U;
    }
    demo_trace_reset();
    g_demo_nested_active = 1;
    memory_set_allocator_log_enabled(0);

    for (i = 0; i < timer_count; i++) {
        if (add_timer(demo_timer_callback, (void *)(uintptr_t)(i + 1U),
                      (uint64_t)(i + 1U) * step_ticks) != 0) {
            g_demo_nested_active = 0;
            memory_set_allocator_log_enabled(1);
            uart_send_string("demo nested: failed to schedule timer markers\n");
            return -1;
        }
    }

    if (uart_stress_start(8192U) != 0) {
        g_demo_nested_active = 0;
        memory_set_allocator_log_enabled(1);
        uart_send_string("demo nested: failed to start UART stress\n");
        return -1;
    }

    uart_send_string("demo nested: schedule T1..T8 at 0.25s steps and start UART stress\n");
    return 0;
}

static int demo_mem(const char *name)
{
    return memory_run_kmtest(name);
}

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
