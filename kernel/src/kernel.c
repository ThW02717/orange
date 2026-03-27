
#include <stdint.h>
#include "uart.h"
#include "fdt.h"
#include "shell.h"
#include "sbi.h"
#include "memory.h"
#include "timer.h"
#include "trap.h"

/* Bootstrap-core-only kernel entry. Secondary harts are started after the
 * primary hart initializes memory, shell context, and other global state.
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

/* Boot-time rdtime probe only: add a little distance between reads so the
 * monotonic counter is visible on a fast CPU.
 */
static void timer_probe_delay(unsigned long count)
{
    while (count-- > 0) {
        asm volatile("nop");
    }
}

static void timer_probe_readout(void)
{
    uint64_t t1;
    uint64_t t2;
    uint64_t t3;

    /* Read the raw time counter a few times to confirm it is increasing
     * before building interrupt scheduling on top of it.
     */
    t1 = timer_read();
    timer_probe_delay(10000UL);
    t2 = timer_read();
    timer_probe_delay(10000UL);
    t3 = timer_read();

    uart_send_string("timer read: t1=");
    uart_send_dec(t1);
    uart_send_string(" t2=");
    uart_send_dec(t2);
    uart_send_string(" t3=");
    uart_send_dec(t3);
    uart_send_string("\n");
}

static void timer_probe_state(void)
{
    /* Dump the timer subsystem's initial bookkeeping before interrupts are
     * wired in so later regressions have a clear baseline.
     */
    uart_send_string("timer state: boot=");
    uart_send_dec(g_boot_time);
    uart_send_string(" next=");
    uart_send_dec(g_next_deadline);
    uart_send_string(" uptime=");
    uart_send_dec(g_uptime_seconds);
    uart_send_string("\n");
}

static void timer_probe_sbi_support(void)
{
    long supported;

    /* Probe the SBI TIME extension explicitly so we can distinguish
     * "deadline math is wrong" from "firmware does not implement TIME".
     */
    supported = sbi_probe_extension(SBI_EXT_TIMER);
    uart_send_string("sbi TIME ext = ");
    uart_send_dec((unsigned long)supported);
    uart_send_string("\n");
}

static void greet_once(unsigned long core_id) {
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

void secondary_main(unsigned long hartid) {
    greet_once(hartid);
    while (1) {
        asm volatile("wfi");
    }
}

void kernel_main(unsigned long hartid, unsigned long dtb_addr,
                 unsigned long initrd_start_hint, unsigned long initrd_end_hint) {
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

    if (uart_platform_init_from_fdt((const void *)dtb_addr) != 0) {
        while (1) {
            asm volatile("wfi");
        }
    }

    /* Keep bootloader UART state first to avoid serial regressions. */
    shell_set_context(hartid, dtb_addr, (uint64_t)initrd_start_hint, (uint64_t)initrd_end_hint);
    memory_init((const void *)dtb_addr, (uint64_t)initrd_start_hint, (uint64_t)initrd_end_hint);
    /* Timer interrupts are delivered while the shell is running in S-mode,
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

    /* Print the DT-provided timer frequency after the multi-core greeting
     * burst settles down so the value is easier to read on the shared UART.
     */
    if (fdt_get_timebase_frequency((const void *)dtb_addr, &g_timebase_freq) == 0) {
        timer_init_state(g_timebase_freq);
        uart_send_string("timer freq = ");
        uart_send_dec(g_timebase_freq);
        uart_send_string("\n");
    } else {
        uart_send_string("timer freq = <unavailable>\n");
    }

    timer_probe_state();
    timer_probe_readout();
    timer_probe_sbi_support();

    /* The multiplexed timer subsystem no longer depends on a fixed periodic
     * heartbeat, so it is safe to arm the timer path during boot. The single
     * hardware timer stays idle until software timers are queued.
     */
    timer_init();
    /* Arm the UART external-interrupt route during boot so shell I/O can
     * transition to the asynchronous path without a manual enable command.
     * uart_recv() still keeps a polling fallback until the first real RX IRQ
     * is observed, which keeps bring-up interactive on real hardware.
     */
    uart_irq_init();
    uart_send_string("uart: interrupt-driven I/O armed at boot\n");

    /* The primary hart remains in the shell loop for the rest of boot. */
    {
        int32_t pid = 1;
        while (1) {
            runAShell(pid++);
        }
    }

    while (1) {
        asm volatile("wfi");
    }
}
