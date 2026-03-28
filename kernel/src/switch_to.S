    .section .text
    .globl switch_to

/* Cooperative thread context switch.
 *
 * a0 = prev thread pointer
 * a1 = next thread pointer
 *
 * Because `struct thread_context` is the first field of `struct thread`,
 * each thread pointer can be used directly as the base address of the saved
 * context image:
 *
 *   0   -> ra
 *   8   -> sp
 *   16  -> s0
 *   ...
 *   104 -> s11
 *
 * This first version only saves/restores ra, sp, and s0-s11 because the
 * scheduler is cooperative. Caller-saved registers are not preserved across
 * switch_to(), just like an ordinary function call boundary.
 */
switch_to:
    sd ra,   0(a0)
    sd sp,   8(a0)
    sd s0,  16(a0)
    sd s1,  24(a0)
    sd s2,  32(a0)
    sd s3,  40(a0)
    sd s4,  48(a0)
    sd s5,  56(a0)
    sd s6,  64(a0)
    sd s7,  72(a0)
    sd s8,  80(a0)
    sd s9,  88(a0)
    sd s10, 96(a0)
    sd s11, 104(a0)

    ld ra,   0(a1)
    ld sp,   8(a1)
    ld s0,  16(a1)
    ld s1,  24(a1)
    ld s2,  32(a1)
    ld s3,  40(a1)
    ld s4,  48(a1)
    ld s5,  56(a1)
    ld s6,  64(a1)
    ld s7,  72(a1)
    ld s8,  80(a1)
    ld s9,  88(a1)
    ld s10, 96(a1)
    ld s11, 104(a1)

    /* Keep the current-thread pointer in tp so later code can fetch the
     * running thread without going through a global variable.
     */
    mv tp, a1
    ret
