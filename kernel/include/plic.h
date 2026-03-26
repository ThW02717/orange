#ifndef PLIC_H
#define PLIC_H

#include <stdint.h>
#include "uart.h"

/* Minimal PLIC layout for OrangePi RV2 bring-up.
 * The UART path currently runs on hart0 in S-mode, so this helper only
 * targets hart0's supervisor interrupt context. In other words, the
 * "receiver" we configure here is hart0 S-mode rather than an IRQ source.
 *
 * PLIC delivery has a few distinct steps:
 * 1. Give an interrupt source a non-zero priority.
 * 2. Enable that source for a specific hart/context.
 * 3. Set a context threshold low enough that the source can pass through.
 * 4. When an external interrupt trap arrives, claim the active IRQ ID.
 * 5. After the device ISR finishes, complete that same IRQ ID.
 *
 * After these registers are configured, PLIC hardware itself performs the
 * forwarding. Kernel code mainly fills in the policy knobs and later uses
 * claim/complete during interrupt handling.
 */
#ifndef QEMU
#define PLIC_HART0_SCTX            1UL
/* Per-source priority registers live here. Source priority 0 means the IRQ
 * will never be forwarded by the PLIC.
 */
#define PLIC_PRIORITY_OFFSET       0x0000UL
/* Per-context enable bitmaps live here. Each context gets its own enable
 * window that decides which IRQ sources may be delivered to that context.
 */
#define PLIC_ENABLE_OFFSET         0x2000UL
#define PLIC_ENABLE_STRIDE         0x80UL
/* Per-context threshold plus claim/complete registers live here. "Context"
 * means an interrupt receiver such as hart0 S-mode. Threshold filters
 * low-priority IRQs for that receiver; claim/complete is the runtime
 * handshake used once an external interrupt trap has already arrived.
 */
#define PLIC_CONTEXT_OFFSET        0x200000UL
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
