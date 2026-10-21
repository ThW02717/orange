#ifndef TRAP_H
#define TRAP_H

#include <stdint.h>
#include "syscall.h"

#include "csr.h"
#include "context_offsets.h"

/* Fixed trap context layout shared by trap_entry.S and the C handlers.
 * General registers capture the interrupted CPU context.
 * sepc    : PC of the trapped instruction / return target for sret
 * sstatus : privilege and interrupt state restored by sret
 * scause  : trap reason reported by hardware
 * stval   : extra fault information supplied by hardware
 */
struct trap_context {
    uint64_t ra;
    uint64_t sp;
    uint64_t gp;
    uint64_t tp;
    uint64_t t0;
    uint64_t t1;
    uint64_t t2;
    uint64_t s0;
    uint64_t s1;
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t a3;
    uint64_t a4;
    uint64_t a5;
    uint64_t a6;
    uint64_t a7;
    uint64_t s2;
    uint64_t s3;
    uint64_t s4;
    uint64_t s5;
    uint64_t s6;
    uint64_t s7;
    uint64_t s8;
    uint64_t s9;
    uint64_t s10;
    uint64_t s11;
    uint64_t t3;
    uint64_t t4;
    uint64_t t5;
    uint64_t t6;
    uint64_t sepc;
    uint64_t sstatus;
    uint64_t scause;
    uint64_t stval;
};

extern void trap_entry(void);
extern void trap_return(struct trap_context *tc);

void trap_init(void);
int trap_trace_ecalls(int enabled);
void trap_set_ecall_demo(int32_t tid);
void trap_dispatch(struct trap_context *tc);
void handle_user_fault(struct trap_context *tc);

static inline uint64_t trap_scause_code(uint64_t scause)
{
    return scause & ~SCAUSE_INTERRUPT_BIT;
}

static inline int trap_is_interrupt(uint64_t scause)
{
    return (scause & SCAUSE_INTERRUPT_BIT) != 0;
}

#endif
