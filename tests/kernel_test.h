#ifndef KERNEL_TEST_H
#define KERNEL_TEST_H

#include <stdint.h>
struct thread;
void check(int ok, const char *reason);

/* Fixtures only; production builds do not export these hooks. */
struct thread *thread_test_find(int32_t tid);
uint32_t thread_test_next_tid(uint32_t next);
void sched_test_set_current(struct thread *task);
void uart_test_set_irq_enabled(int enabled);
unsigned int uart_test_tx_count(void);
int demo_uart_test_marker(unsigned long id);

#endif
