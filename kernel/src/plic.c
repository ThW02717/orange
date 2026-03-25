#include "plic.h"
#include "utils.h"

#ifndef QEMU
static inline uintptr_t plic_priority_reg(uint32_t irq)
{
    return PLIC_PRIORITY_BASE + ((uintptr_t)irq * 4UL);
}

static inline uintptr_t plic_enable_reg(uint32_t context, uint32_t irq)
{
    return PLIC_ENABLE_BASE + (context * PLIC_ENABLE_STRIDE) + (((uintptr_t)irq / 32UL) * 4UL);
}

static inline uintptr_t plic_context_reg(uint32_t context, uintptr_t offset)
{
    return PLIC_CONTEXT_BASE + (context * PLIC_CONTEXT_STRIDE) + offset;
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
    return get32(plic_context_reg(PLIC_HART0_SCTX, PLIC_CLAIM_COMPLETE_OFFSET));
#else
    return 0;
#endif
}

void plic_complete(uint32_t irq)
{
#ifndef QEMU
    put32(plic_context_reg(PLIC_HART0_SCTX, PLIC_CLAIM_COMPLETE_OFFSET), irq);
#else
    (void)irq;
#endif
}
