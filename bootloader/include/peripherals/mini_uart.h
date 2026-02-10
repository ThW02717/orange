#ifndef MINI_UART_H
#define MINI_UART_H

#include <stdint.h>

#ifdef QEMU
#define UART_BASE 0x10000000UL
#define UART_REG(off) (UART_BASE + (off))
#define UART_THR_REG UART_REG(0x00)
#define UART_RBR_REG UART_REG(0x00)
#define UART_DLL_REG UART_REG(0x00)
#define UART_IER_REG UART_REG(0x01)
#define UART_DLH_REG UART_REG(0x01)
#define UART_FCR_REG UART_REG(0x02)
#define UART_LCR_REG UART_REG(0x03)
#define UART_MCR_REG UART_REG(0x04)
#define UART_LSR_REG UART_REG(0x05)
#else
#define UART_BASE 0xD4017000UL
#define UART_REG(off) (UART_BASE + (off))
#define UART_THR_REG UART_REG(0x00)
#define UART_RBR_REG UART_REG(0x00)
#define UART_DLL_REG UART_REG(0x00)
#define UART_IER_REG UART_REG(0x04)
#define UART_DLH_REG UART_REG(0x04)
#define UART_FCR_REG UART_REG(0x08)
#define UART_LCR_REG UART_REG(0x0C)
#define UART_MCR_REG UART_REG(0x10)
#define UART_LSR_REG UART_REG(0x14)
#endif

#define LSR_RX_READY 0x01U
#define LSR_TX_IDLE  0x20U

void uart_init(void);
void uart_send(char c);
char uart_recv(void);
void uart_send_string(const char *str);
void uart_send_hex(unsigned long value);
void uart_send_dec(unsigned long value);

/* Compatibility wrappers for existing code. */
void uart_putc(char c);
void uart_puts(const char *s);
char uart_getc(void);
void uart_hex(unsigned long value);
void uart_dec(unsigned long value);

#define KEY_ENTER 13
#define KEY_BACKSPACE 127
#define KEY_ESC 27

#endif
