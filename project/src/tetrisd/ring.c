// The code that actually implements the ring buffer. The locking rules are
// explained at the top of ring.h

// The ring is a fixed array that we treat as a circle. `head` is the position
// of the oldest record. A new record goes at position (head + count) % RING_CAP,
// which is the slot just after the newest record. Adding a record makes `count`
// go up by one, and taking a record out moves `head` forward by one. The % (the
// remainder operator) is what makes the position wrap back to the start of the
// array once it reaches the end.

// When count reaches RING_CAP the ring is full. In that case we drop the new
// record and keep all the older ones. We chose this policy because it keeps the
// earliest history and is the easiest one to explain: once a record has been
// accepted, we never lose it.
#include <string.h>
#include "ring.h"

int ring_init(Ring *r){
    r->head  = 0;
    r->count = 0;
    atomic_init(&r->dropped, 0);
    return pthread_mutex_init(&r->mu, NULL) == 0 ? 0 : -1;
}

void ring_destroy(Ring *r){
    pthread_mutex_destroy(&r->mu);
}

int ring_push(Ring *r, const char *msg, size_t n){
    if (n >= RING_REC_MAX)
        n = RING_REC_MAX - 1;               // if the record is too long, cut it short so it fits in the slot

    // Try to take the lock without waiting. If the shipper thread (or some
    // other producer) is holding the mutex at this exact moment, we do not
    // wait for it. Instead we add one to the drop counter and return straight
    // away. This is the main idea of the whole design: it is fine to lose a
    // log record, but it is not fine for the game to fall behind.
    if (pthread_mutex_trylock(&r->mu) != 0){
        atomic_fetch_add(&r->dropped, 1);   // we do not hold the lock here, so we use the atomic counter
        return -1;
    }

    if (r->count == RING_CAP){              // the ring is full, so drop this new record
        pthread_mutex_unlock(&r->mu);
        atomic_fetch_add(&r->dropped, 1);
        return -1;
    }

    // Work out the slot for the new record. It goes just after the newest one,
    // and the % wraps us back to the start of the array if we run off the end.
    size_t tail = (r->head + r->count) % RING_CAP;
    memcpy(r->rec[tail], msg, n);
    r->rec[tail][n] = '\0';                 // add a string terminator so the record is safe to print later
    r->len[tail] = n;
    r->count++;

    pthread_mutex_unlock(&r->mu);
    return 0;
}

size_t ring_pop_batch(Ring *r, char out[][RING_REC_MAX], size_t *lens, size_t max){
    // Here we use a normal lock that is allowed to wait, because the shipper
    // thread has no strict deadline. While we hold the lock we only copy data
    // out, and we never make a slow system call. Keeping the locked part this
    // short is what lets the producer's trylock succeed almost every time.
    pthread_mutex_lock(&r->mu);

    // Take either `max` records, or fewer if the ring does not have that many.
    size_t n = (r->count < max) ? r->count : max;
    for (size_t i = 0; i < n; i++){
        size_t idx = (r->head + i) % RING_CAP;      // step through the records from oldest to newest
        memcpy(out[i], r->rec[idx], r->len[idx] + 1); // the +1 also copies the '\0' at the end of the record
        lens[i] = r->len[idx];
    }
    // Move head forward past the records we just took, wrapping if needed.
    r->head  = (r->head + n) % RING_CAP;
    r->count -= n;

    pthread_mutex_unlock(&r->mu);
    return n;
}

unsigned long ring_dropped(Ring *r){
    return atomic_load(&r->dropped);        // reading an atomic value is safe without taking the mutex
}
