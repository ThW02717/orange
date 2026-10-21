#include "context_offsets.h"

    .section .text
    .globl switch_to

/*
 * Switch the kernel call context: a0 = prev thread, a1 = next thread.
 *
 * thread_context must remain the first member of struct thread so both
 * pointers also address the saved ra, sp and s0-s11 directly.
 * This is a C-call boundary, not a full trap snapshot; interrupted registers
 * are saved separately in trap_context.
 */
switch_to:
    sd ra, THREAD_RA(a0)
    sd sp, THREAD_SP(a0)
    sd s0, THREAD_S0(a0)
    sd s1, THREAD_S1(a0)
    sd s2, THREAD_S2(a0)
    sd s3, THREAD_S3(a0)
    sd s4, THREAD_S4(a0)
    sd s5, THREAD_S5(a0)
    sd s6, THREAD_S6(a0)
    sd s7, THREAD_S7(a0)
    sd s8, THREAD_S8(a0)
    sd s9, THREAD_S9(a0)
    sd s10, THREAD_S10(a0)
    sd s11, THREAD_S11(a0)

    ld ra, THREAD_RA(a1)
    ld sp, THREAD_SP(a1)
    ld s0, THREAD_S0(a1)
    ld s1, THREAD_S1(a1)
    ld s2, THREAD_S2(a1)
    ld s3, THREAD_S3(a1)
    ld s4, THREAD_S4(a1)
    ld s5, THREAD_S5(a1)
    ld s6, THREAD_S6(a1)
    ld s7, THREAD_S7(a1)
    ld s8, THREAD_S8(a1)
    ld s9, THREAD_S9(a1)
    ld s10, THREAD_S10(a1)
    ld s11, THREAD_S11(a1)

    /* Both user and kernel code reserve tp for the running thread. */
    mv tp, a1
    ret
