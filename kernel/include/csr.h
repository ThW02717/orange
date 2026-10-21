#ifndef CSR_H
#define CSR_H

/* sret restores privilege from SPP and interrupt enable from SPIE. */
#define SSTATUS_SIE          (1 << 1)
#define SSTATUS_SPIE         (1 << 5)
#define SSTATUS_SPP          (1 << 8)
#define SIE_STIE             (1 << 5)
#define SIE_SEIE             (1 << 9)

#ifndef __ASSEMBLER__
#define SCAUSE_INTERRUPT_BIT (1ULL << 63)
#define SCAUSE_INST_FAULT    1UL
#define SCAUSE_ILLEGAL_INST  2UL
#define SCAUSE_LOAD_FAULT    5UL
#define SCAUSE_STORE_FAULT   7UL
#define SCAUSE_U_ECALL       8UL
#define SCAUSE_S_TIMER_INT   5UL
#define SCAUSE_S_EXT_INT     9UL
#endif

#endif
