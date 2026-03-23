#include <stdint.h>
#include "trap.h"
#include "user.h"

/* Minimal user-mode launch state for Basic Exercise 1. */
/* Single kernel stack used when the current user context traps back into S-mode. */
static uint8_t g_user_kstack[USER_KSTACK_SIZE] __attribute__((aligned(16)));

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

/* Prepare the initial user-mode context and transfer control through trap_return. */
void enter_user_mode(void)
{
    uint64_t sstatus;
    struct trapframe *tf;

    /* Prepare the minimal execution state for the next user-mode entry. */
    user_prepare_state();

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

    trap_return(tf);

    while (1) {
        asm volatile("wfi");
    }
}
