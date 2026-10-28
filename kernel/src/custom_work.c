/* One-shot callbacks share the CUSTOM bit, but keep separate payloads. */
#include "custom_work.h"
#include "bottom_half.h"
#include "irqflags.h"

struct custom_work {
    void (*callback)(void *);
    void *arg;
    struct custom_work *next;
    int allocated;
};

static struct custom_work pool[CUSTOM_WORK_CAPACITY];
static struct custom_work *head;
static struct custom_work *tail;

void custom_work_init(void)
{
    head = tail = 0;
    for (unsigned int i = 0; i < CUSTOM_WORK_CAPACITY; i++)
        pool[i].allocated = 0;
}

int defer_callback(void (*callback)(void *), void *arg)
{
    uint64_t status;

    if (callback == 0)
        return -1;
    status = local_irq_save();
    for (unsigned int i = 0; i < CUSTOM_WORK_CAPACITY; i++) {
        struct custom_work *work = &pool[i];

        if (work->allocated)
            continue;
        work->allocated = 1;
        work->callback = callback;
        work->arg = arg;
        work->next = 0;
        if (tail != 0)
            tail->next = work;
        else
            head = work;
        tail = work;
        raise_bottom_half(BH_CUSTOM);
        local_irq_restore(status);
        return 0;
    }
    local_irq_restore(status);
    return -1;
}

void run_custom_callbacks(void)
{
    uint64_t status = local_irq_save();
    struct custom_work *work = head;

    /* New submissions go to a fresh list, not this batch. */
    head = tail = 0;
    local_irq_restore(status);
    while (work != 0) {
        struct custom_work *next = work->next;

        work->callback(work->arg);
        status = local_irq_save();
        work->allocated = 0;
        local_irq_restore(status);
        work = next;
    }
}
