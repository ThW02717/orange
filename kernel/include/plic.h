#ifndef PLIC_H
#define PLIC_H

#include <stdint.h>
#include "uart.h"

/* Minimal PLIC layout for OrangePi RV2 bring-up.
 * For now we only target hart0's supervisor context because the kernel shell
 * and the upcoming UART interrupt path both run there.
 */
#ifndef QEMU
#define PLIC_HART0_SCTX            1UL
#define PLIC_PRIORITY_BASE         (PLIC_BASE + 0x0000UL)
#define PLIC_ENABLE_BASE           (PLIC_BASE + 0x2000UL)
#define PLIC_ENABLE_STRIDE         0x80UL
#define PLIC_CONTEXT_BASE          (PLIC_BASE + 0x200000UL)
#define PLIC_CONTEXT_STRIDE        0x1000UL
#define PLIC_THRESHOLD_OFFSET      0x0UL
#define PLIC_CLAIM_COMPLETE_OFFSET 0x4UL
#endif

void plic_set_priority(uint32_t irq, uint32_t priority);
void plic_enable_irq(uint32_t irq);
void plic_disable_irq(uint32_t irq);
void plic_set_threshold(uint32_t threshold);
uint32_t plic_claim(void);
void plic_complete(uint32_t irq);

#endif
