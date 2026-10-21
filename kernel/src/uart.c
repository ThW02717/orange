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
#include "deferred_work.h"
#include "memory.h"

static struct ringbuf g_uart_rx_buf;
static struct ringbuf g_uart_tx_buf;
static uintptr_t g_uart_mmio_base =
#ifdef QEMU
    0x10000000UL;
#else
    0UL;
#endif
static uint32_t g_uart_irq_id = 0U;
static int g_uart_irq_enabled = 0;
static int g_uart_async_ready = 0;
static volatile unsigned long g_uart_ext_irq_count = 0;
static volatile unsigned long g_uart_rx_irq_count = 0;
static volatile unsigned long g_uart_tx_irq_count = 0;
static volatile unsigned long g_uart_rx_fallback_count = 0;
static volatile unsigned long g_uart_tx_poll_count = 0;
static void (*g_uart_tx_refill)(void);

static int uart_rx_work_run(struct deferred_work *task);
static int uart_tx_work_run(struct deferred_work *task);

static struct deferred_work g_uart_rx_work = {
    .type = DEFERRED_WORK_UART_RX,
    .prio = 0,
    .state = DEFERRED_WORK_IDLE,
    .next = 0,
    .run = uart_rx_work_run,
};
static struct deferred_work g_uart_tx_work = {
    .type = DEFERRED_WORK_UART_TX,
    .prio = 0,
    .state = DEFERRED_WORK_IDLE,
    .next = 0,
    .run = uart_tx_work_run,
};

#define UART_DEFERRED_WORK_BUDGET 16U

uintptr_t uart_mmio_base(void)
{
    return g_uart_mmio_base;
}

uint32_t uart_irq_id(void)
{
    return g_uart_irq_id;
}

/*
 * Read or write one UART MMIO register.
 * OrangePi RV2 exposes 32-bit MMIO registers, while QEMU uses byte access.
 */
static inline uint32_t uart_reg_read(uintptr_t reg)
{
#ifdef QEMU
    return (uint32_t)(*(volatile uint8_t *)reg);
#else
    return get32(reg);
#endif
}

static inline void uart_reg_write(uintptr_t reg, uint32_t value)
{
#ifdef QEMU
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

static uint32_t uart_irq_status_snapshot(void)
{
    return uart_reg_read(UART_LSR_REG);
}

static int uart_rx_ready_from_status(uint32_t status)
{
    return (status & LSR_RX_READY) != 0U;
}

static int uart_tx_ready_from_status(uint32_t status)
{
    uint32_t ier;

    ier = uart_reg_read(UART_IER_REG);
    return ((status & LSR_TX_IDLE) != 0U) && ((ier & UART_IER_TX_ENABLE) != 0U);
}

/*
 * RX/TX ring buffers are shared between interrupt context and normal shell
 * code. Keep every head/tail/count update inside a tiny local critical
 * section so concurrent push/pop pairs cannot corrupt the FIFO state.
 */
static int uart_rx_push_char(char c)
{
    uint64_t sstatus;
    int ret;

    sstatus = local_irq_save();
    ret = ringbuf_push(&g_uart_rx_buf, c);
    local_irq_restore(sstatus);
    return ret;
}

static int uart_rx_pop_char(char *out)
{
    uint64_t sstatus;
    int ret;

    sstatus = local_irq_save();
    ret = ringbuf_pop(&g_uart_rx_buf, out);
    local_irq_restore(sstatus);
    return ret;
}

static int uart_tx_push_char(char c)
{
    uint64_t sstatus;
    int ret;

    sstatus = local_irq_save();
    ret = ringbuf_push(&g_uart_tx_buf, c);
    local_irq_restore(sstatus);
    return ret;
}

static int uart_tx_pop_char(char *out)
{
    uint64_t sstatus;
    int ret;

    sstatus = local_irq_save();
    ret = ringbuf_pop(&g_uart_tx_buf, out);
    local_irq_restore(sstatus);
    return ret;
}

static int uart_tx_buf_is_empty(void)
{
    uint64_t sstatus;
    int ret;

    sstatus = local_irq_save();
    ret = ringbuf_is_empty(&g_uart_tx_buf);
    local_irq_restore(sstatus);
    return ret;
}

static int uart_tx_try_drain_one(void);

void uart_tx_wait_idle(void)
{
    for (;;) {
        if (uart_tx_buf_is_empty() && (uart_reg_read(UART_LSR_REG) & LSR_TX_IDLE) != 0U) {
            return;
        }
        /* This synchronous flush also works inside IRQ-off/deferred paths. */
        if (!uart_tx_buf_is_empty()) {
            (void)uart_tx_try_drain_one();
        }
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

    if (uart_tx_pop_char(&c) != 0) {
        local_irq_restore(status);
        return 0;
    }

    /*
     * THR is the transmit holding register: writing one byte starts the next
     * piece of serial output.
     */
    uart_reg_write(UART_THR_REG, (uint32_t)(uint8_t)c);
    local_irq_restore(status);
    return 1;
}

static void uart_tx_kick(void)
{
    /*
     * TX interrupts stay disabled while the queue is empty. Once new data is
     * queued, enable TX-ready interrupts and opportunistically push one byte
     * so the hardware pipeline starts moving immediately.
     */
    uart_tx_unmask();
    (void)uart_tx_try_drain_one();
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

/*
 * Drop any software-side backlog from a previous demo run and drain stale
 * hardware RX state before re-arming the interrupt-driven console path.
 */
static void uart_reset_runtime_state(void)
{
    ringbuf_init(&g_uart_rx_buf);
    ringbuf_init(&g_uart_tx_buf);

    g_uart_async_ready = 0;
    g_uart_rx_work.state = DEFERRED_WORK_IDLE;
    g_uart_rx_work.next = 0;
    g_uart_tx_work.state = DEFERRED_WORK_IDLE;
    g_uart_tx_work.next = 0;
    g_uart_tx_refill = 0;

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

void uart_init(void)
{
#ifdef QEMU
    uart_reg_write(UART_IER_REG, 0);
    uart_reg_write(UART_LCR_REG, 0x80);
    uart_reg_write(UART_DLL_REG, 0x01);
    uart_reg_write(UART_DLH_REG, 0x00);
    uart_reg_write(UART_LCR_REG, 0x03);
    uart_reg_write(UART_FCR_REG, 0x07);
    uart_reg_write(UART_MCR_REG, 0x03);
#else
    /* Preserve the UART configuration established by the bootloader. */
#endif
}

void uart_irq_init(void)
{
    /*
     * Start from a clean console state every boot/load so an earlier stress
     * run cannot leave backlog in the software rings or deferred-task state.
     */
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
    g_uart_irq_enabled = 1;
    asm volatile("csrs sie, %0" : : "r"(SIE_SEIE) : "memory");
    local_irq_enable();
}

void uart_send(char c)
{
    /*
     * Once the first real UART IRQ is observed, all output is queued and
     * drained by TX-ready interrupts.
     */
    if (g_uart_async_ready) {
        if (local_irqs_enabled()) {
            while (uart_tx_push_char(c) != 0) {
                /*
                 * A deferred callback may be the current producer. It cannot
                 * wait for its own runner to return and service TX work.
                 */
                (void)uart_tx_try_drain_one();
            }
            uart_tx_kick();
            return;
        }

        /*
         * Preserve FIFO order before IRQ-off direct output. Otherwise newer
         * syscall/log bytes would overtake text already queued by the shell.
         */
        while (!uart_tx_buf_is_empty()) {
            (void)uart_tx_try_drain_one();
        }
    }

    /*
     * Early boot fallback: keep console output alive until the first real
     * UART interrupt proves that the asynchronous path is active.
     */
    while ((uart_reg_read(UART_LSR_REG) & LSR_TX_IDLE) == 0U) {
    }
    g_uart_tx_poll_count++;
    uart_reg_write(UART_THR_REG, (uint32_t)(uint8_t)c);
}

char uart_recv(void)
{
    /*
     * Once IRQ delivery is confirmed, input is consumed purely from the RX
     * ring buffer that the ISR fills.
     */
    if (g_uart_async_ready && local_irqs_enabled()) {
        char c;

        while (1) {
            if (uart_rx_pop_char(&c) == 0) {
                return c;
            }
        }
    }

    /*
     * Syscalls enter with SIE clear. Consume buffered input first, then poll
     * hardware instead of waiting for an RX bottom half that cannot run.
     */
    {
        char c;
        if (uart_rx_pop_char(&c) == 0) {
            return c;
        }
    }

    /*
     * Bring-up fallback: briefly wait for an RX IRQ before polling the UART
     * directly. This keeps the shell usable even if the interrupt route is
     * not live yet during early boot.
     */
    if (g_uart_irq_enabled && local_irqs_enabled()) {
        unsigned long wait = 500000UL;
        char c;

        while (wait-- > 0UL) {
            if (uart_rx_pop_char(&c) == 0) {
                return c;
            }
            asm volatile("nop");
        }
    }

    while ((uart_reg_read(UART_LSR_REG) & LSR_RX_READY) == 0U) {
    }
    g_uart_rx_fallback_count++;
    return (char)(uart_reg_read(UART_RBR_REG) & 0xFFU);
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
    while (budget > 0U && (uart_reg_read(UART_LSR_REG) & LSR_RX_READY) != 0U) {
        char c = (char)(uart_reg_read(UART_RBR_REG) & 0xFFU);
        (void)uart_rx_push_char(c);
        g_uart_rx_irq_count++;
        budget--;
    }
}

void uart_irq_top(void)
{
    uint32_t status;

    status = uart_irq_status_snapshot();
    if (!g_uart_async_ready) {
        g_uart_async_ready = 1;
    }

    if (uart_rx_ready_from_status(status)) {
        trace_record(TRACE_UART_TOP_RX, 0U);
        uart_rx_mask();
        /* Preserve a bounded batch before deferring the remaining RX work. */
        uart_rx_drain(UART_DEFERRED_WORK_BUDGET);
        (void)deferred_work_enqueue(&g_uart_rx_work);
    }

    if (uart_tx_ready_from_status(status)) {
        trace_record(TRACE_UART_TOP_TX, (uint32_t)g_uart_tx_buf.count);
        uart_tx_mask();
        (void)deferred_work_enqueue(&g_uart_tx_work);
    }
}

static int uart_rx_work_run(struct deferred_work *task)
{
    unsigned int budget = UART_DEFERRED_WORK_BUDGET;

    (void)task;
    trace_record(TRACE_UART_RX_BOTTOM, budget);

    uart_rx_drain(budget);

    if ((uart_reg_read(UART_LSR_REG) & LSR_RX_READY) != 0U) {
        return 1;
    }

    uart_rx_unmask();
    return 0;
}

static int uart_tx_work_run(struct deferred_work *task)
{
    unsigned int budget = UART_DEFERRED_WORK_BUDGET;

    (void)task;
    trace_record(TRACE_UART_TX_BOTTOM, g_uart_tx_buf.count);
    if (uart_tx_buf_is_empty() && g_uart_tx_refill != 0) {
        g_uart_tx_refill();
    }
    while (budget > 0U && uart_tx_try_drain_one()) {
        g_uart_tx_irq_count++;
        budget--;
    }
    if (uart_tx_buf_is_empty() && g_uart_tx_refill != 0) {
        g_uart_tx_refill();
    }
    if (!uart_tx_buf_is_empty()) {
        uart_tx_unmask();
        /* Wait for TX-ready rather than spinning the runner on a busy device. */
        return (uart_reg_read(UART_LSR_REG) & LSR_TX_IDLE) != 0;
    }
    uart_tx_mask();
    return 0;
}

void uart_note_external_irq(uint32_t irq)
{
    if (irq == uart_irq_id()) {
        g_uart_ext_irq_count++;
    }
}

void uart_reset_stats(void)
{
    uint64_t sstatus;

    sstatus = local_irq_save();
    g_uart_ext_irq_count = 0;
    g_uart_rx_irq_count = 0;
    g_uart_tx_irq_count = 0;
    g_uart_rx_fallback_count = 0;
    g_uart_tx_poll_count = 0;
    local_irq_restore(sstatus);
}

void uart_get_stats(struct uart_stats *stats)
{
    if (stats == 0) {
        return;
    }

    stats->async_ready = (unsigned long)g_uart_async_ready;
    stats->ext_irq_count = g_uart_ext_irq_count;
    stats->rx_irq_count = g_uart_rx_irq_count;
    stats->tx_irq_count = g_uart_tx_irq_count;
    stats->rx_fallback_count = g_uart_rx_fallback_count;
    stats->tx_poll_count = g_uart_tx_poll_count;
}

void uart_set_platform(uintptr_t base, uint32_t irq)
{
    g_uart_mmio_base = base;
    g_uart_irq_id = irq;
}

/* Queue the whole buffer or return -1. Never wait for room in the TX ring. */
int uart_write_async(const char *buf, unsigned int size)
{
    uint64_t status = local_irq_save();

    if (buf == 0 || !g_uart_irq_enabled || size > RINGBUF_CAPACITY - g_uart_tx_buf.count) {
        local_irq_restore(status);
        return -1;
    }
    for (unsigned int i = 0; i < size; i++) {
        ringbuf_push(&g_uart_tx_buf, buf[i]);
    }
    if (size != 0) {
        uart_tx_kick();
    }
    local_irq_restore(status);
    return 0;
}

/* One streaming producer may refill an empty TX ring from the bottom half. */
int uart_tx_set_refill(void (*refill)(void))
{
    uint64_t status = local_irq_save();

    if (refill != 0 && (!g_uart_irq_enabled ||
        (g_uart_tx_refill != 0 && g_uart_tx_refill != refill))) {
        local_irq_restore(status);
        return -1;
    }
    g_uart_tx_refill = refill;
    local_irq_restore(status);
    return 0;
}

#ifdef KERNEL_TEST
void uart_test_async(int enabled)
{
    g_uart_async_ready = enabled;
    g_uart_irq_enabled = enabled;
}

unsigned int uart_test_tx_count(void)
{
    return g_uart_tx_buf.count;
}
#endif
