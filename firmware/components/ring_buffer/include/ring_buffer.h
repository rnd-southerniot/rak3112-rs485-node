/*
 * ring_buffer.h — byte FIFO ring buffer (pure C, no SDK deps → host-testable).
 *
 * Single-producer/single-consumer friendly: rb_push() from one context, rb_pop()
 * from another, with external synchronization for the shared struct. Storage is
 * caller-owned (static pool, no malloc — firmware domain CLAUDE.md §4).
 *
 * Used by the RS-485 RX staging path (Phase 4) and reusable for later framing work.
 */
#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t *buf; /* caller-owned storage, >= cap bytes */
    size_t cap;   /* usable capacity in bytes */
    size_t head;  /* next write index */
    size_t tail;  /* next read index */
    size_t count; /* bytes currently stored (0..cap) */
} ring_buffer_t;

/* Bind rb to caller storage of `cap` bytes and clear it. */
void rb_init(ring_buffer_t *rb, uint8_t *storage, size_t cap);

/* Drop all contents (keeps storage binding). */
void rb_reset(ring_buffer_t *rb);

size_t rb_count(const ring_buffer_t *rb);
size_t rb_free(const ring_buffer_t *rb);
bool rb_is_empty(const ring_buffer_t *rb);
bool rb_is_full(const ring_buffer_t *rb);

/* Push up to `len` bytes; returns the number actually stored (< len if it filled). */
size_t rb_push(ring_buffer_t *rb, const uint8_t *data, size_t len);

/* Pop up to `len` bytes into `out`; returns the number actually removed. */
size_t rb_pop(ring_buffer_t *rb, uint8_t *out, size_t len);

#endif /* RING_BUFFER_H */
