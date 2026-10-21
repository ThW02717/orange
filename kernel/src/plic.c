#include "plic.h"
#include "utils.h"

static uintptr_t g_plic_base;

/* Called during platform discovery, before external interrupts are enabled. */
void plic_set_base(uintptr_t base)
{
    g_plic_base = base;
}

static inline uintptr_t plic_priority_reg(uint32_t irq)
{
    return g_plic_base + PLIC_PRIORITY_OFFSET + ((uintptr_t)irq * 4UL);
}

static inline uintptr_t plic_enable_reg(uint32_t context, uint32_t irq)
{
    return g_plic_base + PLIC_ENABLE_OFFSET +
           ((uintptr_t)context * PLIC_ENABLE_STRIDE) +
           (((uintptr_t)irq / 32UL) * 4UL);
}

static inline uintptr_t plic_context_reg(uint32_t context, uintptr_t offset)
{
    return g_plic_base + PLIC_CONTEXT_OFFSET +
           ((uintptr_t)context * PLIC_CONTEXT_STRIDE) + offset;
}

void plic_set_priority(uint32_t irq, uint32_t priority)
{
    put32(plic_priority_reg(irq), priority);
}

void plic_enable_irq(uint32_t irq)
{
    uintptr_t reg;
    uint32_t value;
    uint32_t bit;

    /* Enable this source in hart0's supervisor context. */
    reg = plic_enable_reg(PLIC_HART0_SCTX, irq);
    value = get32(reg);
    bit = 1U << (irq % 32U);
    put32(reg, value | bit);
}

void plic_disable_irq(uint32_t irq)
{
    uintptr_t reg;
    uint32_t value;
    uint32_t bit;

    /* Mask this source from hart0's supervisor context. */
    reg = plic_enable_reg(PLIC_HART0_SCTX, irq);
    value = get32(reg);
    bit = 1U << (irq % 32U);
    put32(reg, value & ~bit);
}

void plic_set_threshold(uint32_t threshold)
{
    put32(plic_context_reg(PLIC_HART0_SCTX, PLIC_THRESHOLD_OFFSET), threshold);
}

uint32_t plic_claim(void)
{
    /* Claim one pending source; zero means none. */
    return get32(plic_context_reg(PLIC_HART0_SCTX, PLIC_CLAIM_COMPLETE_OFFSET));
}

void plic_complete(uint32_t irq)
{
    /* Allow this source to be delivered again after its handler finishes. */
    put32(plic_context_reg(PLIC_HART0_SCTX, PLIC_CLAIM_COMPLETE_OFFSET), irq);
}
