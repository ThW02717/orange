/* UART stress data belongs to the demo, not the UART driver. */
#include "demo_uart.h"
#include "uart.h"
#include "irqflags.h"

static uint64_t uart_stress_next = 0;
static uint64_t uart_stress_remaining = 0;
static unsigned long uart_timer_marker_ids[16];
static unsigned int uart_timer_marker_head = 0U;
static unsigned int uart_timer_marker_tail = 0U;

static void demo_uart_refill(void);

static unsigned int demo_queue_tx_marker(void)
{
    char buf[32];
    unsigned int len = 0;
    uint64_t value;
    char rev[24];
    unsigned int rev_len = 0;
    unsigned int i;
    uint64_t marker;

    if (uart_stress_remaining == 0U) {
        return 0;
    }

    marker = uart_stress_next;

    buf[len++] = '[';
    buf[len++] = 'U';

    value = marker;
    if (value == 0U) {
        rev[rev_len++] = '0';
    } else {
        while (value > 0U && rev_len < sizeof(rev)) {
            rev[rev_len++] = (char)('0' + (value % 10U));
            value /= 10U;
        }
    }

    for (i = 0; i < rev_len; i++) {
        buf[len++] = rev[rev_len - 1U - i];
    }

    buf[len++] = ']';
    if (((marker + 1U) % 8U) == 0U) {
        buf[len++] = '\r';
        buf[len++] = '\n';
    } else {
        buf[len++] = ' ';
    }

    if (uart_write_async(buf, len) != 0) {
        return 0;
    }
    uart_stress_next++;
    uart_stress_remaining--;
    return 1;
}

static int demo_timer_pending(void)
{
    return uart_timer_marker_head != uart_timer_marker_tail;
}

static int demo_queue_timer_marker(unsigned long id)
{
    unsigned int next_tail;
    unsigned int capacity = (unsigned int)(sizeof(uart_timer_marker_ids) /
                                           sizeof(uart_timer_marker_ids[0]));

    next_tail = (uart_timer_marker_tail + 1U) % capacity;
    if (next_tail == uart_timer_marker_head) {
        return -1;
    }

    uart_timer_marker_ids[uart_timer_marker_tail] = id;
    uart_timer_marker_tail = next_tail;
    return 0;
}

static int demo_queue_pending_timer_marker(void)
{
    char buf[32];
    char rev[24];
    unsigned int len = 0;
    unsigned int rev_len = 0;
    unsigned int i;
    unsigned int capacity = (unsigned int)(sizeof(uart_timer_marker_ids) /
                                           sizeof(uart_timer_marker_ids[0]));
    unsigned long id;
    unsigned long value;

    if (!demo_timer_pending()) {
        return 0;
    }

    id = uart_timer_marker_ids[uart_timer_marker_head];

    buf[len++] = '\r';
    buf[len++] = '\n';
    buf[len++] = '[';
    buf[len++] = 'T';

    value = id;
    if (value == 0UL) {
        rev[rev_len++] = '0';
    } else {
        while (value > 0UL && rev_len < sizeof(rev)) {
            rev[rev_len++] = (char)('0' + (value % 10UL));
            value /= 10UL;
        }
    }

    for (i = 0; i < rev_len; i++) {
        buf[len++] = rev[rev_len - 1U - i];
    }

    buf[len++] = ']';
    buf[len++] = '\r';
    buf[len++] = '\n';

    if (uart_write_async(buf, len) != 0) {
        return 0;
    }
    uart_timer_marker_head = (uart_timer_marker_head + 1U) % capacity;
    return 1;
}

static void demo_fill_tx(unsigned int budget_markers)
{
    /* The shell and deferred callbacks share this producer state. */
    uint64_t status = local_irq_save();
    while (budget_markers > 0U) {
        if (demo_timer_pending()) {
            if (demo_queue_pending_timer_marker() == 0U) {
                break;
            }
            continue;
        }
        if (uart_stress_remaining == 0U) {
            break;
        }
        if (demo_queue_tx_marker() == 0U) {
            break;
        }
        budget_markers--;
    }
    local_irq_restore(status);
}

int demo_uart_stress_start(uint64_t count)
{
    uint64_t sstatus;

    if (count == 0U) {
        return -1;
    }

    sstatus = local_irq_save();
    if (uart_stress_remaining != 0U || demo_timer_pending()) {
        local_irq_restore(sstatus);
        return -1;
    }
    if (uart_tx_set_refill(demo_uart_refill) != 0) {
        local_irq_restore(sstatus);
        return -1;
    }
    uart_stress_next = 0;
    uart_stress_remaining = count;
    uart_timer_marker_head = 0U;
    uart_timer_marker_tail = 0U;
    demo_fill_tx(16U);
    local_irq_restore(sstatus);

    return 0;
}

int demo_uart_stress_active(void)
{
    uint64_t sstatus;
    int active;

    sstatus = local_irq_save();
    active = (uart_stress_remaining != 0U) ||
             demo_timer_pending();
    local_irq_restore(sstatus);
    return active;
}

int demo_uart_stress_note_timer(unsigned long id)
{
    uint64_t sstatus;
    int ret;

    sstatus = local_irq_save();
    if (uart_stress_remaining == 0U && !demo_timer_pending()) {
        local_irq_restore(sstatus);
        return -1;
    }
    ret = demo_queue_timer_marker(id);
    if (ret == 0) {
        demo_fill_tx(16U);
    }
    local_irq_restore(sstatus);

    return ret;
}

static void demo_uart_refill(void)
{
    demo_fill_tx(16U);
    if (!demo_uart_stress_active()) {
        (void)uart_tx_set_refill(0);
    }
}

#ifdef KERNEL_TEST
int demo_uart_test_marker(unsigned long id)
{
    if (demo_queue_timer_marker(id) != 0) {
        return -1;
    }
    return demo_queue_pending_timer_marker() ? 0 : -1;
}
#endif
