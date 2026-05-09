/*
 * uart.c - 16550-compatible UART driver (polling mode).
 *
 * All I/O is done by busy-wait polling — no interrupts.
 * The bootloader never enables UART interrupts; it just
 * spins on the line-status register (LSR) until the hardware
 * is ready to send/receive a byte.
 */

#include "peripherals/mini_uart.h"
#include "utils.h"

/* read a 32-bit UART register */
static inline uint32_t uart_reg_read(uintptr_t reg)
{
#ifdef QEMU
    return (uint32_t)(*(volatile uint8_t *)reg);
#else
    return get32(reg);
#endif
}

/* write a 32-bit value to a UART register */
static inline void uart_reg_write(uintptr_t reg, uint32_t value)
{
#ifdef QEMU
    *(volatile uint8_t *)reg = (uint8_t)value;
#else
    put32(reg, value);
#endif
}

/*
 * Drain the RX FIFO: read-and-discard any stale bytes left
 * by a previous kernel session or cable noise.
 */
static void uart_flush_hardware_rx_fifo(void)
{
    unsigned int limit = 256U;

    while (limit-- > 0U && (uart_reg_read(UART_LSR_REG) & LSR_RX_READY) != 0U) {
        (void)uart_reg_read(UART_RBR_REG);
    }
}

/*
 * uart_init: prepare the UART for polling-based communication.
 *
 * On real hardware the baud rate was already configured by U-Boot,
 * so we skip re-configuration and only:
 *   1. Disable interrupts   (IER = 0)
 *   2. Clear TX/RX FIFOs    (FCR = 0x07)
 *   3. Flush any leftover data from the RX FIFO
 */
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
    uart_reg_write(UART_IER_REG, 0x00);
    uart_reg_write(UART_FCR_REG, 0x07);
    uart_flush_hardware_rx_fifo();
#endif
}

/*
 * uart_send: send one byte (blocking until TX FIFO has space).
 */
void uart_send(char c)
{
    while ((uart_reg_read(UART_LSR_REG) & LSR_TX_IDLE) == 0U) {
        /* spin — wait for TX holding register to empty */
    }
    uart_reg_write(UART_THR_REG, (uint32_t)(uint8_t)c);
}

/*
 * uart_recv: receive one byte (blocking until data is ready).
 */
char uart_recv(void)
{
    while ((uart_reg_read(UART_LSR_REG) & LSR_RX_READY) == 0U) {
        /* spin — wait for a byte to arrive */
    }
    return (char)(uart_reg_read(UART_RBR_REG) & 0xFFU);
}

/*
 * uart_send_string: send a null-terminated string.
 * Converts '\n' to "\r\n" for terminal compatibility.
 */
void uart_send_string(const char *str)
{
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '\n') {
            uart_send('\r');
        }
        uart_send(str[i]);
    }
}

/* uart_send_hex: print an unsigned long in hexadecimal */
void uart_send_hex(unsigned long value)
{
    for (int shift = (int)(sizeof(unsigned long) * 8U) - 4; shift >= 0; shift -= 4) {
        unsigned long digit = (value >> shift) & 0xFUL;
        char c = (digit < 10UL) ? (char)('0' + digit) : (char)('a' + (digit - 10UL));
        uart_send(c);
    }
}

/* uart_send_dec: print an unsigned long in decimal */
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

/* Compatibility wrappers */
void uart_putc(char c) { uart_send(c); }
void uart_puts(const char *s) { uart_send_string(s); }
char uart_getc(void) { return uart_recv(); }
void uart_hex(unsigned long value) { uart_send_hex(value); }
void uart_dec(unsigned long value) { uart_send_dec(value); }
