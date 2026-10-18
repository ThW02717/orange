#include "timer.h"
#include "demo.h"
#include "trap.h"
#include "uart.h"
#include "memory.h"
#include "shell.h"
#include "irq_task.h"

/* Shared timer subsystem state.
 * - g_next_deadline is the earliest scheduler tick or software-timer deadline
 * - g_uptime_seconds is updated from the hardware time counter on each IRQ
 */
uint32_t g_timebase_freq;
uint64_t g_boot_time;
uint64_t g_next_deadline;
uint64_t g_uptime_seconds;
uint64_t g_timer_irq_count;

static struct timer_event *g_timer_head = 0;
static int g_timer_enabled = 0;
static uint64_t g_sched_tick_interval = 0;
static uint64_t g_sched_tick_deadline = 0;
static uint64_t g_periodic_demo_deadline = 0;
static struct irq_task g_timer_task = {
    .type = IRQ_TASK_TIMER,
    .prio = 0,
    .state = IRQ_TASK_IDLE,
    .next = 0,
    .run = timer_task_run,
};
/* Invariants for the software-timer queue:
 * 1. g_timer_head points to a list sorted by expire, smallest first.
 * 2. The hardware deadline is the earliest tick, demo, or pending event.
 */

/* rdtime returns the monotonically increasing platform time counter. */
uint64_t timer_read(void)
{
    uint64_t value;

    asm volatile("rdtime %0" : "=r"(value));
    return value;
}

static inline uint64_t timer_irq_save(void)
{
    uint64_t sstatus;

    asm volatile("csrrc %0, sstatus, %1" : "=r"(sstatus) : "r"(SSTATUS_SIE) : "memory");
    return sstatus;
}

static inline void timer_irq_restore(uint64_t sstatus)
{
    asm volatile("csrw sstatus, %0" : : "r"(sstatus) : "memory");
}

/* Enable/disable only the hardware timer interrupt source. The software queue
 * can still exist while the source is masked.
 */
void timer_source_mask(void)
{
    asm volatile("csrc sie, %0" : : "r"(SIE_STIE) : "memory");
}

void timer_source_unmask(void)
{
    asm volatile("csrs sie, %0" : : "r"(SIE_STIE) : "memory");
    asm volatile("csrs sstatus, %0" : : "r"(SSTATUS_SIE) : "memory");
}

static void timer_sync_uptime(uint64_t now)
{
    if (g_timebase_freq == 0) {
        g_uptime_seconds = 0;
        return;
    }

    g_uptime_seconds = (now - g_boot_time) / g_timebase_freq;
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
    return timer_program_abs(timer_read() + delta_ticks);
}

/* One SBI timer serves the scheduler and all software events. Caller holds
 * the IRQ lock so queue changes and the programmed deadline stay consistent.
 */
static void timer_reprogram_locked(void)
{
    uint64_t deadline = 0;

    if (g_sched_tick_interval != 0 && g_sched_tick_deadline != 0) {
        deadline = g_sched_tick_deadline;
    }
    if (g_timer_head != 0 && (deadline == 0 || g_timer_head->expire < deadline)) {
        deadline = g_timer_head->expire;
    }
    if (g_periodic_demo_deadline != 0 &&
        (deadline == 0 || g_periodic_demo_deadline < deadline)) {
        deadline = g_periodic_demo_deadline;
    }

    if (deadline != 0) {
        (void)timer_program_abs(deadline);
        return;
    }

    g_next_deadline = 0;
    (void)sbi_set_timer(~0ULL);
}

/* Use the first timer read after initialization as the uptime baseline. */
void timer_init_state(uint32_t timebase_freq)
{
    g_timebase_freq = timebase_freq;
    g_boot_time = timer_read();
    g_next_deadline = 0;
    g_uptime_seconds = 0;
    g_timer_irq_count = 0;
    g_sched_tick_interval = 0;
    g_sched_tick_deadline = 0;
    g_periodic_demo_deadline = 0;
}

void timer_init(void)
{
    uint64_t sstatus;

    if (g_timebase_freq == 0) {
        return;
    }

    /* Share the one-shot hardware timer with a 20 Hz scheduling deadline. */
    sstatus = timer_irq_save();
    g_timer_enabled = 1;
    g_sched_tick_interval = (g_timebase_freq >= 20U) ? ((uint64_t)g_timebase_freq / 20ULL) : 1ULL;
    g_sched_tick_deadline = timer_read() + g_sched_tick_interval;
    timer_reprogram_locked();
    timer_irq_restore(sstatus);

    timer_source_unmask();

    uart_send_string("[timer] subsystem armed\n");
    uart_send_string("[timer] pending events=");
    uart_send_dec(timer_pending_count());
    uart_send_string("\n");
    uart_send_string("[timer] next deadline=");
    uart_send_dec(g_next_deadline);
    uart_send_string("\n");
}

int add_timer(timer_callback_t callback, void *arg, uint64_t duration_ticks)
{
    return add_timer_with_cleanup(callback, arg, duration_ticks, 0);
}

int add_timer_with_cleanup(timer_callback_t callback, void *arg,
                           uint64_t duration_ticks, timer_callback_t cleanup)
{
    struct timer_event *event;
    struct timer_event **link;
    uint64_t sstatus;
    uint64_t expire;

    if (callback == 0 || g_timebase_freq == 0) {
        return -1;
    }

    event = (struct timer_event *)kmalloc(sizeof(*event));
    if (event == 0) {
        return -1;
    }

    expire = timer_read();
    if (duration_ticks > UINT64_MAX - expire) {
        kfree(event);
        return -1;
    }
    expire += duration_ticks;
    event->expire = expire;
    event->callback = callback;
    event->cleanup = cleanup;
    event->arg = arg;
    event->next = 0;

    /* A higher-priority callback can add a timer while a lower one is paused.
     * Publish the event and reprogram the timer under the same IRQ lock.
     */
    sstatus = timer_irq_save();
    if (!g_timer_enabled) {
        timer_irq_restore(sstatus);
        kfree(event);
        return -1;
    }

    /* Sorted insert by absolute expire so the queue head is always the next
     * event that hardware must wake up for.
     */
    link = &g_timer_head;
    while (*link != 0 && (*link)->expire <= event->expire) {
        link = &(*link)->next;
    }
    event->next = *link;
    *link = event;

    timer_reprogram_locked();
    timer_irq_restore(sstatus);
    return 0;
}

int timer_periodic_demo(int enabled)
{
    uint64_t status = timer_irq_save();

    if (!g_timer_enabled || g_timebase_freq == 0) {
        timer_irq_restore(status);
        return -1;
    }
    g_periodic_demo_deadline = enabled ?
        timer_read() + 2ULL * g_timebase_freq : 0;
    timer_reprogram_locked();
    timer_irq_restore(status);
    return 0;
}

void timer_irq_top(void)
{
    timer_source_mask();
    /* Do not walk the timeout list while entering the interrupt. */
    demo_trace_record(DEMO_TRACE_TIMER_TOP, 0U);
    (void)irq_task_enqueue(&g_timer_task);
}

int timer_task_run(struct irq_task *task)
{
    uint64_t now;
    uint64_t status;
    int fired_any = 0;

    (void)task;
    if (!g_timer_enabled) { return 0; }
    demo_trace_record(DEMO_TRACE_TIMER_BOTTOM, timer_pending_count());

    if (g_timebase_freq == 0) {
        timer_source_unmask();
        return 0;
    }

    g_timer_irq_count++;
    for (;;) {
        struct timer_event *event;
        int print_uptime = 0;

        status = timer_irq_save();
        if (!g_timer_enabled) {
            timer_irq_restore(status);
            break;
        }
        now = timer_read();
        timer_sync_uptime(now);
        while (g_sched_tick_interval != 0 && g_sched_tick_deadline <= now) {
            g_sched_tick_deadline += g_sched_tick_interval;
        }
        if (g_periodic_demo_deadline != 0 && g_periodic_demo_deadline <= now) {
            g_periodic_demo_deadline = now + 2ULL * g_timebase_freq;
            print_uptime = 1;
        }

        event = g_timer_head;
        if (event != 0 && event->expire <= now) {
            g_timer_head = event->next;
            event->next = 0;
        } else {
            event = 0;
        }
        timer_irq_restore(status);

        if (print_uptime) {
            uart_send_string("\n[timer] uptime=");
            uart_send_dec((unsigned long)((now - g_boot_time) / g_timebase_freq));
            uart_send_string(" seconds\n");
        }
        if (event == 0) {
            break;
        }

        /* The event is detached. Callbacks may add/stop timers or be
         * interrupted by higher-priority work without exposing half a list.
         */
        event->callback(event->arg);
        if (event->cleanup != 0) {
            event->cleanup(event->arg);
        }
        kfree(event);
        fired_any = 1;
    }

    /* Once the entire interrupt path is done, redraw the REPL prompt. Doing
     * it here instead of inside the callback avoids printing ">" before the
     * allocator free trace emitted by kfree(event).
     */
    if (fired_any != 0 && !demo_nested_active() && !uart_stress_active()) {
        shell_redraw_prompt_delayed();
    }

    status = timer_irq_save();
    if (g_timer_enabled) {
        timer_reprogram_locked();
        asm volatile("csrs sie, %0" : : "r"(SIE_STIE) : "memory");
    }
    timer_irq_restore(status);
    return 0;
}

void timer_irq_isr(void)
{
    timer_irq_top();
}

void timer_stop(void)
{
    uint64_t sstatus;
    struct timer_event *pending;

    sstatus = timer_irq_save();
    timer_source_mask();
    g_timer_enabled = 0;
    irq_task_cancel(&g_timer_task);
    pending = g_timer_head;
    g_timer_head = 0;
    g_next_deadline = 0;
    g_sched_tick_interval = 0;
    g_sched_tick_deadline = 0;
    g_periodic_demo_deadline = 0;
    (void)sbi_set_timer(~0ULL);
    timer_irq_restore(sstatus);

    /* Pending payloads belong to the event until firing or cancellation.
     * The currently executing event is already detached and frees itself.
     */
    while (pending != 0) {
        struct timer_event *event = pending;
        pending = event->next;
        if (event->cleanup != 0) {
            event->cleanup(event->arg);
        }
        kfree(event);
    }

    uart_send_string("[timer] stopped\n");
}

unsigned int timer_pending_count(void)
{
    uint64_t status = timer_irq_save();
    unsigned int count = 0;
    struct timer_event *cur = g_timer_head;

    while (cur != 0) {
        count++;
        cur = cur->next;
    }
    timer_irq_restore(status);
    return count;
}

void timer_dump_queue(void)
{
    uint64_t status = timer_irq_save();
    uint64_t now = timer_read();
    struct timer_event *cur = g_timer_head;
    unsigned int idx = 0;

    uart_send_string("timer pending=");
    uart_send_dec(timer_pending_count());
    uart_send_string(" next=");
    uart_send_dec(g_next_deadline);
    uart_send_string(" now=");
    uart_send_dec(now);
    uart_send_string(" irq_count=");
    uart_send_dec(g_timer_irq_count);
    uart_send_string("\n");

    /* This is a debug/teaching view of the pending list, not part of the
     * timer subsystem itself.
     */
    while (cur != 0) {
        uart_send_string("timer[");
        uart_send_dec(idx++);
        uart_send_string("] expire=");
        uart_send_dec(cur->expire);
        uart_send_string(" delta_ticks=");
        uart_send_dec(cur->expire - now);
        uart_send_string("\n");
        cur = cur->next;
    }
    timer_irq_restore(status);
}
