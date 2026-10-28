#include "csr.h"
#include "context_offsets.h"

.section .text
.align 2

.global trap_entry
.global trap_return

/*
 * Enter an S-mode trap without clobbering the interrupted registers.
 *
 * U-origin: sscratch holds the kernel stack top; the swap selects that stack.
 * S-origin: sscratch is zero; recover the interrupted kernel sp after the swap.
 * Both paths leave the interrupted sp in sscratch until TC_SP is saved.
 */
trap_entry:
    csrrw sp, sscratch, sp
    bnez sp, trap_save_context
    csrr sp, sscratch
trap_save_context:
    addi sp, sp, -TRAP_CONTEXT_ALLOC_SIZE

    /* Preserve t0 before borrowing it to save the interrupted sp. */
    sd ra, TC_RA(sp)
    sd t0, TC_T0(sp)
    csrr t0, sscratch
    sd t0, TC_SP(sp)
    /* Keep sscratch zero in kernel code so nested traps stay on this stack. */
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

    /* Snapshot the trap CSRs before C code can take another trap. */
    csrr t0, sepc
    sd t0, TC_SEPC(sp)
    csrr t0, sstatus
    sd t0, TC_SSTATUS(sp)
    csrr t0, scause
    sd t0, TC_SCAUSE(sp)
    csrr t0, stval
    sd t0, TC_STVAL(sp)

    /*
     * Handle the event, then run bottom halves with SIE enabled.
     * If a callback is already running, nested traps leave new work queued
     * and resume it. The outer runner drains work before user scheduling.
     */
    mv a0, sp /* trap_context */
    call trap_dispatch

    call run_bottom_halves

    /*
     * Saved SPP: 0 = U-mode, 1 = S-mode.
     * Resume S-origin kernel work directly. Only U-origin returns check user
     * exit and rescheduling below.
     */
    ld t0, TC_SSTATUS(sp)
    andi t0, t0, SSTATUS_SPP /* t0: 0 = U-origin, 0x100 = S-origin */
    bnez t0, trap_restore_context

    call user_has_exited /* a0: 0 = alive, 1 = exited */
    bnez a0, trap_switch_from_exited_user

    call thread_consume_resched_request /* a0: 0 = resume, 1 = schedule */
    beqz a0, trap_restore_context
    call schedule
trap_restore_context:
    /* A switched-out thread reaches here only after it is scheduled again. */
    mv a0, sp
    call trap_return

/*
 * Never restore an exited user context. Switch away before idle reclaims
 * the current stack.
 */
trap_switch_from_exited_user:
    call user_schedule_after_exit
trap_exit_fallback:
    wfi
    j trap_exit_fallback

/*
 * Restore a trap_context on the current kernel stack; a0 = context address.
 * Also used for a new thread's first entry into U-mode. scause and stval are
 * diagnostic snapshots, not inputs to the return path.
 */
trap_return:
    /*
     * Initial user entry may arrive with SIE set. Keep it clear until sret;
     * an interrupt must not see a half-restored context or sscratch.
     */
    csrci sstatus, 2
    mv t0, a0

    ld t1, TC_SEPC(t0)
    csrw sepc, t1
    ld t1, TC_SSTATUS(t0)
    andi t1, t1, -3 /* Clear SIE; sret restores it from SPIE. */
    csrw sstatus, t1

    /*
     * Return SPP: 0 = U-mode, 1 = S-mode.
     * Only U-mode needs a kernel stack top in sscratch for its next trap;
     * S-mode keeps sscratch zero to continue on the current kernel stack.
     */
    andi t1, t1, SSTATUS_SPP
    bnez t1, trap_return_to_supervisor
    addi t1, t0, TRAP_CONTEXT_ALLOC_SIZE
    csrw sscratch, t1
    j trap_restore_registers
trap_return_to_supervisor:
    csrw sscratch, zero
trap_restore_registers:

    /* Keep t0 as the context base until the final register load. */
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

    sret
