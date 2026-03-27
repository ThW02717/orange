#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include "sbi.h"
#include "irq_task.h"

/* Software timers execute deferred work by remembering which callback should
 * run later plus one opaque argument passed back to that callback.
 */
typedef void (*timer_callback_t)(void *arg);

/* One pending software timer event.
 * - expire: absolute rdtime tick when this event becomes runnable
 * - callback/arg: deferred work payload
 * - next: linkage for the sorted singly linked pending list
 */
struct timer_event {
    uint64_t expire;
    timer_callback_t callback;
    void *arg;
    struct timer_event *next;
};

/* Minimal timer subsystem state used by early bring-up and later interrupt
 * handling.
 * - g_timebase_freq: platform timer ticks per second from DT
 * - g_boot_time: raw timer value used as the uptime baseline
 * - g_next_deadline: absolute timer deadline currently programmed via SBI
 * - g_uptime_seconds: uptime already converted into seconds for logging
 */
extern uint32_t g_timebase_freq;
extern uint64_t g_boot_time;
extern uint64_t g_next_deadline;
extern uint64_t g_uptime_seconds;
extern uint64_t g_timer_irq_count;

/* Read the raw platform counter used as the kernel clocksource. */
uint64_t timer_read(void);

/* Initialize timer bookkeeping once DT has told us the platform frequency. */
void timer_init_state(uint32_t timebase_freq);

/* The timer API is split into:
 * - abs: exact future tick value
 * - rel: now + delta_ticks convenience wrapper
 */
struct sbiret timer_program_abs(uint64_t deadline);
struct sbiret timer_program_rel(uint64_t delta_ticks);

void timer_init(void);
void timer_irq_top(void);
int timer_task_run(struct irq_task *task);
void timer_source_mask(void);
void timer_source_unmask(void);
void timer_irq_isr(void);
void timer_stop(void);
/* Queue one one-shot software timer relative to "now".
 * The subsystem stores absolute expire times internally and keeps the pending
 * list sorted by that absolute deadline.
 */
int add_timer(timer_callback_t callback, void *arg, uint64_t duration_ticks);
unsigned int timer_pending_count(void);
void timer_dump_queue(void);
#endif
