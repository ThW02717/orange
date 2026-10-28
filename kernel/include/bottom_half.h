#ifndef BOTTOM_HALF_H
#define BOTTOM_HALF_H

/* Bit positions, in dispatch order. BH_COUNT is not a pending bit. */
enum bottom_half {
    BH_TIMER,
    BH_UART_RX,
    BH_UART_TX,
    BH_CUSTOM,
    BH_COUNT,
};

void bottom_half_init(void);
void raise_bottom_half(enum bottom_half type);
/* Clear a future notification; a handler already selected may still run. */
void clear_bottom_half(enum bottom_half type);
int bottom_halves_pending(void);
/* Single-hart runner. Callbacks must not sleep, schedule or wait for work. */
void run_bottom_halves(void);

#endif
