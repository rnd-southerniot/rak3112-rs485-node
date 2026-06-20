/*
 * Host unit tests for ring_buffer (pure C, no SDK). Compiled and run by ctest.
 * Minimal assert-style harness: each CHECK records a failure; main() exits non-zero
 * if any failed (ctest treats non-zero as test failure).
 */
#include "ring_buffer.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                 \
        }                                                                                          \
    } while (0)

static void test_init_empty(void)
{
    uint8_t store[8];
    ring_buffer_t rb;
    rb_init(&rb, store, sizeof(store));
    CHECK(rb_is_empty(&rb));
    CHECK(!rb_is_full(&rb));
    CHECK(rb_count(&rb) == 0);
    CHECK(rb_free(&rb) == 8);
}

static void test_push_pop_basic(void)
{
    uint8_t store[8];
    ring_buffer_t rb;
    rb_init(&rb, store, sizeof(store));

    const uint8_t in[4] = {1, 2, 3, 4};
    CHECK(rb_push(&rb, in, 4) == 4);
    CHECK(rb_count(&rb) == 4);
    CHECK(rb_free(&rb) == 4);

    uint8_t out[4] = {0};
    CHECK(rb_pop(&rb, out, 4) == 4);
    CHECK(memcmp(in, out, 4) == 0);
    CHECK(rb_is_empty(&rb));
}

static void test_fill_and_overflow(void)
{
    uint8_t store[4];
    ring_buffer_t rb;
    rb_init(&rb, store, sizeof(store));

    const uint8_t in[6] = {10, 20, 30, 40, 50, 60};
    /* Only 4 fit; push reports the truncation. */
    CHECK(rb_push(&rb, in, 6) == 4);
    CHECK(rb_is_full(&rb));
    CHECK(rb_push(&rb, in, 1) == 0); /* full → nothing accepted */

    uint8_t out[4] = {0};
    CHECK(rb_pop(&rb, out, 4) == 4);
    CHECK(out[0] == 10 && out[3] == 40);
}

static void test_wraparound(void)
{
    uint8_t store[4];
    ring_buffer_t rb;
    rb_init(&rb, store, sizeof(store));

    uint8_t tmp[3];
    const uint8_t a[3] = {1, 2, 3};
    CHECK(rb_push(&rb, a, 3) == 3);  /* head=3 */
    CHECK(rb_pop(&rb, tmp, 2) == 2); /* tail=2, count=1 */

    /* Push 3 more → must wrap across the end of storage. */
    const uint8_t b[3] = {7, 8, 9};
    CHECK(rb_push(&rb, b, 3) == 3); /* count=4, head wrapped */
    CHECK(rb_is_full(&rb));

    uint8_t out[4] = {0};
    CHECK(rb_pop(&rb, out, 4) == 4);
    /* Remaining old byte (3) then the three new ones, in FIFO order. */
    CHECK(out[0] == 3 && out[1] == 7 && out[2] == 8 && out[3] == 9);
}

static void test_partial_pop(void)
{
    uint8_t store[8];
    ring_buffer_t rb;
    rb_init(&rb, store, sizeof(store));

    const uint8_t in[5] = {1, 2, 3, 4, 5};
    CHECK(rb_push(&rb, in, 5) == 5);

    uint8_t out[2];
    CHECK(rb_pop(&rb, out, 2) == 2);
    CHECK(out[0] == 1 && out[1] == 2);
    CHECK(rb_count(&rb) == 3);

    uint8_t rest[8];
    CHECK(rb_pop(&rb, rest, 8) == 3); /* only 3 left even though 8 requested */
    CHECK(rest[0] == 3 && rest[2] == 5);
}

static void test_reset(void)
{
    uint8_t store[8];
    ring_buffer_t rb;
    rb_init(&rb, store, sizeof(store));
    const uint8_t in[3] = {9, 9, 9};
    rb_push(&rb, in, 3);
    rb_reset(&rb);
    CHECK(rb_is_empty(&rb));
    CHECK(rb_count(&rb) == 0);
}

int main(void)
{
    test_init_empty();
    test_push_pop_basic();
    test_fill_and_overflow();
    test_wraparound();
    test_partial_pop();
    test_reset();

    printf("ring_buffer: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
