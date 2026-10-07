.section .text
.align 2

.global trap_entry
.global trap_return

.equ TC_RA, 0
.equ TC_SP, 8
.equ TC_GP, 16
.equ TC_TP, 24
.equ TC_T0, 32
.equ TC_T1, 40
.equ TC_T2, 48
.equ TC_S0, 56
.equ TC_S1, 64
.equ TC_A0, 72
.equ TC_A1, 80
.equ TC_A2, 88
.equ TC_A3, 96
.equ TC_A4, 104
.equ TC_A5, 112
.equ TC_A6, 120
.equ TC_A7, 128
.equ TC_S2, 136
.equ TC_S3, 144
.equ TC_S4, 152
.equ TC_S5, 160
.equ TC_S6, 168
.equ TC_S7, 176
.equ TC_S8, 184
.equ TC_S9, 192
.equ TC_S10, 200
.equ TC_S11, 208
.equ TC_T3, 216
.equ TC_T4, 224
.equ TC_T5, 232
.equ TC_T6, 240
.equ TC_SEPC, 248
.equ TC_SSTATUS, 256
.equ TC_SCAUSE, 264
.equ TC_STVAL, 272
.equ TRAP_CONTEXT_ALLOC_SIZE, 288

/* Supervisor trap entry.
 * - Trap from U-mode: sscratch holds the kernel stack top, so swap sp and
 *   sscratch to land on the kernel stack and preserve the user sp.
 * - In S-mode sscratch is zero. The swap produces a zero sp, so recover the
 *   interrupted kernel sp from sscratch without borrowing any other GPR.
 */
trap_entry:
    csrrw sp, sscratch, sp
    bnez sp, 1f
    csrr sp, sscratch
1:
    addi sp, sp, -TRAP_CONTEXT_ALLOC_SIZE

    /* Save the interrupted CPU context into trap_context. TC_SP always stores
     * the interrupted stack pointer, whether that was a user sp or a kernel sp.
     */
    sd ra, TC_RA(sp)
    sd t0, TC_T0(sp)
    csrr t0, sscratch
    sd t0, TC_SP(sp)
    /* Nested supervisor traps must stay on the current kernel stack. */
    csrw sscratch, zero
    sd gp, TC_GP(sp)
    sd tp, TC_TP(sp)
    sd t1, TC_T1(sp)
    sd t2, TC_T2(sp)
    sd s0, TC_S0(sp)
    sd s1, TC_S1(sp)
    sd a0, TC_A0(sp)
    sd a1, TC_A1(sp)
    sd a2, TC_A2(sp)
    sd a3, TC_A3(sp)
    sd a4, TC_A4(sp)
    sd a5, TC_A5(sp)
    sd a6, TC_A6(sp)
    sd a7, TC_A7(sp)
    sd s2, TC_S2(sp)
    sd s3, TC_S3(sp)
    sd s4, TC_S4(sp)
    sd s5, TC_S5(sp)
    sd s6, TC_S6(sp)
    sd s7, TC_S7(sp)
    sd s8, TC_S8(sp)
    sd s9, TC_S9(sp)
    sd s10, TC_S10(sp)
    sd s11, TC_S11(sp)
    sd t3, TC_T3(sp)
    sd t4, TC_T4(sp)
    sd t5, TC_T5(sp)
    sd t6, TC_T6(sp)

    csrr t0, sepc
    sd t0, TC_SEPC(sp)
    csrr t0, sstatus
    sd t0, TC_SSTATUS(sp)
    csrr t0, scause
    sd t0, TC_SCAUSE(sp)
    csrr t0, stval
    sd t0, TC_STVAL(sp)

    /* Hand the completed trap_context to the C-side trap dispatcher. */
    mv a0, sp
    call trap_dispatch

    /* Finish deferred work before switching tasks. In particular, the timer
     * top half masks STIE and its bottom half must rearm it before another
     * user task starts running, or that task would never be preempted.
     */
    call irq_task_run_before_return

    /* A nested S-mode trap must resume its interrupted kernel work, not
     * schedule away or abandon an outer user trap's still-live call chain.
     */
    ld t0, TC_SSTATUS(sp)
    andi t0, t0, 0x100
    bnez t0, 2f

    /* trap_dispatch() may classify the trap as SYS_exit and mark the current
     * user context as finished. In that case we must not restore the user
     * registers and execute another sret back to U-mode.
     */
    call user_has_exited
    bnez a0, trap_exit_stop

    call thread_consume_resched_request
    beqz a0, 1f
    call schedule
1:
2:
    mv a0, sp
    call trap_return

/* A finished user task must switch away; idle reclaims its stack later. */
trap_exit_stop:
    call user_schedule_after_exit
1:
    wfi
    j 1b

/* Common trap return path.
 * a0 must point to struct trap_context on the current kernel stack.
 */
trap_return:
    /* Initial user entry may call us with SIE set. Keep the entire restore
     * atomic, including the interval where sscratch changes its meaning.
     */
    csrci sstatus, 2
    mv t0, a0

    /* Restore CSR state first so sret returns to the correct PC and mode. */
    ld t1, TC_SEPC(t0)
    csrw sepc, t1
    ld t1, TC_SSTATUS(t0)
    andi t1, t1, -3
    csrw sstatus, t1

    /* Only a return to U-mode needs a kernel-stack address in sscratch. */
    andi t1, t1, 0x100
    bnez t1, 1f
    addi t1, t0, TRAP_CONTEXT_ALLOC_SIZE
    csrw sscratch, t1
    j 2f
1:
    csrw sscratch, zero
2:

    /* Restore the general-purpose registers from the trap_context image. */
    ld ra, TC_RA(t0)
    ld gp, TC_GP(t0)
    ld tp, TC_TP(t0)
    ld t1, TC_T1(t0)
    ld t2, TC_T2(t0)
    ld s0, TC_S0(t0)
    ld s1, TC_S1(t0)
    ld a0, TC_A0(t0)
    ld a1, TC_A1(t0)
    ld a2, TC_A2(t0)
    ld a3, TC_A3(t0)
    ld a4, TC_A4(t0)
    ld a5, TC_A5(t0)
    ld a6, TC_A6(t0)
    ld a7, TC_A7(t0)
    ld s2, TC_S2(t0)
    ld s3, TC_S3(t0)
    ld s4, TC_S4(t0)
    ld s5, TC_S5(t0)
    ld s6, TC_S6(t0)
    ld s7, TC_S7(t0)
    ld s8, TC_S8(t0)
    ld s9, TC_S9(t0)
    ld s10, TC_S10(t0)
    ld s11, TC_S11(t0)
    ld t3, TC_T3(t0)
    ld t4, TC_T4(t0)
    ld t5, TC_T5(t0)
    ld t6, TC_T6(t0)

    ld sp, TC_SP(t0)
    ld t0, TC_T0(t0)

    /* Return to the PC recorded in sepc, using the privilege encoded in sstatus. */
    sret
