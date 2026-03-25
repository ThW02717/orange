#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include "sbi.h"

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
void timer_handle_interrupt(void);
void timer_stop(void);
#endif
