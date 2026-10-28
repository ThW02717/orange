#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include "sbi.h"

/* Software timers execute deferred work by remembering which callback should
 * run later plus one opaque argument passed back to that callback.
 */
typedef void (*timer_callback_t)(void *arg);

/* Minimal timer subsystem state used by early bring-up and later interrupt
 * handling.
 * - timebase_frequency: platform timer ticks per second from DT
 * - boot_time: raw timer value used as the uptime baseline
 * - next_deadline: absolute timer deadline currently programmed via SBI
 * - uptime_seconds: uptime already converted into seconds for logging
 */
extern uint32_t timebase_frequency;
extern uint64_t boot_time;
extern uint64_t next_deadline;
extern uint64_t uptime_seconds;
extern uint64_t timer_irq_count;

/* Read the raw platform counter used as the kernel clocksource. */
uint64_t timer_read(void);

/* Initialize timer bookkeeping once DT has told us the platform frequency. */
void timer_init_state(uint32_t timebase_freq);


void timer_init(void);
void timer_irq_top(void);
void timer_run_bottom_half(void);
void timer_source_mask(void);
void timer_source_unmask(void);
void timer_stop(void);
/* Queue one one-shot software timer relative to "now".
 * The subsystem stores absolute expire times internally and keeps the pending
 * list sorted by that absolute deadline.
 */
int add_timer(timer_callback_t callback, void *arg, uint64_t duration_ticks);
/* On success, cleanup(arg) runs after the callback or when timer_stop drops
 * the event. On failure the caller still owns arg. NULL cleanup is allowed.
 */
int add_timer_with_cleanup(timer_callback_t callback, void *arg,
                           uint64_t duration_ticks, timer_callback_t cleanup);
int timer_is_running(void);
unsigned int timer_pending_count(void);
void timer_dump_queue(void);
#endif
