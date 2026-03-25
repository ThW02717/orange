#ifndef UART_H
#define UART_H

#include <stdint.h>

#ifdef QEMU
#define UART_BASE 0x10000000UL
#define UART_REG(off) (UART_BASE + (off))
#define UART_THR_REG UART_REG(0x00)
#define UART_RBR_REG UART_REG(0x00)
#define UART_DLL_REG UART_REG(0x00)
#define UART_IER_REG UART_REG(0x01)
#define UART_DLH_REG UART_REG(0x01)
#define UART_IIR_REG UART_REG(0x02)
#define UART_FCR_REG UART_REG(0x02)
#define UART_LCR_REG UART_REG(0x03)
#define UART_MCR_REG UART_REG(0x04)
#define UART_LSR_REG UART_REG(0x05)
#define UART_MSR_REG UART_REG(0x06)
#else
#define UART_BASE 0xD4017000UL
#define UART_REG(off) (UART_BASE + (off))
#define UART_THR_REG UART_REG(0x00)
#define UART_RBR_REG UART_REG(0x00)
#define UART_DLL_REG UART_REG(0x00)
#define UART_IER_REG UART_REG(0x04)
#define UART_DLH_REG UART_REG(0x04)
#define UART_IIR_REG UART_REG(0x08)
#define UART_FCR_REG UART_REG(0x08)
#define UART_LCR_REG UART_REG(0x0C)
#define UART_MCR_REG UART_REG(0x10)
#define UART_LSR_REG UART_REG(0x14)
#define UART_MSR_REG UART_REG(0x18)
#endif

#define LSR_RX_READY 0x01U
#define LSR_TX_IDLE  0x20U

#define UART_IER_RX_ENABLE   0x01U
#define UART_IER_TX_ENABLE   0x02U
#define UART_IER_LINE_ENABLE 0x04U
#define UART_IER_RTO_ENABLE  0x10U
#define UART_IER_UUE         0x40U

#define UART_MCR_OUT2        0x08U

/* OrangePi RV2 UART0 interrupt wiring used by Basic Exercise 3.
 * Keep the platform constants close to the UART interface first; the later
 * PLIC helper can include this header instead of duplicating board numbers.
 */
#ifndef QEMU
#define UART0_BASE      UART_BASE
#define UART0_IRQ       42UL
#define PLIC_BASE       0xE0000000UL
#endif

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

void uart_irq_init(void);
void uart_irq_stop(void);
void uart_handle_irq(void);
void uart_note_external_irq(uint32_t irq);
void uart_reset_stats(void);
unsigned long uart_get_ext_irq_count(void);
unsigned long uart_get_rx_irq_count(void);
unsigned long uart_get_tx_irq_count(void);
unsigned long uart_get_rx_fallback_count(void);
unsigned long uart_get_tx_poll_count(void);
int uart_async_ready(void);

#define KEY_ENTER 13
#define KEY_BACKSPACE 127
#define KEY_ESC 27

#endif
