#include <stdint.h>
#include "shell.h"
#include "trap.h"
#include "uart.h"
#include "user.h"

/* Minimal user-mode launch state for Basic Exercise 1. */

/* Single kernel stack used when the current user context traps back into S-mode. */
static uint8_t g_user_kstack[USER_KSTACK_SIZE] __attribute__((aligned(16)));
static int g_user_exited = 0;
static int32_t g_shell_resume_pid = 1;

/* Minimal state needed to enter U-mode and recover on the next trap. */
struct user_state {
    uintptr_t user_entry;
    uintptr_t user_sp;
    uintptr_t kstack_top;
};

static struct user_state g_user_state;

/* Prepare the initial user entry, user stack, and kernel trap stack. */
static void user_prepare_state(void)
{
    g_user_state.user_entry = USER_CODE_BASE;
    g_user_state.user_sp = USER_STACK_TOP;
    g_user_state.kstack_top = (uintptr_t)&g_user_kstack[USER_KSTACK_SIZE];
}

/* Build the initial trapframe image that trap_return will restore into U-mode. */
static void user_prepare_trapframe(struct trapframe *tf, uint64_t sstatus)
{
    unsigned int i;
    uint64_t *raw = (uint64_t *)tf;

    for (i = 0; i < (sizeof(*tf) / sizeof(uint64_t)); i++) {
        raw[i] = 0;
    }

    tf->sp = g_user_state.user_sp;
    tf->sepc = g_user_state.user_entry;
    tf->sstatus = sstatus;
}

void user_mark_exit(void) {
    g_user_exited = 1;
}

int user_has_exited(void) {
    return g_user_exited;
}

/* Resume the interactive shell after SYS_exit or a fatal user fault.
 * This kernel does not have a scheduler yet, so the simplest safe handoff is
 * to re-enter the shell loop directly on the current kernel stack.
 */
void user_return_to_shell(void)
{
    uint64_t sstatus;

    /* Once control is back in the shell, later S-mode interrupts must follow
     * the normal trap-return path again. Leave the "user exited" state as a
     * one-shot handoff signal instead of a permanent sticky flag.
     */
    g_user_exited = 0;

    /* trap_exit_stop reaches the shell without going through trap_return, so
     * the current S-mode interrupt-enable bit stays cleared unless we restore
     * it here. The shell relies on UART external interrupts to fill the RX/TX
     * ring buffers, so re-enable S-mode interrupts before re-entering the
     * REPL.
     */
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    sstatus |= SSTATUS_SIE;
    asm volatile("csrw sstatus, %0" : : "r"(sstatus));

    while (1) {
        runAShell(g_shell_resume_pid++);
    }
}

/* Prepare the initial user-mode context and transfer control through trap_return. */
void enter_user_mode(void)
{
    uint64_t sstatus;
    struct trapframe *tf;

    g_user_exited = 0;

    /* Prepare the minimal execution state for the next user-mode entry. */
    user_prepare_state();

    uart_send_string("[user] prepare U-mode entry=");
    uart_send_hex((unsigned long)g_user_state.user_entry);
    uart_send_string(" sp=");
    uart_send_hex((unsigned long)g_user_state.user_sp);
    uart_send_string(" kstack=");
    uart_send_hex((unsigned long)g_user_state.kstack_top);
    uart_send_string("\n");

    /* Register the supervisor trap entry before dropping privilege. */
    trap_init();

    /* sscratch holds the kernel stack top used by trap_entry. */
    asm volatile("csrw sscratch, %0" : : "r"(g_user_state.kstack_top));

    /* sepc is the PC that sret will jump to when entering U-mode. */
    asm volatile("csrw sepc, %0" : : "r"(g_user_state.user_entry));

    /* Program the return state for a later sret:
     * clear SPP so sret returns to U-mode, and set SPIE so the restored
     * interrupt-enable state matches the expected supervisor return path.
     */
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    sstatus &= ~SSTATUS_SPP;
    sstatus |= SSTATUS_SPIE;
    asm volatile("csrw sstatus, %0" : : "r"(sstatus));

    /* Place the initial trapframe at the top of the dedicated user kernel stack
     * so trap_return will re-arm sscratch to the same stack for the next trap.
     */
    tf = (struct trapframe *)(uintptr_t)(g_user_state.kstack_top - TRAPFRAME_ALLOC_SIZE);
    user_prepare_trapframe(tf, sstatus);

    uart_send_string("[user] sret -> U-mode\n");

    /* trap_return restores this synthetic trapframe and performs the first
     * sret into the loaded user program.
     */
    trap_return(tf);

    while (1) {
        asm volatile("wfi");
    }
}
