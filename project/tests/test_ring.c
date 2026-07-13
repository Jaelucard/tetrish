// A unit test for the log ring buffer. This test uses only one thread, so it
// checks that the buffer behaves correctly on its own, not that the locking
// works under real concurrency (the stress test covers that part).

// run this with `make test`. It checks several things in order: that
// records come out in the same order they went in, that the ring drops and
// counts a record once it is full, that draining in batches works, that the
// positions wrap around correctly, and that a record which is too long gets
// cut short instead of overflowing.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ring.h"

int main(void){
    static Ring r;                      // static because the ring is about 256 KiB, which is large for the stack
    assert(ring_init(&r) == 0);

    // Test 1. First in, first out. The records should come back out in the
    // same order that we put them in.
    assert(ring_push(&r, "one", 3) == 0);
    assert(ring_push(&r, "two", 3) == 0);
    assert(ring_push(&r, "three", 5) == 0);
    char out[8][RING_REC_MAX];
    size_t lens[8];
    size_t n = ring_pop_batch(&r, out, lens, 8);
    assert(n == 3);
    assert(strcmp(out[0], "one")   == 0 && lens[0] == 3);
    assert(strcmp(out[1], "two")   == 0 && lens[1] == 3);
    assert(strcmp(out[2], "three") == 0 && lens[2] == 5);

    // Test 2. Fill the ring right up to its limit. The next record after that
    // should be dropped and added to the drop counter, while every record we
    // already accepted stays safe.
    for (size_t i = 0; i < RING_CAP; i++)
        assert(ring_push(&r, "x", 1) == 0);
    assert(ring_push(&r, "overflow", 8) == -1);
    assert(ring_dropped(&r) == 1);

    // Test 3. Take everything back out in small batches until the ring is
    // empty. The total number of records we get back must be exactly RING_CAP.
    size_t total = 0;
    while ((n = ring_pop_batch(&r, out, lens, 8)) > 0)
        total += n;
    assert(total == RING_CAP);

    // Test 4. Wrap-around. By now `head` has moved well into the array, so the
    // next records have to wrap past the end. The order should still be correct.
    assert(ring_push(&r, "a", 1) == 0);
    assert(ring_push(&r, "b", 1) == 0);
    n = ring_pop_batch(&r, out, lens, 8);
    assert(n == 2);
    assert(strcmp(out[0], "a") == 0 && strcmp(out[1], "b") == 0);

    // Test 5. A record that is bigger than a slot should be cut down to
    // RING_REC_MAX minus 1 bytes, and it must never be written past the slot.
    char big[RING_REC_MAX * 2];
    memset(big, 'A', sizeof big);
    assert(ring_push(&r, big, sizeof big) == 0);
    n = ring_pop_batch(&r, out, lens, 1);
    assert(n == 1 && lens[0] == RING_REC_MAX - 1);

    ring_destroy(&r);
    printf("test_ring: ALL PASS\n");
    return 0;
}
