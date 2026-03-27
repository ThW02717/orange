#ifndef UART_H
#define UART_H

#include <stdint.h>
#include "irq_task.h"

/* UART register layout:
 * - QEMU exposes a 16550-style byte-spaced map.
 * - OrangePi RV2 uses the same logical registers but spaces them 4 bytes
 *   apart in MMIO. The MMIO base itself is discovered from DT on real
 *   hardware and stored once during boot.
 */
uintptr_t uart_mmio_base(void);

#ifdef QEMU
#define UART_REG(off) (uart_mmio_base() + (off))
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
#define UART_REG(off) (uart_mmio_base() + (off))
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

/* Line Status Register (LSR):
 * - RX_READY: at least one received byte is waiting in RBR/FIFO.
 * - TX_IDLE: the transmitter can accept another byte in THR.
 */
#define LSR_RX_READY 0x01U
#define LSR_TX_IDLE  0x20U

/* Interrupt Enable Register (IER) bits used by the interrupt-driven path. */
#define UART_IER_RX_ENABLE   0x01U
#define UART_IER_TX_ENABLE   0x02U
#define UART_IER_LINE_ENABLE 0x04U
#define UART_IER_RTO_ENABLE  0x10U
#define UART_IER_UUE         0x40U

/* Modem Control Register (MCR) bit needed to drive the IRQ output line. */
#define UART_MCR_OUT2        0x08U

struct uart_demo_stats {
    unsigned long async_ready;
    unsigned long ext_irq_count;
    unsigned long rx_irq_count;
    unsigned long tx_irq_count;
    unsigned long rx_fallback_count;
    unsigned long tx_poll_count;
};

int uart_platform_init_from_fdt(const void *fdt);
uintptr_t plic_mmio_base(void);
uint32_t uart_irq_id(void);

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

/* Bring up the UART0 -> PLIC -> S-mode external-interrupt path. */
void uart_irq_init(void);
void uart_irq_top(void);
int uart_rx_task_run(struct irq_task *task);
int uart_tx_task_run(struct irq_task *task);
void uart_rx_mask(void);
void uart_rx_unmask(void);
void uart_tx_mask(void);
void uart_tx_unmask(void);
void uart_irq_isr(void);
int uart_stress_start(uint64_t count);
uint64_t uart_stress_progress(void);
int uart_stress_active(void);
int uart_stress_note_timer(unsigned long id);
void uart_demo_note_external_irq(uint32_t irq);
void uart_demo_reset_stats(void);
void uart_demo_get_stats(struct uart_demo_stats *stats);

#define KEY_ENTER 13
#define KEY_BACKSPACE 127
#define KEY_ESC 27

#endif
