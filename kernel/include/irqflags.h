#ifndef IRQFLAGS_H
#define IRQFLAGS_H

#include <stdint.h>
#include "csr.h"

/* Local hart only. These helpers do not provide an SMP lock. */
static inline uint64_t local_irq_save(void)
{
    uint64_t status;

    asm volatile("csrrc %0, sstatus, %1"
                 : "=r"(status) : "r"(SSTATUS_SIE) : "memory");
    return status;
}

/* Restore the saved status, including an already-disabled SIE. */
static inline void local_irq_restore(uint64_t status)
{
    asm volatile("csrw sstatus, %0" : : "r"(status) : "memory");
}

static inline void local_irq_enable(void)
{
    asm volatile("csrs sstatus, %0" : : "r"(SSTATUS_SIE) : "memory");
}

static inline void local_irq_disable(void)
{
    asm volatile("csrc sstatus, %0" : : "r"(SSTATUS_SIE) : "memory");
}

static inline int local_irqs_enabled(void)
{
    uint64_t status;

    asm volatile("csrr %0, sstatus" : "=r"(status));
    return (status & SSTATUS_SIE) != 0;
}

#endif
