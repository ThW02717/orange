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

/* Pair with save() while IRQs remain disabled. Restore only SIE; keep any
 * changes to the other status bits made inside the critical section.
 */
static inline void local_irq_restore(uint64_t status)
{
    asm volatile("csrs sstatus, %0"
                 : : "r"(status & SSTATUS_SIE) : "memory");
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

    asm volatile("csrr %0, sstatus" : "=r"(status) : : "memory");
    return (status & SSTATUS_SIE) != 0;
}

#endif
