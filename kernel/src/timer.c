/*
 * Software timers: add_timer() inserts a deadline; timer_irq_top() defers it.
 * timer_work_run() runs due callbacks and rearms the shared SBI timer.
 */
#include "irqflags.h"
#include "timer.h"
#include "trace.h"
#include "csr.h"
#include "uart.h"
#include "memory.h"
#include "deferred_work.h"

/*
 * Shared timer subsystem state.
 * - g_next_deadline is the earliest scheduler tick or software-timer deadline
 * - g_uptime_seconds is updated from the hardware time counter on each IRQ
 */
uint32_t g_timebase_freq;
uint64_t g_boot_time;
uint64_t g_next_deadline;
uint64_t g_uptime_seconds;
uint64_t g_timer_irq_count;

/*
 * One pending software timer event.
 * - expire: absolute rdtime tick when this event becomes runnable
 * - callback/arg: deferred work payload
 * - next: linkage for the sorted singly linked pending list
 */
struct timer_event {
    uint64_t expire;
    timer_callback_t callback;
    timer_callback_t cleanup;
    void *arg;
    struct timer_event *next;
};

static struct timer_event *g_timer_head = 0;
static int g_timer_enabled = 0;
static uint64_t g_sched_tick_interval = 0;
static uint64_t g_sched_tick_deadline = 0;
static int timer_work_run(struct deferred_work *task);

static struct deferred_work g_timer_work = {
    .type = DEFERRED_WORK_TIMER,
    .prio = 0,
    .state = DEFERRED_WORK_IDLE,
    .next = 0,
    .run = timer_work_run,
};
/*
 * Invariants for the software-timer queue:
 * 1. g_timer_head points to a list sorted by expire, smallest first.
 * 2. The hardware deadline is the earliest scheduler tick or pending event.
 */

/* rdtime returns the monotonically increasing platform time counter. */
uint64_t timer_read(void)
{
    uint64_t value;

    asm volatile("rdtime %0" : "=r"(value));
    return value;
}

/*
 * Enable/disable only the hardware timer interrupt source. The software queue
 * can still exist while the source is masked.
 */
void timer_source_mask(void)
{
    asm volatile("csrc sie, %0" : : "r"(SIE_STIE) : "memory");
}

void timer_source_unmask(void)
{
    asm volatile("csrs sie, %0" : : "r"(SIE_STIE) : "memory");
    local_irq_enable();
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
static struct sbiret timer_program_abs(uint64_t deadline)
{
    g_next_deadline = deadline;
    return sbi_set_timer(deadline);
}

/*
 * One SBI timer serves the scheduler and all software events. Caller holds
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

    /* Share the one-shot hardware timer with a 20 Hz scheduling deadline. */
    sstatus = local_irq_save();
    g_timer_enabled = 1;
    g_sched_tick_interval = (g_timebase_freq >= 20U) ? ((uint64_t)g_timebase_freq / 20ULL) : 1ULL;
    g_sched_tick_deadline = timer_read() + g_sched_tick_interval;
    timer_reprogram_locked();
    local_irq_restore(sstatus);

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

    /* Insert and reprogram together so an IRQ never sees half an update. */
    sstatus = local_irq_save();
    if (!g_timer_enabled) {
        local_irq_restore(sstatus);
        kfree(event);
        return -1;
    }

    /*
     * Sorted insert by absolute expire so the queue head is always the next
     * event that hardware must wake up for.
     */
    link = &g_timer_head;
    while (*link != 0 && (*link)->expire <= event->expire) {
        link = &(*link)->next;
    }
    event->next = *link;
    *link = event;

    timer_reprogram_locked();
    local_irq_restore(sstatus);
    return 0;
}

void timer_irq_top(void)
{
    timer_source_mask();
    /* Do not walk the timeout list while entering the interrupt. */
    trace_record(TRACE_TIMER_TOP, 0U);
    (void)deferred_work_enqueue(&g_timer_work);
}

static int timer_work_run(struct deferred_work *task)
{
    uint64_t now;
    uint64_t status;

    (void)task;
    if (!g_timer_enabled) {
        return 0;
    }
    trace_record(TRACE_TIMER_BOTTOM, timer_pending_count());

    if (g_timebase_freq == 0) {
        timer_source_unmask();
        return 0;
    }

    g_timer_irq_count++;
    for (;;) {
        struct timer_event *event;

        status = local_irq_save();
        if (!g_timer_enabled) {
            local_irq_restore(status);
            break;
        }
        now = timer_read();
        timer_sync_uptime(now);
        while (g_sched_tick_interval != 0 && g_sched_tick_deadline <= now) {
            g_sched_tick_deadline += g_sched_tick_interval;
        }

        event = g_timer_head;
        if (event != 0 && event->expire <= now) {
            g_timer_head = event->next;
            event->next = 0;
        } else {
            event = 0;
        }
        local_irq_restore(status);

        if (event == 0) {
            break;
        }

        /* Detach first: the callback may add or cancel other timers. */
        event->callback(event->arg);
        if (event->cleanup != 0) {
            event->cleanup(event->arg);
        }
        kfree(event);
    }

    status = local_irq_save();
    if (g_timer_enabled) {
        timer_reprogram_locked();
        asm volatile("csrs sie, %0" : : "r"(SIE_STIE) : "memory");
    }
    local_irq_restore(status);
    return 0;
}

void timer_stop(void)
{
    uint64_t sstatus;
    struct timer_event *pending;

    sstatus = local_irq_save();
    timer_source_mask();
    g_timer_enabled = 0;
    deferred_work_cancel(&g_timer_work);
    pending = g_timer_head;
    g_timer_head = 0;
    g_next_deadline = 0;
    g_sched_tick_interval = 0;
    g_sched_tick_deadline = 0;
    (void)sbi_set_timer(~0ULL);
    local_irq_restore(sstatus);

    /*
     * Pending payloads belong to the event until firing or cancellation.
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
    uint64_t status = local_irq_save();
    unsigned int count = 0;
    struct timer_event *cur = g_timer_head;

    while (cur != 0) {
        count++;
        cur = cur->next;
    }
    local_irq_restore(status);
    return count;
}

void timer_dump_queue(void)
{
    uint64_t status = local_irq_save();
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

    /*
     * This is a debug/teaching view of the pending list, not part of the
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
    local_irq_restore(status);
}

int timer_is_running(void)
{
    return g_timer_enabled;
}
