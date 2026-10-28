#ifndef CUSTOM_WORK_H
#define CUSTOM_WORK_H

#define CUSTOM_WORK_CAPACITY 32U

/* FIFO, one-shot callbacks. Caller keeps arg alive until the callback returns.
 * Return -1 for a null callback or a full pool. Callbacks must not sleep,
 * schedule, or wait for another bottom half.
 */
int defer_callback(void (*callback)(void *), void *arg);
void custom_work_init(void);
void run_custom_callbacks(void);

#endif
