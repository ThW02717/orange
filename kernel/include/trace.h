#ifndef TRACE_H
#define TRACE_H

#include <stdint.h>

enum trace_kind {
    TRACE_TIMER_TOP = 0,
    TRACE_TIMER_BOTTOM,
    TRACE_UART_TOP_RX,
    TRACE_UART_TOP_TX,
    TRACE_UART_RX_BOTTOM,
    TRACE_UART_TX_BOTTOM,
    TRACE_RUN_TIMER,
    TRACE_RUN_UART_RX,
    TRACE_RUN_UART_TX,
};

void trace_record(enum trace_kind kind, uint32_t aux);
void trace_reset(void);
void trace_dump(void);
int trace_seen_since(enum trace_kind kind, uint64_t since);

#endif
