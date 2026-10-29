/* Run the production driver with a small UART model. Inject an IRQ after
 * sampling LSR; delivery waits until SIE is set, just like a local IRQ.
 */
#include <stdint.h>
#include "irqflags.h"

static uint32_t uart_test_reg_read(uintptr_t reg);
static void uart_test_reg_write(uintptr_t reg, uint32_t value);
static void uart_test_irq_restore(uint64_t status);

#define UART_MMIO_TEST
#define local_irq_restore uart_test_irq_restore
#include "../kernel/src/uart.c"
#undef local_irq_restore

static int model_active;
static unsigned char rx[32], tx[32];
static unsigned int rx_head, rx_count, tx_count;
static unsigned int empty_reads, busy_writes, reads, tx_busy_reads;
static uint32_t ier;
static int arrival, pending_irq, in_irq, inject_tx, inject_byte;
static unsigned int delivered;

static void deliver_irq(void)
{
    uint64_t status;

    if (!pending_irq || in_irq || !local_irqs_enabled())
        return;
    pending_irq = 0;
    in_irq = 1;
    status = local_irq_save();
    delivered++;
    if (inject_byte) {
        rx[rx_head + rx_count++] = 'p';
        inject_byte = 0;
    }
    if (inject_tx)
        uart_send('!');
    else
        uart_irq_top();
    in_irq = 0;
    local_irq_restore(status);
}

static void uart_test_irq_restore(uint64_t status)
{
    local_irq_restore(status);
    if (model_active)
        deliver_irq();
}

static uint32_t uart_test_reg_read(uintptr_t reg)
{
    uint32_t status;

    if (!model_active)
        return *(volatile uint8_t *)reg;
    if (reg == UART_LSR_REG) {
        status = (rx_count ? LSR_RX_READY : 0U) |
                 (tx_busy_reads ? 0U : LSR_TX_IDLE);
        if (tx_busy_reads)
            tx_busy_reads--;
        /* Bound a broken polling loop so the test reports a failure. */
        if (++reads > 1000)
            status |= LSR_RX_READY | LSR_TX_IDLE;
        if (arrival) {
            arrival = 0;
            pending_irq = 1;
            deliver_irq();
        }
        return status;
    }
    if (reg == UART_RBR_REG) {
        if (!rx_count) {
            empty_reads++;
            return 0xee;
        }
        rx_count--;
        return rx[rx_head++];
    }
    if (reg == UART_IER_REG)
        return ier;
    return 0;
}

static void uart_test_reg_write(uintptr_t reg, uint32_t value)
{
    if (!model_active) {
        *(volatile uint8_t *)reg = (uint8_t)value;
        return;
    }
    if (reg == UART_IER_REG)
        ier = value;
    if (reg == UART_THR_REG) {
        if (tx_busy_reads)
            busy_writes++;
        if (tx_count < sizeof(tx))
            tx[tx_count++] = (unsigned char)value;
        tx_busy_reads = 1;
    }
}

static void reset_model(void)
{
    local_irq_disable();
    model_active = 1;
    rx_head = rx_count = tx_count = 0;
    empty_reads = busy_writes = reads = tx_busy_reads = 0;
    arrival = pending_irq = in_irq = inject_tx = inject_byte = 0;
    delivered = 0;
    ier = UART_IER_TX_ENABLE;
    uart_reset_runtime_state();
    uart_irq_enabled = 0;
    local_irq_enable();
}

const char *test_uart_races(void)
{
    uint64_t status = local_irq_save();
    uint64_t sources;
    const char *error = 0;
    char c;

    /* Only the controlled arrivals below may interrupt these scenarios. */
    asm volatile("csrrw %0, sie, zero" : "=r"(sources) : : "memory");
#define VERIFY(cond, message) do { if (!(cond)) { error = message; goto out; } } while (0)

    reset_model();
    rx[0] = 'p';
    rx_count = 1;
    arrival = 1;
    uart_rx_run_bottom_half();
    VERIFY(delivered == 1 && !empty_reads && uart_rx_buf.count == 1,
           "RX bottom half consumed an empty RBR after a nested UART IRQ");
    VERIFY(uart_rx_pop_char(&c) == 0 && c == 'p', "RX byte changed");
    VERIFY(local_irqs_enabled(), "RX bottom half left IRQs disabled");

    reset_model();
    rx[0] = 'h'; rx[1] = 'e'; rx[2] = 'l'; rx[3] = 'p';
    rx_count = 4;
    arrival = 1;
    uart_rx_run_bottom_half();
    VERIFY(delivered == 1 && !empty_reads && uart_rx_buf.count == 4,
           "RX interleaving lost or duplicated bytes");
    for (unsigned int i = 0; i < 4; i++)
        VERIFY(uart_rx_pop_char(&c) == 0 && c == "help"[i], "RX FIFO order changed");

    reset_model();
    rx[0] = 'p';
    rx_count = 1;
    arrival = 1;
    c = uart_recv();
    VERIFY(c == 'p' && delivered == 1 && !empty_reads && !uart_rx_buf.count,
           "RX polling read an empty RBR after a nested UART IRQ");

    reset_model();
    arrival = inject_byte = 1;
    c = uart_recv();
    VERIFY(c == 'p' && delivered == 1 && !empty_reads && !uart_rx_buf.count && reads < 1000,
           "RX polling missed the byte moved into the ring while waiting");

    reset_model();
    arrival = inject_tx = 1;
    uart_send('x');
    VERIFY(delivered == 1 && !busy_writes && tx_count == 2 && tx[0] == 'x' && tx[1] == '!',
           "TX polling wrote to a busy THR after an interrupt");
    VERIFY(local_irqs_enabled(), "TX polling left IRQs disabled");

    reset_model();
    uart_irq_enabled = 1;
    tx_busy_reads = 2;
    VERIFY(uart_write_async("a", 1) == 0, "TX fixture enqueue failed");
    uart_send('b');
    VERIFY(!busy_writes && tx_count == 2 && tx[0] == 'a' && tx[1] == 'b',
           "TX polling overtook queued output before first UART interrupt");

    reset_model();
    local_irq_disable();
    rx[0] = 'z';
    rx_count = 1;
    VERIFY(uart_recv() == 'z' && !local_irqs_enabled(), "IRQ-off RX changed SIE");
    uart_send('z');
    VERIFY(!local_irqs_enabled() && tx_count == 1 && !busy_writes, "IRQ-off TX changed SIE");

out:
    local_irq_disable();
    pending_irq = 0;
    uart_reset_runtime_state();
    uart_irq_enabled = 0;
    model_active = 0;
    asm volatile("csrw sie, %0" : : "r"(sources) : "memory");
    local_irq_restore(status);
    return error;
#undef VERIFY
}
