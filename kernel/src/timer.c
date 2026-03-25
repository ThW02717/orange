#include "timer.h"
#include "trap.h"
#include "uart.h"

/* Shared timer subsystem state. Later interrupt handling updates the same
 * values instead of inventing another source of truth.
 */
uint32_t g_timebase_freq;
uint64_t g_boot_time;
uint64_t g_next_deadline;
uint64_t g_uptime_seconds;

/* rdtime returns the monotonically increasing platform time counter. */
uint64_t timer_read(void)
{
    uint64_t value;

    
    asm volatile("rdtime %0" : "=r"(value));
    return value;
}

/* Use the first timer read after initialization as the uptime baseline. */
void timer_init_state(uint32_t timebase_freq)
{
    
    g_timebase_freq = timebase_freq;
    g_boot_time = timer_read();
    g_next_deadline = 0;
    g_uptime_seconds = 0;
}

/* Program the next timer interrupt at an absolute time point. */
struct sbiret timer_program_abs(uint64_t deadline)
{
    
    g_next_deadline = deadline;
    return sbi_set_timer(deadline);
}

/* Convert a relative delay into the absolute deadline expected by SBI. */
struct sbiret timer_program_rel(uint64_t delta_ticks)
{
    uint64_t now;

    
    now = timer_read();
    return timer_program_abs(now + delta_ticks);
}

void timer_init(void) {
    uint64_t sie;
    uint64_t sstatus;

    /* Start timer bookkeeping from a fresh baseline and schedule the first
     * one-second event. The CSR writes below then open the timer interrupt
     * source and the global supervisor interrupt gate.
     */
    g_next_deadline = 0;

    if (g_timebase_freq == 0) {
        return;
    }

    g_boot_time = timer_read();
    g_uptime_seconds = 0;
    timer_program_rel(g_timebase_freq);

    /* Enable the supervisor timer interrupt source in sie. Without STIE,
     * the programmed timer deadline can fire in firmware/hardware but S-mode
     * still will not receive the timer interrupt.
     */
    asm volatile("csrr %0, sie" : "=r"(sie));
    sie |= SIE_STIE;
    asm volatile("csrw sie, %0" : : "r"(sie));

    /* Enable supervisor interrupts globally in sstatus. This is the final
     * gate; the timer source must be enabled in both sie and sstatus.
     */
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    sstatus |= SSTATUS_SIE;
    asm volatile("csrw sstatus, %0" : : "r"(sstatus));
    uart_send_string("[timer] init done\n");
    uart_send_string("[timer] boot_time=");
    uart_send_dec(g_boot_time);
    uart_send_string("\n");
    uart_send_string("[timer] first deadline=");
    uart_send_dec(g_next_deadline);
    uart_send_string("\n");
}

void timer_handle_interrupt(void)
{
    uint64_t now;
    uint64_t elapsed_ticks;

    if (g_timebase_freq == 0) {
        return;
    }

    now = timer_read();
    elapsed_ticks = now - g_boot_time;
    g_uptime_seconds = elapsed_ticks / g_timebase_freq;

    uart_send_string("\n[timer] irq: ");
    uart_send_dec(g_uptime_seconds);
    uart_send_string(" sec\n");

    /* The first interrupt is one second after init. Subsequent interrupts
     * follow the required two-second cadence.
     */
    timer_program_rel(2UL * (uint64_t)g_timebase_freq);
}

void timer_stop(void)
{
    uint64_t sie;

    /* Mask the supervisor timer interrupt source first, then push the next
     * event effectively infinitely far away so firmware stops re-triggering it.
     */
    asm volatile("csrr %0, sie" : "=r"(sie));
    sie &= ~SIE_STIE;
    asm volatile("csrw sie, %0" : : "r"(sie));

    g_next_deadline = ~0ULL;
    (void)sbi_set_timer(~0ULL);

    uart_send_string("[timer] stopped\n");
}
