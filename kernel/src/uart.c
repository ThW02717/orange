/*
 * UART byte I/O and bounded IRQ work. Start at uart_send()/uart_recv()
 * or uart_irq_top(); RX/TX callbacks drain the rings. Demo payloads live elsewhere.
 */
#include "irqflags.h"
#include "uart.h"
#include "uart_regs.h"
#include "trace.h"
#include "ringbuf.h"
#include "trap.h"
#include "plic.h"
#include "utils.h"
#include "bottom_half.h"
#include "memory.h"
#include "wait.h"

static struct wait_queue uart_rx_wait;
static struct wait_queue uart_tx_wait;

static struct ringbuf uart_rx_buf;
static struct ringbuf uart_tx_buf;
static uintptr_t uart_base_addr =
#ifdef QEMU
    0x10000000UL;
#else
    0UL;
#endif
static uint32_t uart_irq_number = 0U;
static int uart_irq_enabled = 0;
static volatile unsigned long uart_ext_irq_count = 0;
static volatile unsigned long uart_rx_irq_count = 0;
static volatile unsigned long uart_tx_irq_count = 0;
static volatile unsigned long uart_rx_fallback_count = 0;
static volatile unsigned long uart_tx_poll_count = 0;
static void (*uart_tx_refill)(void);

#define UART_DEFERRED_WORK_BUDGET 16U

uintptr_t uart_mmio_base(void)
{
    return uart_base_addr;
}

uint32_t uart_irq_id(void)
{
    return uart_irq_number;
}

/*
 * Read or write one UART MMIO register.
 * OrangePi RV2 exposes 32-bit MMIO registers, while QEMU uses byte access.
 */
static inline uint32_t uart_reg_read(uintptr_t reg)
{
#ifdef UART_MMIO_TEST
    return uart_test_reg_read(reg);
#elif defined(QEMU)
    return (uint32_t)(*(volatile uint8_t *)reg);
#else
    return get32(reg);
#endif
}

static inline void uart_reg_write(uintptr_t reg, uint32_t value)
{
#ifdef UART_MMIO_TEST
    uart_test_reg_write(reg, value);
#elif defined(QEMU)
    *(volatile uint8_t *)reg = (uint8_t)value;
#else
    put32(reg, value);
#endif
}

static void uart_ier_set_bits(uint32_t bits)
{
    uint64_t status = local_irq_save();
    uart_reg_write(UART_IER_REG, uart_reg_read(UART_IER_REG) | bits);
    local_irq_restore(status);
}

static void uart_ier_clear_bits(uint32_t bits)
{
    uint64_t status = local_irq_save();
    uart_reg_write(UART_IER_REG, uart_reg_read(UART_IER_REG) & ~bits);
    local_irq_restore(status);
}

static void uart_rx_mask(void)
{
    uart_ier_clear_bits(UART_IER_RX_ENABLE | UART_IER_LINE_ENABLE | UART_IER_RTO_ENABLE);
}

static void uart_rx_unmask(void)
{
    uart_ier_set_bits(UART_IER_RX_ENABLE | UART_IER_LINE_ENABLE | UART_IER_RTO_ENABLE);
}

static void uart_tx_mask(void)
{
    uart_ier_clear_bits(UART_IER_TX_ENABLE);
}

static void uart_tx_unmask(void)
{
    uart_ier_set_bits(UART_IER_TX_ENABLE);
}

static int uart_tx_buf_is_empty(void)
{
    uint64_t sstatus;
    int ret;

    sstatus = local_irq_save();
    ret = ringbuf_is_empty(&uart_tx_buf);
    local_irq_restore(sstatus);
    return ret;
}

static int uart_tx_try_drain_one(void);

void uart_tx_wait_idle(void)
{
    for (;;) {
        uint64_t status = local_irq_save();
        int idle = ringbuf_is_empty(&uart_tx_buf) &&
                   (uart_reg_read(UART_LSR_REG) & LSR_TX_IDLE) != 0U;

        if (idle) {
            local_irq_restore(status);
            return;
        }
        /* This synchronous flush also works inside IRQ-off/deferred paths. */
        (void)uart_tx_try_drain_one();
        local_irq_restore(status);
    }
}

static int uart_tx_try_drain_one(void)
{
    char c;
    uint64_t status = local_irq_save();

    /* Only feed THR when the transmitter can accept a new byte. */
    if ((uart_reg_read(UART_LSR_REG) & LSR_TX_IDLE) == 0U) {
        local_irq_restore(status);
        return 0;
    }

    if (ringbuf_pop(&uart_tx_buf, &c) != 0) {
        local_irq_restore(status);
        return 0;
    }

    /*
     * THR is the transmit holding register: writing one byte starts the next
     * piece of serial output.
     */
    uart_reg_write(UART_THR_REG, (uint32_t)(uint8_t)c);
    wait_queue_wake_one(&uart_tx_wait);
    local_irq_restore(status);
    return 1;
}

static void uart_irq_ack_stale_status(void)
{
    /*
     * Boot ROM / firmware may leave old UART status pending. Read the common
     * status registers once so interrupt-driven I/O starts from a clean slate.
     */
    (void)uart_reg_read(UART_LSR_REG);
    (void)uart_reg_read(UART_RBR_REG);
    (void)uart_reg_read(UART_IIR_REG);
    (void)uart_reg_read(UART_MSR_REG);
}

/* Reset software queues before arming UART interrupts. */
static void uart_reset_runtime_state(void)
{
    ringbuf_init(&uart_rx_buf);
    ringbuf_init(&uart_tx_buf);

    uart_irq_enabled = 0;
    clear_bottom_half(BH_UART_RX);
    clear_bottom_half(BH_UART_TX);
    uart_tx_refill = 0;

    uart_reset_stats();
}

static void uart_flush_hardware_rx_fifo(void)
{
    unsigned int budget = 256U;

    while (budget > 0U && (uart_reg_read(UART_LSR_REG) & LSR_RX_READY) != 0U) {
        (void)uart_reg_read(UART_RBR_REG);
        budget--;
    }
}

void uart_irq_init(void)
{
    /* Keep the handler out while rings and UART registers are reset. */
    local_irq_disable();
    uart_reset_runtime_state();

    /* Route the DT-discovered UART IRQ into hart0's S-mode context. */
    plic_set_priority(uart_irq_id(), 1U);
    plic_enable_irq(uart_irq_id());
    plic_set_threshold(0U);

    /*
     * This UART block is DT-compatible with pxa-uart. Besides the basic RX
     * enable bit, it also needs:
     * - FIFO enabled and cleared,
     * - OUT2 asserted so the IRQ output line is driven,
     * - UUE/LINE/RTO bits enabled so sparse shell input still raises an IRQ.
     */
    uart_reg_write(UART_IER_REG, 0U);
    uart_reg_write(UART_FCR_REG, 0x07U);
    uart_reg_write(UART_MCR_REG, uart_reg_read(UART_MCR_REG) | UART_MCR_OUT2);
    uart_flush_hardware_rx_fifo();
    uart_irq_ack_stale_status();
#ifdef QEMU
    uart_ier_set_bits(UART_IER_RX_ENABLE | UART_IER_LINE_ENABLE);
#else
    uart_ier_set_bits(UART_IER_UUE |
                      UART_IER_RX_ENABLE |
                      UART_IER_LINE_ENABLE |
                      UART_IER_RTO_ENABLE);
#endif
    uart_ier_clear_bits(UART_IER_TX_ENABLE);
    uart_irq_ack_stale_status();

    /* Finally allow supervisor external interrupts to reach trap_entry. */
    uart_irq_enabled = 1;
    asm volatile("csrs sie, %0" : : "r"(SIE_SEIE) : "memory");
    local_irq_enable();
}

void uart_send(char c)
{
    if (uart_irq_enabled && local_irqs_enabled()) {
        int sleepable = thread_can_sleep();

        for (;;) {
            uint64_t status = local_irq_save();

            if (ringbuf_push(&uart_tx_buf, c) == 0) {
                uart_tx_unmask();
                local_irq_restore(status);
                return;
            }
            uart_tx_unmask();
            if (sleepable) {
                /* Enqueue before IRQ delivery can free space and wake us. */
                (void)wait_queue_sleep_locked(&uart_tx_wait);
            } else {
                /* Idle and bottom-half callbacks cannot block this runner. */
                (void)uart_tx_try_drain_one();
            }
            local_irq_restore(status);
        }
    }

    /* Before IRQ setup, or with SIE clear, output must make progress here. */
    for (;;) {
        uint64_t status = local_irq_save();

        /* IRQ delivery may have started while polling. Drain older bytes
         * before checking THR, and keep that check paired with the write.
         */
        if (!ringbuf_is_empty(&uart_tx_buf)) {
            (void)uart_tx_try_drain_one();
        } else if ((uart_reg_read(UART_LSR_REG) & LSR_TX_IDLE) != 0U) {
            uart_tx_poll_count++;
            uart_reg_write(UART_THR_REG, (uint32_t)(uint8_t)c);
            local_irq_restore(status);
            return;
        }
        local_irq_restore(status);
    }
}

char uart_recv(void)
{
    if (uart_irq_enabled && local_irqs_enabled()) {
        int sleepable = thread_can_sleep();

        while (1) {
            uint64_t status = local_irq_save();
            char c;

            if (ringbuf_pop(&uart_rx_buf, &c) == 0) {
                local_irq_restore(status);
                return c;
            }
            if (sleepable)
                (void)wait_queue_sleep_locked(&uart_rx_wait);
            local_irq_restore(status);
        }
    }

    /*
     * Before IRQ setup or inside an IRQ-off caller, consume buffered input
     * first, then poll hardware. Normal user reads enable IRQs and can sleep.
     */
    for (;;) {
        uint64_t status = local_irq_save();
        char c;

        /* An IRQ may have moved the byte into the ring while we waited.
         * Check both sources with IRQs off; RBR consumes the hardware byte.
         */
        if (ringbuf_pop(&uart_rx_buf, &c) == 0) {
            local_irq_restore(status);
            return c;
        }
        if ((uart_reg_read(UART_LSR_REG) & LSR_RX_READY) != 0U) {
            c = (char)(uart_reg_read(UART_RBR_REG) & 0xFFU);
            uart_rx_fallback_count++;
            local_irq_restore(status);
            return c;
        }
        local_irq_restore(status);
    }
}

void uart_send_string(const char *str)
{
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '\n') {
            uart_send('\r');
        }
        uart_send(str[i]);
    }
}

void uart_send_hex(unsigned long value)
{
    for (int shift = (int)(sizeof(unsigned long) * 8U) - 4; shift >= 0; shift -= 4) {
        unsigned long digit = (value >> shift) & 0xFUL;
        char c = (digit < 10UL) ? (char)('0' + digit) : (char)('a' + (digit - 10UL));
        uart_send(c);
    }
}

void uart_send_dec(unsigned long value)
{
    char buf[24];
    int i = 0;

    if (value == 0UL) {
        uart_send('0');
        return;
    }

    while (value > 0UL && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10UL));
        value /= 10UL;
    }

    while (i-- > 0) {
        uart_send(buf[i]);
    }
}

static void uart_rx_drain(unsigned int budget)
{
    while (budget-- > 0U) {
        uint64_t status = local_irq_save();
        char c;

        /* Pair the readiness check with the consuming RBR read. Keep ring
         * insertion and the reader wakeup in the same IRQ-off section.
         */
        if ((uart_reg_read(UART_LSR_REG) & LSR_RX_READY) == 0U) {
            local_irq_restore(status);
            break;
        }
        c = (char)(uart_reg_read(UART_RBR_REG) & 0xFFU);
        if (ringbuf_push(&uart_rx_buf, c) == 0)
            wait_queue_wake_one(&uart_rx_wait);
        uart_rx_irq_count++;
        local_irq_restore(status);
    }
}

void uart_irq_top(void)
{
    uint32_t status;

    uart_ext_irq_count++;
    status = uart_reg_read(UART_LSR_REG);

    if (status & LSR_RX_READY) {
        trace_record(TRACE_UART_TOP_RX, 0U);
        uart_rx_mask();
        /* Keep bytes in the hardware FIFO until the RX bottom half runs. */
        raise_bottom_half(BH_UART_RX);
    }

    if ((status & LSR_TX_IDLE) && (uart_reg_read(UART_IER_REG) & UART_IER_TX_ENABLE)) {
        trace_record(TRACE_UART_TOP_TX, (uint32_t)uart_tx_buf.count);
        uart_tx_mask();
        raise_bottom_half(BH_UART_TX);
    }
}

void uart_rx_run_bottom_half(void)
{
    unsigned int budget = UART_DEFERRED_WORK_BUDGET;

    trace_record(TRACE_UART_RX_BOTTOM, budget);

    uart_rx_drain(budget);

    if ((uart_reg_read(UART_LSR_REG) & LSR_RX_READY) != 0U) {
        raise_bottom_half(BH_UART_RX);
        return;
    }

    uart_rx_unmask();
}

void uart_tx_run_bottom_half(void)
{
    unsigned int budget = UART_DEFERRED_WORK_BUDGET;
    uint64_t status;

    trace_record(TRACE_UART_TX_BOTTOM, uart_tx_buf.count);
    if (uart_tx_buf_is_empty() && uart_tx_refill != 0) {
        uart_tx_refill();
    }
    while (budget > 0U && uart_tx_try_drain_one()) {
        uart_tx_irq_count++;
        budget--;
    }
    if (uart_tx_buf_is_empty() && uart_tx_refill != 0) {
        uart_tx_refill();
    }
    /* Decide whether to arm TX from the same protected queue state. */
    status = local_irq_save();
    if (!ringbuf_is_empty(&uart_tx_buf)) {
        uart_tx_unmask();
        /* Wait for TX-ready rather than spinning the runner on a busy device. */
        if ((uart_reg_read(UART_LSR_REG) & LSR_TX_IDLE) != 0)
            raise_bottom_half(BH_UART_TX);
    } else {
        uart_tx_mask();
    }
    local_irq_restore(status);
}

void uart_reset_stats(void)
{
    uint64_t sstatus;

    sstatus = local_irq_save();
    uart_ext_irq_count = 0;
    uart_rx_irq_count = 0;
    uart_tx_irq_count = 0;
    uart_rx_fallback_count = 0;
    uart_tx_poll_count = 0;
    local_irq_restore(sstatus);
}

void uart_get_stats(struct uart_stats *stats)
{
    uint64_t status;

    if (stats == 0) {
        return;
    }

    status = local_irq_save();
    stats->irq_enabled = (unsigned long)uart_irq_enabled;
    stats->ext_irq_count = uart_ext_irq_count;
    stats->rx_irq_count = uart_rx_irq_count;
    stats->tx_irq_count = uart_tx_irq_count;
    stats->rx_fallback_count = uart_rx_fallback_count;
    stats->tx_poll_count = uart_tx_poll_count;
    local_irq_restore(status);
}

void uart_set_platform(uintptr_t base, uint32_t irq)
{
    uart_base_addr = base;
    uart_irq_number = irq;
}

/* Queue the whole buffer or return -1. Never wait for room in the TX ring. */
int uart_write_async(const char *buf, unsigned int size)
{
    uint64_t status = local_irq_save();

    if (buf == 0 || !uart_irq_enabled || size > RINGBUF_CAPACITY - uart_tx_buf.count) {
        local_irq_restore(status);
        return -1;
    }
    for (unsigned int i = 0; i < size; i++) {
        ringbuf_push(&uart_tx_buf, buf[i]);
    }
    if (size != 0) {
        uart_tx_unmask();
    }
    local_irq_restore(status);
    return 0;
}

/* One streaming producer may refill an empty TX ring from the bottom half. */
int uart_tx_set_refill(void (*refill)(void))
{
    uint64_t status = local_irq_save();

    if (refill != 0 && (!uart_irq_enabled ||
        (uart_tx_refill != 0 && uart_tx_refill != refill))) {
        local_irq_restore(status);
        return -1;
    }
    uart_tx_refill = refill;
    local_irq_restore(status);
    return 0;
}

#ifdef KERNEL_TEST
void uart_test_set_irq_enabled(int enabled)
{
    uart_irq_enabled = enabled;
}

unsigned int uart_test_tx_count(void)
{
    return uart_tx_buf.count;
}
#endif
