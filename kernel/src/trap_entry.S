.section .text
.align 2

.global trap_entry
.global trap_return

.equ TF_RA, 0
.equ TF_SP, 8
.equ TF_GP, 16
.equ TF_TP, 24
.equ TF_T0, 32
.equ TF_T1, 40
.equ TF_T2, 48
.equ TF_S0, 56
.equ TF_S1, 64
.equ TF_A0, 72
.equ TF_A1, 80
.equ TF_A2, 88
.equ TF_A3, 96
.equ TF_A4, 104
.equ TF_A5, 112
.equ TF_A6, 120
.equ TF_A7, 128
.equ TF_S2, 136
.equ TF_S3, 144
.equ TF_S4, 152
.equ TF_S5, 160
.equ TF_S6, 168
.equ TF_S7, 176
.equ TF_S8, 184
.equ TF_S9, 192
.equ TF_S10, 200
.equ TF_S11, 208
.equ TF_T3, 216
.equ TF_T4, 224
.equ TF_T5, 232
.equ TF_T6, 240
.equ TF_SEPC, 248
.equ TF_SSTATUS, 256
.equ TF_SCAUSE, 264
.equ TF_STVAL, 272
.equ TRAPFRAME_ALLOC_SIZE, 288

/* Supervisor trap entry.
 * - Trap from U-mode: sscratch holds the kernel stack top, so swap sp and
 *   sscratch to land on the kernel stack and preserve the user sp.
 * - Trap from S-mode: we are already on a kernel stack, so keep sp as-is and
 *   save the interrupted kernel sp into the trapframe directly.
 */
trap_entry:
    csrr t0, sstatus
    andi t0, t0, 0x100
    bnez t0, 1f

    /* U-mode trap path: switch from user sp to the task's kernel stack. */
    csrrw sp, sscratch, sp
    addi sp, sp, -TRAPFRAME_ALLOC_SIZE
    j 2f

1:
    /* S-mode trap path: keep the current kernel stack and allocate the
     * trapframe on it directly.
     */
    addi sp, sp, -TRAPFRAME_ALLOC_SIZE

2:

    /* Save the interrupted CPU context into the trapframe. TF_SP always stores
     * the interrupted stack pointer, whether that was a user sp or a kernel sp.
     */
    sd ra, TF_RA(sp)
    sd t0, TF_T0(sp)
    csrr t0, sstatus
    andi t0, t0, 0x100
    bnez t0, 3f
    csrr t0, sscratch
    j 4f
3:
    addi t0, sp, TRAPFRAME_ALLOC_SIZE
4:
    sd t0, TF_SP(sp)
    sd gp, TF_GP(sp)
    sd tp, TF_TP(sp)
    sd t1, TF_T1(sp)
    sd t2, TF_T2(sp)
    sd s0, TF_S0(sp)
    sd s1, TF_S1(sp)
    sd a0, TF_A0(sp)
    sd a1, TF_A1(sp)
    sd a2, TF_A2(sp)
    sd a3, TF_A3(sp)
    sd a4, TF_A4(sp)
    sd a5, TF_A5(sp)
    sd a6, TF_A6(sp)
    sd a7, TF_A7(sp)
    sd s2, TF_S2(sp)
    sd s3, TF_S3(sp)
    sd s4, TF_S4(sp)
    sd s5, TF_S5(sp)
    sd s6, TF_S6(sp)
    sd s7, TF_S7(sp)
    sd s8, TF_S8(sp)
    sd s9, TF_S9(sp)
    sd s10, TF_S10(sp)
    sd s11, TF_S11(sp)
    sd t3, TF_T3(sp)
    sd t4, TF_T4(sp)
    sd t5, TF_T5(sp)
    sd t6, TF_T6(sp)

    csrr t0, sepc
    sd t0, TF_SEPC(sp)
    csrr t0, sstatus
    sd t0, TF_SSTATUS(sp)
    csrr t0, scause
    sd t0, TF_SCAUSE(sp)
    csrr t0, stval
    sd t0, TF_STVAL(sp)

    /* Hand the completed trapframe to the C-side trap dispatcher. */
    mv a0, sp
    call trap_dispatch

    /* trap_dispatch() may classify the trap as SYS_exit and mark the current
     * user context as finished. In that case we must not restore the user
     * registers and execute another sret back to U-mode.
     */
    call user_has_exited
    bnez a0, trap_exit_stop

    /* Normal trap path: restore the saved trapframe and return to the
     * interrupted user context through sret.
     */
    mv a0, sp
    call trap_return

/* Resume the shell after SYS_exit or a fatal user fault.
 * This toy kernel does not have a scheduler, so once a user program is done
 * we stay on the current kernel stack and re-enter the shell loop directly.
 */
trap_exit_stop:
    call user_return_to_shell
1:
    wfi
    j 1b

/* Common trap return path.
 * a0 must point to struct trapframe on the current kernel stack.
 */
trap_return:
    mv t0, a0

    /* Restore CSR state first so sret returns to the correct PC and mode. */
    ld t1, TF_SEPC(t0)
    csrw sepc, t1
    ld t1, TF_SSTATUS(t0)
    csrw sstatus, t1

    /* Restore the general-purpose registers from the trapframe image. */
    ld ra, TF_RA(t0)
    ld gp, TF_GP(t0)
    ld tp, TF_TP(t0)
    ld t1, TF_T1(t0)
    ld t2, TF_T2(t0)
    ld s0, TF_S0(t0)
    ld s1, TF_S1(t0)
    ld a0, TF_A0(t0)
    ld a1, TF_A1(t0)
    ld a2, TF_A2(t0)
    ld a3, TF_A3(t0)
    ld a4, TF_A4(t0)
    ld a5, TF_A5(t0)
    ld a6, TF_A6(t0)
    ld a7, TF_A7(t0)
    ld s2, TF_S2(t0)
    ld s3, TF_S3(t0)
    ld s4, TF_S4(t0)
    ld s5, TF_S5(t0)
    ld s6, TF_S6(t0)
    ld s7, TF_S7(t0)
    ld s8, TF_S8(t0)
    ld s9, TF_S9(t0)
    ld s10, TF_S10(t0)
    ld s11, TF_S11(t0)
    ld t3, TF_T3(t0)
    ld t4, TF_T4(t0)
    ld t5, TF_T5(t0)
    ld t6, TF_T6(t0)

    addi t1, t0, TRAPFRAME_ALLOC_SIZE
    csrw sscratch, t1
    ld sp, TF_SP(t0)
    ld t0, TF_T0(t0)

    /* Return to the PC recorded in sepc, using the privilege encoded in sstatus. */
    sret
