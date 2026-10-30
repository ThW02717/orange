#include "platform.h"

#include <stdint.h>
#include "uart.h"
#include "fdt.h"
#include "shell.h"
#include "sbi.h"
#include "memory.h"
#include "timer.h"
#include "trap.h"
#include "bottom_half.h"

/*
 * Bootstrap-core-only kernel entry. Secondary harts are started after the
 * primary hart initializes memory, platform resources and interrupt state.
 */
static volatile unsigned long bootstrap_hart = ~0UL;
static volatile unsigned int greet_ticket = 0;
static volatile unsigned int greet_serving = 0;
static volatile unsigned int greeted_count = 0;
static volatile unsigned int started_cores = 1;

#define MAX_CORES 8
#define SECONDARY_STACK_SIZE 4096

static unsigned char secondary_stacks[MAX_CORES - 1][SECONDARY_STACK_SIZE] __attribute__((aligned(16)));
extern void secondary_start(void);

static void greet_once(unsigned long core_id)
{
    unsigned int my_ticket = __sync_fetch_and_add(&greet_ticket, 1);
    while (greet_serving != my_ticket) {
        asm volatile("nop");
    }

    uart_send_string("Hello from core ");
    uart_send_dec(core_id);
    uart_send_string("\n");

    __sync_synchronize();
    greet_serving++;
    __sync_fetch_and_add(&greeted_count, 1);
}

void secondary_main(unsigned long hartid)
{
    greet_once(hartid);
    while (1) {
        asm volatile("wfi");
    }
}

void kernel_main(unsigned long hartid, unsigned long dtb_addr,
                 unsigned long initrd_start_hint, unsigned long initrd_end_hint)
{
    unsigned int stack_slot = 0;
    unsigned long target_hart;

    /* Only the bootstrap hart continues through the full kernel init path. */
    if (bootstrap_hart == ~0UL) {
        bootstrap_hart = hartid;
    }

    if (hartid != bootstrap_hart) {
        while (1) {
            asm volatile("wfi");
        }        
    }

    if (platform_init(hartid, dtb_addr, initrd_start_hint, initrd_end_hint) != 0) {
        while (1) {
            asm volatile("wfi");
        }
    }

    /* Reserve the same archive range that the shell and loader will use. */
    memory_init((const void *)dtb_addr, platform_boot_info()->initrd_start,
                platform_boot_info()->initrd_end);
    /*
     * Timer interrupts are delivered while the shell is running in S-mode,
     * so stvec must already point at trap_entry before timeron can be used.
     */
    trap_init();

    /* Bring up the remaining harts with private secondary stacks. */
    for (target_hart = 0; target_hart < MAX_CORES; target_hart++) {
        long ret;
        unsigned long stack_top;

        if (target_hart == hartid) {
            continue;
        }
        if (stack_slot >= (MAX_CORES - 1)) {
            break;
        }

        stack_top = (unsigned long)&secondary_stacks[stack_slot][SECONDARY_STACK_SIZE];
        ret = sbi_hart_start(target_hart, (unsigned long)&secondary_start, stack_top);
        if (ret == SBI_SUCCESS) {
            started_cores++;
            stack_slot++;
        }
    }

    greet_once(hartid);

    {
        unsigned long wait = 50000000UL;
        while (greeted_count < started_cores && wait-- > 0) {
            asm volatile("nop");
        }
        if (greeted_count < started_cores) {
            uart_send_string("core greet timeout: ");
            uart_send_dec(greeted_count);
            uart_send_string("/");
            uart_send_dec(started_cores);
            uart_send_string("\n");
        }
    }

    /*
     * Print the DT-provided timer frequency after the multi-core greeting
     * burst settles down so the value is easier to read on the shared UART.
     */
    if (fdt_get_timebase_frequency((const void *)dtb_addr, &timebase_frequency) == 0) {
        timer_init_state(timebase_frequency);
        uart_send_string("timer freq = ");
        uart_send_dec(timebase_frequency);
        uart_send_string("\n");
    } else {
        uart_send_string("timer freq = <unavailable>\n");
    }

    bottom_half_init();

    /* One SBI timer serves scheduler ticks and queued timeout events. */
    timer_init();
    /* Arm UART interrupts before the shell starts using the RX/TX rings. */
    uart_irq_init();
    uart_send_string("uart: interrupt-driven I/O armed at boot\n");

    /* The primary hart remains in the shell loop for the rest of boot. */
    shell_run();
}
