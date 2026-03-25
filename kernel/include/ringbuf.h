#ifndef RINGBUF_H
#define RINGBUF_H

#include <stdint.h>

#define RINGBUF_CAPACITY 128U

/* Fixed-size FIFO used by the later UART RX/TX interrupt path.
 * head points to the next write slot, tail points to the next read slot.
 */
struct ringbuf {
    char data[RINGBUF_CAPACITY];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
};

void ringbuf_init(struct ringbuf *rb);
int ringbuf_push(struct ringbuf *rb, char c);
int ringbuf_pop(struct ringbuf *rb, char *out);
int ringbuf_is_empty(const struct ringbuf *rb);
int ringbuf_is_full(const struct ringbuf *rb);

#endif
