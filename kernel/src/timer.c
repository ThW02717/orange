#include "timer.h"
#include "demo.h"
#include "trap.h"
#include "uart.h"
#include "memory.h"
#include "shell.h"
#include "irq_task.h"

/* Shared timer subsystem state.
 * - g_next_deadline follows the head of the pending software-timer list
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
static struct irq_task g_timer_task = {
    .type = IRQ_TASK_TIMER,
    .prio = 0,
    .state = IRQ_TASK_IDLE,
    .next = 0,
    .run = timer_task_run,
};
/* Invariants for the software-timer queue:
 * 1. g_timer_head points to a list sorted by expire, smallest first.
 * 2. The programmed hardware deadline always matches g_timer_head->expire
 *    whenever the list is non-empty.
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

    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    asm volatile("csrc sstatus, %0" : : "r"(SSTATUS_SIE));
    return sstatus;
}

static inline void timer_irq_restore(uint64_t sstatus)
{
    asm volatile("csrw sstatus, %0" : : "r"(sstatus));
}

/* Enable/disable only the hardware timer interrupt source. The software queue
 * can still exist while the source is masked.
 */
void timer_source_mask(void)
{
    uint64_t sie;

    asm volatile("csrr %0, sie" : "=r"(sie));
    sie &= ~SIE_STIE;
    asm volatile("csrw sie, %0" : : "r"(sie));
}

void timer_source_unmask(void)
{
    uint64_t sie;
    uint64_t sstatus;

    asm volatile("csrr %0, sie" : "=r"(sie));
    sie |= SIE_STIE;
    asm volatile("csrw sie, %0" : : "r"(sie));

    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    sstatus |= SSTATUS_SIE;
    asm volatile("csrw sstatus, %0" : : "r"(sstatus));
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

/* Reprogram the single hardware timer so it always tracks the earliest
 * pending software timer event. If the queue is empty, park the deadline far
 * away and clear g_next_deadline for observability.
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
}

void timer_init(void)
{
    uint64_t sstatus;

    if (g_timebase_freq == 0) {
        return;
    }

    /* Arm the timer subsystem without inventing a fake periodic event. The
     * first real interrupt will come from whichever software timer becomes the
     * list head next.
     */
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

    expire = timer_read() + duration_ticks;
    event->expire = expire;
    event->callback = callback;
    event->arg = arg;
    event->next = 0;

    /* Critical section begins here.
     *
     * From this point until timer_irq_restore(), we temporarily block
     * interrupts while mutating the pending timer list and the programmed
     * hardware deadline. The queue and the hardware timer must change as one
     * logical update:
     *
     *   1. Insert the new event into the sorted pending list.
     *   2. Recompute which event is now the head.
     *   3. Reprogram hardware so its next deadline matches head->expire.
     *
     * If a timer interrupt were allowed to arrive in the middle of that
     * sequence, the handler could observe a half-updated list or an outdated
     * deadline.
     */
    sstatus = timer_irq_save();

    /* Sorted insert by absolute expire so the queue head is always the next
     * event that hardware must wake up for.
     */
    link = &g_timer_head;
    while (*link != 0 && (*link)->expire <= event->expire) {
        link = &(*link)->next;
    }
    event->next = *link;
    *link = event;

    /* Still inside the same critical section: after the list order may have
     * changed, update the one hardware timer source so it always tracks the
     * new head deadline.
     */
    timer_reprogram_locked();

    /* Critical section ends here. Restore the caller's previous interrupt
     * state now that both the software queue and the hardware deadline are
     * consistent again.
     */
    timer_irq_restore(sstatus);
    return 0;
}

void timer_irq_top(void)
{
    demo_trace_record(DEMO_TRACE_TIMER_TOP, timer_pending_count());
    timer_source_mask();

    if (g_timer_task.state == IRQ_TASK_IDLE) {
        (void)irq_task_enqueue(&g_timer_task);
    }
}

int timer_task_run(struct irq_task *task)
{
    uint64_t now;
    int fired_any = 0;

    (void)task;
    demo_trace_record(DEMO_TRACE_TIMER_BOTTOM, timer_pending_count());

    if (g_timebase_freq == 0) {
        timer_source_unmask();
        return 0;
    }

    now = timer_read();
    timer_sync_uptime(now);
    g_timer_irq_count++;
    while (g_sched_tick_interval != 0 && g_sched_tick_deadline <= now) {
        g_sched_tick_deadline += g_sched_tick_interval;
    }

    /* Timer ISR body: consume every event that is already due at this
     * interrupt edge. This
     * keeps the queue invariant simple: once the loop exits, either the list
     * is empty or the new head has expire > now.
     */
    while (g_timer_head != 0 && g_timer_head->expire <= now) {
        struct timer_event *event = g_timer_head;

        g_timer_head = event->next;
        event->next = 0;
        /* First version: callbacks run synchronously in the timer interrupt
         * path. This keeps the implementation simple and matches the current
         * one-shot software timer exercise.
         */
        event->callback(event->arg);
        kfree(event);
        fired_any = 1;

        now = timer_read();
        timer_sync_uptime(now);
    }

    /* Invariant: after consuming all expired events, the programmed hardware
     * deadline must equal the new list head.
     */
    timer_reprogram_locked();

    /* Once the entire interrupt path is done, redraw the REPL prompt. Doing
     * it here instead of inside the callback avoids printing ">" before the
     * allocator free trace emitted by kfree(event).
     */
    if (fired_any != 0 && !demo_nested_active() && !uart_stress_active()) {
        shell_redraw_prompt_delayed();
    }

    timer_source_unmask();
    return 0;
}

void timer_irq_isr(void)
{
    timer_irq_top();
}

void timer_stop(void)
{
    uint64_t sstatus;

    timer_source_mask();

    sstatus = timer_irq_save();
    g_timer_enabled = 0;
    g_timer_task.state = IRQ_TASK_IDLE;
    g_timer_task.next = 0;
    /* Stop means "drop the whole pending queue" in this first version. */
    while (g_timer_head != 0) {
        struct timer_event *event = g_timer_head;
        g_timer_head = event->next;
        kfree(event);
    }
    g_next_deadline = 0;
    g_sched_tick_interval = 0;
    g_sched_tick_deadline = 0;
    (void)sbi_set_timer(~0ULL);
    timer_irq_restore(sstatus);

    uart_send_string("[timer] stopped\n");
}

unsigned int timer_pending_count(void)
{
    unsigned int count = 0;
    struct timer_event *cur = g_timer_head;

    while (cur != 0) {
        count++;
        cur = cur->next;
    }
    return count;
}

void timer_dump_queue(void)
{
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
}
