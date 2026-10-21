#ifndef DEMO_UART_H
#define DEMO_UART_H

#include <stdint.h>

int demo_uart_stress_start(uint64_t count);
int demo_uart_stress_active(void);
int demo_uart_stress_note_timer(unsigned long id);

#endif
