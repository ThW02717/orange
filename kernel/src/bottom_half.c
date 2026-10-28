/* Fixed bottom-half types. Payloads stay in their owning subsystem. */
#include "bottom_half.h"
#include "custom_work.h"
#include "irqflags.h"
#include "timer.h"
#include "uart.h"
#include "trace.h"

static unsigned int pending;
static int running;

void bottom_half_init(void)
{
    pending = 0;
    running = 0;
    custom_work_init();
}

void raise_bottom_half(enum bottom_half type)
{
    uint64_t status;

    if ((unsigned int)type >= BH_COUNT)
        return;
    status = local_irq_save();
    pending |= 1U << type;
    local_irq_restore(status);
}

void clear_bottom_half(enum bottom_half type)
{
    uint64_t status;

    if ((unsigned int)type >= BH_COUNT)
        return;
    status = local_irq_save();
    pending &= ~(1U << type);
    local_irq_restore(status);
}

int bottom_halves_pending(void)
{
    uint64_t status = local_irq_save();
    int any = pending != 0;

    local_irq_restore(status);
    return any;
}

void run_bottom_halves(void)
{
    uint64_t status = local_irq_save();

    /* Nested traps leave notifications for the paused outer runner. */
    if (running) {
        local_irq_restore(status);
        return;
    }
    running = 1;
    while (pending != 0) {
        unsigned int round = pending;

        /* Clear before enabling IRQs. New notifications belong to next round. */
        pending = 0;
        local_irq_enable();
        if (round & (1U << BH_TIMER)) {
            trace_record(TRACE_RUN_TIMER, BH_TIMER);
            timer_run_bottom_half();
        }
        if (round & (1U << BH_UART_RX)) {
            trace_record(TRACE_RUN_UART_RX, BH_UART_RX);
            uart_rx_run_bottom_half();
        }
        if (round & (1U << BH_UART_TX)) {
            trace_record(TRACE_RUN_UART_TX, BH_UART_TX);
            uart_tx_run_bottom_half();
        }
        if (round & (1U << BH_CUSTOM))
            run_custom_callbacks();
        (void)local_irq_save();
    }
    running = 0;
    local_irq_restore(status);
}
