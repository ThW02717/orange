#ifndef DEMO_H
#define DEMO_H

#include <stdint.h>

enum demo_trace_kind {
    DEMO_TRACE_TIMER_TOP = 0,
    DEMO_TRACE_TIMER_BOTTOM,
    DEMO_TRACE_UART_TOP_RX,
    DEMO_TRACE_UART_TOP_TX,
    DEMO_TRACE_UART_RX_BOTTOM,
    DEMO_TRACE_UART_TX_BOTTOM,
    DEMO_TRACE_RUN_TIMER,
    DEMO_TRACE_RUN_UART_RX,
    DEMO_TRACE_RUN_UART_TX,
};

int demo_run(const char *name);
void demo_print_usage(void);
void demo_trace_reset(void);
void demo_trace_record(enum demo_trace_kind kind, uint32_t aux);
void demo_trace_dump(void);
int demo_nested_active(void);

#endif
