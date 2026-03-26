#include "plic.h"
#include "utils.h"

#ifndef QEMU
static inline uintptr_t plic_priority_reg(uint32_t irq)
{
    return plic_mmio_base() + PLIC_PRIORITY_OFFSET + ((uintptr_t)irq * 4UL);
}


static inline uintptr_t plic_enable_reg(uint32_t context, uint32_t irq)
{
    return plic_mmio_base() + PLIC_ENABLE_OFFSET +
           ((uintptr_t)context * PLIC_ENABLE_STRIDE) +
           (((uintptr_t)irq / 32UL) * 4UL);
}


static inline uintptr_t plic_context_reg(uint32_t context, uintptr_t offset)
{
    return plic_mmio_base() + PLIC_CONTEXT_OFFSET +
           ((uintptr_t)context * PLIC_CONTEXT_STRIDE) + offset;
}
#endif

void plic_set_priority(uint32_t irq, uint32_t priority)
{
#ifndef QEMU
    put32(plic_priority_reg(irq), priority);
#else
    (void)irq;
    (void)priority;
#endif
}

void plic_enable_irq(uint32_t irq)
{
#ifndef QEMU
    uintptr_t reg;
    uint32_t value;
    uint32_t bit;

    /* Step 2: enable this source for hart0's supervisor context. */
    reg = plic_enable_reg(PLIC_HART0_SCTX, irq);
    value = get32(reg);
    bit = 1U << (irq % 32U);
    put32(reg, value | bit);
#else
    (void)irq;
#endif
}

void plic_disable_irq(uint32_t irq)
{
#ifndef QEMU
    uintptr_t reg;
    uint32_t value;
    uint32_t bit;

    /* Mask this source from hart0's supervisor context. */
    reg = plic_enable_reg(PLIC_HART0_SCTX, irq);
    value = get32(reg);
    bit = 1U << (irq % 32U);
    put32(reg, value & ~bit);
#else
    (void)irq;
#endif
}

void plic_set_threshold(uint32_t threshold)
{
#ifndef QEMU
    put32(plic_context_reg(PLIC_HART0_SCTX, PLIC_THRESHOLD_OFFSET), threshold);
#else
    (void)threshold;
#endif
}

uint32_t plic_claim(void)
{
#ifndef QEMU
    /* Step 4: ask the PLIC which IRQ ID is currently pending for this
     * context. A return value of 0 means "no interrupt to service".
     */
    return get32(plic_context_reg(PLIC_HART0_SCTX, PLIC_CLAIM_COMPLETE_OFFSET));
#else
    return 0;
#endif
}

void plic_complete(uint32_t irq)
{
#ifndef QEMU
    /* Step 5: tell the PLIC that servicing of this IRQ ID is finished, so
     * later interrupts from the same source may be delivered again.
     */
    put32(plic_context_reg(PLIC_HART0_SCTX, PLIC_CLAIM_COMPLETE_OFFSET), irq);
#else
    (void)irq;
#endif
}
