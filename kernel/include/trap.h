#ifndef TRAP_H
#define TRAP_H

#include <stdint.h>

/* sstatus bits used by the minimal S-mode <-> U-mode path.
 * SPP  : privilege level restored by sret (0 = U-mode, 1 = S-mode)
 * SPIE : interrupt-enable state restored by sret
 * SIE  : current supervisor interrupt-enable bit
 */
#define SSTATUS_SIE            (1UL << 1)
#define SSTATUS_SPIE           (1UL << 5)
#define SSTATUS_SPP            (1UL << 8)

/* sie selects which supervisor interrupt sources are individually enabled.
 * STIE specifically enables the supervisor timer interrupt source.
 */
#define SIE_STIE               (1UL << 5)

/* scause encoding: top bit distinguishes interrupt from exception. */
#define SCAUSE_INTERRUPT_BIT   (1UL << 63)

/* Minimal exception causes used by Basic Exercise 1. */
#define SCAUSE_U_ECALL         8UL
#define SCAUSE_ILLEGAL_INST    2UL
#define SCAUSE_INST_FAULT      1UL
#define SCAUSE_LOAD_FAULT      5UL
#define SCAUSE_STORE_FAULT     7UL

/* Supervisor timer interrupt cause code used by Exercise 2. */
#define SCAUSE_S_TIMER_INT     5UL

/* Minimal syscall ids carried in a7 across the ecall boundary. */
#define SYS_test               0UL
#define SYS_exit               1UL

/* User memory layout.
 * Keep board addresses aligned with the OrangePi RV2 plan.
 * QEMU uses a different RAM map, so the test layout must live in QEMU RAM.
 */
#ifdef QEMU
#define USER_CODE_BASE         0x83000000UL
#define USER_CODE_SIZE         0x00100000UL
#define USER_STACK_SIZE        0x00010000UL
#define USER_STACK_TOP         0x83200000UL
#else
#define USER_CODE_BASE         0x32000000UL
#define USER_CODE_SIZE         0x00100000UL
#define USER_STACK_SIZE        0x00010000UL
#define USER_STACK_TOP         0x32200000UL
#endif

#define USER_KSTACK_SIZE       0x00010000UL

#define USER_CODE_LIMIT        (USER_CODE_BASE + USER_CODE_SIZE)
#define USER_STACK_BASE        (USER_STACK_TOP - USER_STACK_SIZE)

/* Fixed trap save area shared by trap_entry.S and trap.c.
 * General registers capture the interrupted CPU context.
 * sepc    : PC of the trapped instruction / return target for sret
 * sstatus : privilege and interrupt state restored by sret
 * scause  : trap reason reported by hardware
 * stval   : extra fault information supplied by hardware
 */
struct trapframe {
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

#define TF_RA                0UL
#define TF_SP                8UL
#define TF_GP                16UL
#define TF_TP                24UL
#define TF_T0                32UL
#define TF_T1                40UL
#define TF_T2                48UL
#define TF_S0                56UL
#define TF_S1                64UL
#define TF_A0                72UL
#define TF_A1                80UL
#define TF_A2                88UL
#define TF_A3                96UL
#define TF_A4                104UL
#define TF_A5                112UL
#define TF_A6                120UL
#define TF_A7                128UL
#define TF_S2                136UL
#define TF_S3                144UL
#define TF_S4                152UL
#define TF_S5                160UL
#define TF_S6                168UL
#define TF_S7                176UL
#define TF_S8                184UL
#define TF_S9                192UL
#define TF_S10               200UL
#define TF_S11               208UL
#define TF_T3                216UL
#define TF_T4                224UL
#define TF_T5                232UL
#define TF_T6                240UL
#define TF_SEPC              248UL
#define TF_SSTATUS           256UL
#define TF_SCAUSE            264UL
#define TF_STVAL             272UL

#define TRAPFRAME_SIZE         ((uint64_t)sizeof(struct trapframe))
#define TRAPFRAME_ALLOC_SIZE   ((TRAPFRAME_SIZE + 15UL) & ~15UL)

extern void trap_entry(void);
extern void trap_return(struct trapframe *tf);

void trap_init(void);
void do_trap(struct trapframe *tf);
void handle_user_ecall(struct trapframe *tf);
void handle_user_fault(struct trapframe *tf);

static inline uint64_t trap_scause_code(uint64_t scause)
{
    return scause & ~SCAUSE_INTERRUPT_BIT;
}

static inline int trap_is_interrupt(uint64_t scause)
{
    return (scause & SCAUSE_INTERRUPT_BIT) != 0;
}

#endif
