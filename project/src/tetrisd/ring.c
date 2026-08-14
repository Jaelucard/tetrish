// The ring buffer itself. Locking rules are at the top of ring.h.

// The ring is a fixed array treated as a circle. `head` is the oldest record.
// A new record goes at (head + count) % RING_CAP, the slot just after the
// newest one. A push bumps count, a pop moves head forward, and the % (the
// remainder operator) is what wraps either one back to the start of the array
// once it runs off the end.

// count == RING_CAP means full. We then drop the NEW record and keep the older
// ones, rather than overwriting the oldest. That keeps the earliest history and
// gives one rule that is easy to state: once a record has been accepted, we
// never lose it.
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
        n = RING_REC_MAX - 1;               // too long for a slot, so cut it short

    // Take the lock only if it is free. If the shipper thread (or another
    // producer) holds it at this exact moment we do not wait: count a drop and
    // return. This is the whole idea of the design. Losing a log record is
    // fine, the game falling behind is not.
    if (pthread_mutex_trylock(&r->mu) != 0){
        atomic_fetch_add(&r->dropped, 1);   // no lock held here, hence the atomic
        return -1;
    }

    if (r->count == RING_CAP){              // full: drop the new record, keep the old ones
        pthread_mutex_unlock(&r->mu);
        atomic_fetch_add(&r->dropped, 1);
        return -1;
    }

    size_t tail = (r->head + r->count) % RING_CAP;   // just past the newest, wrapping
    memcpy(r->rec[tail], msg, n);
    r->rec[tail][n] = '\0';                 // terminate it so the record is safe to print later
    r->len[tail] = n;
    r->count++;

    pthread_mutex_unlock(&r->mu);
    return 0;
}

size_t ring_pop_batch(Ring *r, char out[][RING_REC_MAX], size_t *lens, size_t max){
    // A normal lock here, allowed to wait, because the shipper has no deadline.
    // We only copy data out while holding it and make no slow system call.
    // Keeping the locked stretch this short is what lets the producer's trylock
    // succeed almost every time.
    pthread_mutex_lock(&r->mu);

    size_t n = (r->count < max) ? r->count : max;   // fewer if the ring has fewer
    for (size_t i = 0; i < n; i++){
        size_t idx = (r->head + i) % RING_CAP;      // oldest first
        memcpy(out[i], r->rec[idx], r->len[idx] + 1); // the +1 takes the '\0' too
        lens[i] = r->len[idx];
    }
    r->head  = (r->head + n) % RING_CAP;            // past what we just took
    r->count -= n;

    pthread_mutex_unlock(&r->mu);
    return n;
}

unsigned long ring_dropped(Ring *r){
    return atomic_load(&r->dropped);        // atomic, so no mutex needed to read it
}
