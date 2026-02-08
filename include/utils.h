#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>

static inline uint32_t get32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void put32(uintptr_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}

static inline void delay(unsigned int cnt)
{
    while (cnt--) {
        asm volatile("nop");
    }
}

#endif
