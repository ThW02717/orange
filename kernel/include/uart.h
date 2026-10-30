#ifndef UART_H
#define UART_H

#include <stdint.h>

struct uart_stats {
    unsigned long irq_enabled;
    unsigned long ext_irq_count;
    unsigned long rx_irq_count;
    unsigned long tx_irq_count;
    unsigned long rx_fallback_count;
    unsigned long tx_poll_count;
};

void uart_set_platform(uintptr_t base, uint32_t irq);
uint32_t uart_irq_id(void);

void uart_send(char c);
char uart_recv(void);
void uart_send_string(const char *str);
void uart_send_hex(unsigned long value);
void uart_send_dec(unsigned long value);
void uart_tx_wait_idle(void);


/* Bring up the UART0 -> PLIC -> S-mode external-interrupt path. */
void uart_irq_init(void);
void uart_irq_top(void);
void uart_rx_run_bottom_half(void);
void uart_tx_run_bottom_half(void);
void uart_reset_stats(void);
void uart_get_stats(struct uart_stats *stats);

/* Nonblocking TX; refill runs on the bottom-half stack and must not sleep. */
int uart_write_async(const char *buf, unsigned int size);
int uart_tx_set_refill(void (*refill)(void));

#define KEY_ENTER 13
#define KEY_BACKSPACE 127
#define KEY_ESC 27

#endif
