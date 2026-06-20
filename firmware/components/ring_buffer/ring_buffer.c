#include "ring_buffer.h"

#include <string.h>

void rb_init(ring_buffer_t *rb, uint8_t *storage, size_t cap)
{
    rb->buf = storage;
    rb->cap = cap;
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
}

void rb_reset(ring_buffer_t *rb)
{
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
}

size_t rb_count(const ring_buffer_t *rb)
{
    return rb->count;
}

size_t rb_free(const ring_buffer_t *rb)
{
    return rb->cap - rb->count;
}

bool rb_is_empty(const ring_buffer_t *rb)
{
    return rb->count == 0;
}

bool rb_is_full(const ring_buffer_t *rb)
{
    return rb->count == rb->cap;
}

size_t rb_push(ring_buffer_t *rb, const uint8_t *data, size_t len)
{
    size_t space = rb->cap - rb->count;
    if (len > space) {
        len = space;
    }
    if (len == 0) {
        return 0;
    }

    /* Write in up to two contiguous spans across the wrap point. */
    size_t first = rb->cap - rb->head;
    if (first > len) {
        first = len;
    }
    memcpy(rb->buf + rb->head, data, first);
    if (len > first) {
        memcpy(rb->buf, data + first, len - first);
    }

    rb->head = (rb->head + len) % rb->cap;
    rb->count += len;
    return len;
}

size_t rb_pop(ring_buffer_t *rb, uint8_t *out, size_t len)
{
    if (len > rb->count) {
        len = rb->count;
    }
    if (len == 0) {
        return 0;
    }

    size_t first = rb->cap - rb->tail;
    if (first > len) {
        first = len;
    }
    memcpy(out, rb->buf + rb->tail, first);
    if (len > first) {
        memcpy(out + first, rb->buf, len - first);
    }

    rb->tail = (rb->tail + len) % rb->cap;
    rb->count -= len;
    return len;
}
