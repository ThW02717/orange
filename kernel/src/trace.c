/* Record IRQ paths without printing from their top halves. */
#include "trace.h"
#include "irqflags.h"
#include "timer.h"
#include "uart.h"

#define TRACE_CAPACITY 128U

struct trace_event {
    uint64_t now;
    uint32_t aux;
    uint8_t kind;
};

static struct trace_event g_trace[TRACE_CAPACITY];
static unsigned int g_trace_head = 0;
static unsigned int g_trace_count = 0;

static const char *trace_kind_name(enum trace_kind kind)
{
    switch (kind) {
    case TRACE_TIMER_TOP:      return "timer_top";
    case TRACE_TIMER_BOTTOM:   return "timer_bottom";
    case TRACE_UART_TOP_RX:    return "uart_top_rx";
    case TRACE_UART_TOP_TX:    return "uart_top_tx";
    case TRACE_UART_RX_BOTTOM: return "uart_rx_bottom";
    case TRACE_UART_TX_BOTTOM: return "uart_tx_bottom";
    case TRACE_RUN_TIMER:      return "run_timer";
    case TRACE_RUN_UART_RX:    return "run_uart_rx";
    case TRACE_RUN_UART_TX:    return "run_uart_tx";
    default:                        return "unknown";
    }
}

void trace_reset(void)
{
    uint64_t sstatus;

    sstatus = local_irq_save();
    g_trace_head = 0U;
    g_trace_count = 0U;
    local_irq_restore(sstatus);
}

void trace_record(enum trace_kind kind, uint32_t aux)
{
    uint64_t sstatus;
    unsigned int slot;

    sstatus = local_irq_save();
    slot = g_trace_head;
    g_trace[slot].now = timer_read();
    g_trace[slot].aux = aux;
    g_trace[slot].kind = (uint8_t)kind;
    g_trace_head = (slot + 1U) % TRACE_CAPACITY;
    if (g_trace_count < TRACE_CAPACITY) {
        g_trace_count++;
    }
    local_irq_restore(sstatus);
}

void trace_dump(void)
{
    struct trace_event snapshot[TRACE_CAPACITY];
    uint64_t sstatus;
    unsigned int count;
    unsigned int head;
    unsigned int start;
    unsigned int i;

    sstatus = local_irq_save();
    count = g_trace_count;
    head = g_trace_head;
    start = (head + TRACE_CAPACITY - count) % TRACE_CAPACITY;
    for (i = 0; i < count; i++) {
        snapshot[i] = g_trace[(start + i) % TRACE_CAPACITY];
    }
    local_irq_restore(sstatus);

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
        uart_send_string(trace_kind_name((enum trace_kind)snapshot[i].kind));
        uart_send_string(" aux=");
        uart_send_dec((unsigned long)snapshot[i].aux);
        uart_send_string("\n");
    }
}

int trace_seen_since(enum trace_kind kind, uint64_t since)
{
    uint64_t status = local_irq_save();
    unsigned int start = (g_trace_head + TRACE_CAPACITY -
                          g_trace_count) % TRACE_CAPACITY;
    int found = 0;

    for (unsigned int i = 0; i < g_trace_count; i++) {
        const struct trace_event *event =
            &g_trace[(start + i) % TRACE_CAPACITY];
        if (event->kind == kind && event->now >= since) {
            found = 1;
            break;
        }
    }
    local_irq_restore(status);
    return found;
}
