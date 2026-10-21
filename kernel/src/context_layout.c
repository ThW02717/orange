#include <stddef.h>
#include "thread.h"
#include "trap.h"

/* Fail the build if a C layout stops matching its assembly offsets. */
_Static_assert(offsetof(struct trap_context, ra) == TC_RA, "TC_RA");
_Static_assert(offsetof(struct trap_context, sp) == TC_SP, "TC_SP");
_Static_assert(offsetof(struct trap_context, gp) == TC_GP, "TC_GP");
_Static_assert(offsetof(struct trap_context, tp) == TC_TP, "TC_TP");
_Static_assert(offsetof(struct trap_context, t0) == TC_T0, "TC_T0");
_Static_assert(offsetof(struct trap_context, t1) == TC_T1, "TC_T1");
_Static_assert(offsetof(struct trap_context, t2) == TC_T2, "TC_T2");
_Static_assert(offsetof(struct trap_context, s0) == TC_S0, "TC_S0");
_Static_assert(offsetof(struct trap_context, s1) == TC_S1, "TC_S1");
_Static_assert(offsetof(struct trap_context, a0) == TC_A0, "TC_A0");
_Static_assert(offsetof(struct trap_context, a1) == TC_A1, "TC_A1");
_Static_assert(offsetof(struct trap_context, a2) == TC_A2, "TC_A2");
_Static_assert(offsetof(struct trap_context, a3) == TC_A3, "TC_A3");
_Static_assert(offsetof(struct trap_context, a4) == TC_A4, "TC_A4");
_Static_assert(offsetof(struct trap_context, a5) == TC_A5, "TC_A5");
_Static_assert(offsetof(struct trap_context, a6) == TC_A6, "TC_A6");
_Static_assert(offsetof(struct trap_context, a7) == TC_A7, "TC_A7");
_Static_assert(offsetof(struct trap_context, s2) == TC_S2, "TC_S2");
_Static_assert(offsetof(struct trap_context, s3) == TC_S3, "TC_S3");
_Static_assert(offsetof(struct trap_context, s4) == TC_S4, "TC_S4");
_Static_assert(offsetof(struct trap_context, s5) == TC_S5, "TC_S5");
_Static_assert(offsetof(struct trap_context, s6) == TC_S6, "TC_S6");
_Static_assert(offsetof(struct trap_context, s7) == TC_S7, "TC_S7");
_Static_assert(offsetof(struct trap_context, s8) == TC_S8, "TC_S8");
_Static_assert(offsetof(struct trap_context, s9) == TC_S9, "TC_S9");
_Static_assert(offsetof(struct trap_context, s10) == TC_S10, "TC_S10");
_Static_assert(offsetof(struct trap_context, s11) == TC_S11, "TC_S11");
_Static_assert(offsetof(struct trap_context, t3) == TC_T3, "TC_T3");
_Static_assert(offsetof(struct trap_context, t4) == TC_T4, "TC_T4");
_Static_assert(offsetof(struct trap_context, t5) == TC_T5, "TC_T5");
_Static_assert(offsetof(struct trap_context, t6) == TC_T6, "TC_T6");
_Static_assert(offsetof(struct trap_context, sepc) == TC_SEPC, "TC_SEPC");
_Static_assert(offsetof(struct trap_context, sstatus) == TC_SSTATUS, "TC_SSTATUS");
_Static_assert(offsetof(struct trap_context, scause) == TC_SCAUSE, "TC_SCAUSE");
_Static_assert(offsetof(struct trap_context, stval) == TC_STVAL, "TC_STVAL");
_Static_assert(offsetof(struct thread_context, ra) == THREAD_RA, "THREAD_RA");
_Static_assert(offsetof(struct thread_context, sp) == THREAD_SP, "THREAD_SP");
_Static_assert(offsetof(struct thread_context, s0) == THREAD_S0, "THREAD_S0");
_Static_assert(offsetof(struct thread_context, s1) == THREAD_S1, "THREAD_S1");
_Static_assert(offsetof(struct thread_context, s2) == THREAD_S2, "THREAD_S2");
_Static_assert(offsetof(struct thread_context, s3) == THREAD_S3, "THREAD_S3");
_Static_assert(offsetof(struct thread_context, s4) == THREAD_S4, "THREAD_S4");
_Static_assert(offsetof(struct thread_context, s5) == THREAD_S5, "THREAD_S5");
_Static_assert(offsetof(struct thread_context, s6) == THREAD_S6, "THREAD_S6");
_Static_assert(offsetof(struct thread_context, s7) == THREAD_S7, "THREAD_S7");
_Static_assert(offsetof(struct thread_context, s8) == THREAD_S8, "THREAD_S8");
_Static_assert(offsetof(struct thread_context, s9) == THREAD_S9, "THREAD_S9");
_Static_assert(offsetof(struct thread_context, s10) == THREAD_S10, "THREAD_S10");
_Static_assert(offsetof(struct thread_context, s11) == THREAD_S11, "THREAD_S11");
_Static_assert(offsetof(struct thread, thread_context) == 0, "thread context base");
_Static_assert(sizeof(struct trap_context) == TRAP_CONTEXT_SIZE, "trap context size");
_Static_assert(sizeof(struct thread_context) == THREAD_CONTEXT_SIZE, "thread context size");
_Static_assert(TRAP_CONTEXT_ALLOC_SIZE >= TRAP_CONTEXT_SIZE && TRAP_CONTEXT_ALLOC_SIZE % 16 == 0, "trap stack alignment");
