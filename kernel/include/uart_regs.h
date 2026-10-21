#ifndef UART_REGS_H
#define UART_REGS_H

#include <stdint.h>

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


#endif
