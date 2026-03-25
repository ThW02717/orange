#include "ringbuf.h"

void ringbuf_init(struct ringbuf *rb)
{
    if (rb == 0) {
        return;
    }

    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
}

int ringbuf_push(struct ringbuf *rb, char c)
{
    if (rb == 0 || ringbuf_is_full(rb)) {
        return -1;
    }

    rb->data[rb->head] = c;
    rb->head = (rb->head + 1U) % RINGBUF_CAPACITY;
    rb->count++;
    return 0;
}

int ringbuf_pop(struct ringbuf *rb, char *out)
{
    if (rb == 0 || out == 0 || ringbuf_is_empty(rb)) {
        return -1;
    }

    *out = rb->data[rb->tail];
    rb->tail = (rb->tail + 1U) % RINGBUF_CAPACITY;
    rb->count--;
    return 0;
}

int ringbuf_is_empty(const struct ringbuf *rb)
{
    return rb == 0 || rb->count == 0U;
}

int ringbuf_is_full(const struct ringbuf *rb)
{
    return rb != 0 && rb->count == RINGBUF_CAPACITY;
}
