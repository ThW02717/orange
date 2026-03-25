#include "uart.h"
#include "ringbuf.h"
#include "trap.h"
#include "plic.h"
#include "utils.h"

static struct ringbuf g_uart_rx_buf;
static struct ringbuf g_uart_tx_buf;
static int g_uart_irq_enabled = 0;
static int g_uart_async_ready = 0;
static volatile unsigned long g_uart_ext_irq_count = 0;
static volatile unsigned long g_uart_rx_irq_count = 0;
static volatile unsigned long g_uart_tx_irq_count = 0;
static volatile unsigned long g_uart_rx_fallback_count = 0;
static volatile unsigned long g_uart_tx_poll_count = 0;

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

static inline uint64_t uart_irq_save(void)
{
    uint64_t sstatus;

    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    asm volatile("csrc sstatus, %0" : : "r"(SSTATUS_SIE));
    return sstatus;
}

static inline void uart_irq_restore(uint64_t sstatus)
{
    asm volatile("csrw sstatus, %0" : : "r"(sstatus));
}

static void uart_ier_set_bits(uint32_t bits)
{
    uart_reg_write(UART_IER_REG, uart_reg_read(UART_IER_REG) | bits);
}

static void uart_ier_clear_bits(uint32_t bits)
{
    uart_reg_write(UART_IER_REG, uart_reg_read(UART_IER_REG) & ~bits);
}

/* RX/TX ring buffers are shared between interrupt context and normal shell
 * code. Keep every head/tail/count update inside a tiny local critical
 * section so concurrent push/pop pairs cannot corrupt the FIFO state.
 */
static int uart_rx_push_char(char c)
{
    uint64_t sstatus;
    int ret;

    sstatus = uart_irq_save();
    ret = ringbuf_push(&g_uart_rx_buf, c);
    uart_irq_restore(sstatus);
    return ret;
}

static int uart_rx_pop_char(char *out)
{
    uint64_t sstatus;
    int ret;

    sstatus = uart_irq_save();
    ret = ringbuf_pop(&g_uart_rx_buf, out);
    uart_irq_restore(sstatus);
    return ret;
}

static int uart_tx_push_char(char c)
{
    uint64_t sstatus;
    int ret;

    sstatus = uart_irq_save();
    ret = ringbuf_push(&g_uart_tx_buf, c);
    uart_irq_restore(sstatus);
    return ret;
}

static int uart_tx_pop_char(char *out)
{
    uint64_t sstatus;
    int ret;

    sstatus = uart_irq_save();
    ret = ringbuf_pop(&g_uart_tx_buf, out);
    uart_irq_restore(sstatus);
    return ret;
}

static int uart_tx_buf_is_full(void)
{
    uint64_t sstatus;
    int ret;

    sstatus = uart_irq_save();
    ret = ringbuf_is_full(&g_uart_tx_buf);
    uart_irq_restore(sstatus);
    return ret;
}

static int uart_tx_buf_is_empty(void)
{
    uint64_t sstatus;
    int ret;

    sstatus = uart_irq_save();
    ret = ringbuf_is_empty(&g_uart_tx_buf);
    uart_irq_restore(sstatus);
    return ret;
}

static int uart_tx_try_drain_one(void)
{
    char c;

    if ((uart_reg_read(UART_LSR_REG) & LSR_TX_IDLE) == 0U) {
        return 0;
    }

    if (uart_tx_pop_char(&c) != 0) {
        return 0;
    }

    uart_reg_write(UART_THR_REG, (uint32_t)(uint8_t)c);
    return 1;
}

static void uart_tx_kick(void)
{
    uart_ier_set_bits(UART_IER_TX_ENABLE);
    (void)uart_tx_try_drain_one();
}

static void uart_irq_ack_stale_status(void)
{
    (void)uart_reg_read(UART_LSR_REG);
    (void)uart_reg_read(UART_RBR_REG);
    (void)uart_reg_read(UART_IIR_REG);
    (void)uart_reg_read(UART_MSR_REG);
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
    /* Keep bootloader UART settings for board bring-up stability. */
#endif
}

void uart_irq_init(void)
{
#ifdef QEMU
    return;
#else
    uint64_t sie;
    uint64_t sstatus;

    ringbuf_init(&g_uart_rx_buf);
    ringbuf_init(&g_uart_tx_buf);
    uart_reset_stats();

    plic_set_priority(UART0_IRQ, 1U);
    plic_enable_irq(UART0_IRQ);
    plic_set_threshold(0U);

    /* This UART block is DT-compatible with pxa-uart. Besides the basic RX
     * interrupt bit, it also wants unit-enable plus receiver-timeout enable
     * so incoming characters can raise an interrupt promptly while the shell
     * is idle.
     */
    uart_reg_write(UART_FCR_REG, 0x07U);
    uart_reg_write(UART_MCR_REG, uart_reg_read(UART_MCR_REG) | UART_MCR_OUT2);
    uart_irq_ack_stale_status();
    uart_ier_set_bits(UART_IER_UUE |
                      UART_IER_RX_ENABLE |
                      UART_IER_LINE_ENABLE |
                      UART_IER_RTO_ENABLE);
    uart_ier_clear_bits(UART_IER_TX_ENABLE);
    uart_irq_ack_stale_status();

    asm volatile("csrr %0, sie" : "=r"(sie));
    sie |= SIE_SEIE;
    asm volatile("csrw sie, %0" : : "r"(sie));

    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    sstatus |= SSTATUS_SIE;
    asm volatile("csrw sstatus, %0" : : "r"(sstatus));

    g_uart_irq_enabled = 1;
    g_uart_async_ready = 0;
#endif
}

void uart_irq_stop(void)
{
#ifdef QEMU
    return;
#else
    uint64_t sie;

    g_uart_async_ready = 0;
    g_uart_irq_enabled = 0;
    uart_reg_write(UART_IER_REG, 0U);
    plic_disable_irq(UART0_IRQ);

    asm volatile("csrr %0, sie" : "=r"(sie));
    sie &= ~SIE_SEIE;
    asm volatile("csrw sie, %0" : : "r"(sie));
#endif
}

void uart_send(char c)
{
    if (g_uart_async_ready) {
        while (1) {
            if (!uart_tx_buf_is_full()) {
                (void)uart_tx_push_char(c);
                uart_tx_kick();
                return;
            }
            (void)uart_tx_try_drain_one();
        }
    }

    g_uart_tx_poll_count++;
    while ((uart_reg_read(UART_LSR_REG) & LSR_TX_IDLE) == 0U) {
    }
    uart_reg_write(UART_THR_REG, (uint32_t)(uint8_t)c);
}

char uart_recv(void)
{
    if (g_uart_async_ready) {
        char c;

        while (1) {
            if (uart_rx_pop_char(&c) == 0) {
                return c;
            }
        }
    }

    if (g_uart_irq_enabled) {
        unsigned long wait = 500000UL;
        char c;

        while (wait-- > 0UL) {
            if (uart_rx_pop_char(&c) == 0) {
                return c;
            }
            asm volatile("nop");
        }
    }

    g_uart_rx_fallback_count++;
    while ((uart_reg_read(UART_LSR_REG) & LSR_RX_READY) == 0U) {
    }
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

void uart_handle_irq(void)
{
    while ((uart_reg_read(UART_LSR_REG) & LSR_RX_READY) != 0U) {
        char c = (char)(uart_reg_read(UART_RBR_REG) & 0xFFU);
        (void)uart_rx_push_char(c);
        g_uart_rx_irq_count++;
    }

    while (uart_tx_try_drain_one()) {
        g_uart_tx_irq_count++;
    }

    if ((g_uart_rx_irq_count != 0UL || g_uart_tx_irq_count != 0UL) && !g_uart_async_ready) {
        g_uart_async_ready = 1;
    }

    if (uart_tx_buf_is_empty()) {
        uart_ier_clear_bits(UART_IER_TX_ENABLE);
    }
}

void uart_note_external_irq(uint32_t irq)
{
    if (irq == UART0_IRQ) {
        g_uart_ext_irq_count++;
    }
}

void uart_reset_stats(void)
{
    uint64_t sstatus;

    sstatus = uart_irq_save();
    g_uart_ext_irq_count = 0;
    g_uart_rx_irq_count = 0;
    g_uart_tx_irq_count = 0;
    g_uart_rx_fallback_count = 0;
    g_uart_tx_poll_count = 0;
    uart_irq_restore(sstatus);
}

unsigned long uart_get_ext_irq_count(void)
{
    return g_uart_ext_irq_count;
}

unsigned long uart_get_rx_irq_count(void)
{
    return g_uart_rx_irq_count;
}

unsigned long uart_get_tx_irq_count(void)
{
    return g_uart_tx_irq_count;
}

unsigned long uart_get_rx_fallback_count(void)
{
    return g_uart_rx_fallback_count;
}

unsigned long uart_get_tx_poll_count(void)
{
    return g_uart_tx_poll_count;
}

int uart_async_ready(void)
{
    return g_uart_async_ready;
}

/* Compatibility wrappers */
void uart_putc(char c) { uart_send(c); }
void uart_puts(const char *s) { uart_send_string(s); }
char uart_getc(void) { return uart_recv(); }
void uart_hex(unsigned long value) { uart_send_hex(value); }
void uart_dec(unsigned long value) { uart_send_dec(value); }
