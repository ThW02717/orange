#include "peripherals/mini_uart.h"
#include "utils.h"

void uart_init(void)
{
#ifdef QEMU
    put32(UART_IER_REG, 0);
    put32(UART_LCR_REG, 0x80);
    put32(UART_DLL_REG, 0x01);
    put32(UART_DLH_REG, 0x00);
    put32(UART_LCR_REG, 0x03);
    put32(UART_FCR_REG, 0x07);
    put32(UART_MCR_REG, 0x03);
#else
    /* Keep bootloader UART settings for board bring-up stability. */
#endif
}

void uart_send(char c)
{
    while ((get32(UART_LSR_REG) & LSR_TX_IDLE) == 0U) {
    }
    put32(UART_THR_REG, (uint32_t)(uint8_t)c);
}

char uart_recv(void)
{
    while ((get32(UART_LSR_REG) & LSR_RX_READY) == 0U) {
    }
    return (char)(get32(UART_RBR_REG) & 0xFFU);
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

/* Compatibility wrappers */
void uart_putc(char c) { uart_send(c); }
void uart_puts(const char *s) { uart_send_string(s); }
char uart_getc(void) { return uart_recv(); }
void uart_hex(unsigned long value) { uart_send_hex(value); }
void uart_dec(unsigned long value) { uart_send_dec(value); }
