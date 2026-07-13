// The log ring buffer. This is protected by mutex

// A ring buffer is a fixed size list that we reuse in a circle. We put log
// records in at one end and take them out at the other end, and when we reach
// the end of the array we wrap back around to the start.

// Two kinds of code use this buffer:
//   The producer is the game code inside tetrisd (the main thread). It calls
//   ring_push() to add a log record.
//   The consumer is the logshipper thread. It calls ring_pop_batch() to take
//   records out, and then it sends them to tetrislogd.

// The locking rule:
//   The producer uses pthread_mutex_trylock to lock it only if it
//   is free right now, otherwise give up immediately. If the lock is busy, or
//   if the ring is already full, we throw the record away and add one to the
//   drop counter. We do this because the game must never wait for logging.
//   The consumer uses a normal pthread_mutex_lock and is allowed to wait,
//   because it has no real time deadline. While it holds the lock it only
//   copies records out of the ring. The slow part, actually sending the
//   records over the socket, happens after the lock is released. This follows
//   the rule that we never hold a mutex while doing a slow system call.

// Because there is only ever one mutex in the whole system, there is no second
// lock that could be taken in the wrong order, so a deadlock cannot happen.
#ifndef RING_H
#define RING_H

#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>

#define RING_CAP     1024   // how many records the ring can hold before it starts dropping
#define RING_REC_MAX 256    // the largest a single record can be, in bytes (longer ones get cut short)

typedef struct {
    char   rec[RING_CAP][RING_REC_MAX]; // the storage for the records themselves
    size_t len[RING_CAP];               // the real length of each stored record
    size_t head;                        // the position of the oldest record, which is the next one we take out
    size_t count;                       // how many records are in the ring right now
    pthread_mutex_t mu;                 // the one mutex that protects this ring (explained in the comment above)
    atomic_ulong dropped;               // a running count of records we had to throw away (lock busy, or ring full)
} Ring;

int    ring_init(Ring *r);                 // sets the ring up, returns 0 if it worked and -1 if it did not
void   ring_destroy(Ring *r);              // cleans the ring up when we are done with it
// Add one record to the ring. Returns 0 if the record was stored, or -1 if we
// had to drop it (the lock was busy, or the ring was full). This function
// never waits, so it is safe to call from the game code that must stay fast.
int    ring_push(Ring *r, const char *msg, size_t n);
// Take up to `max` records out of the ring, oldest first, copying them into
// out[] and their lengths into lens[]. The records are removed from the ring.
// Returns how many records were actually copied. This may wait a short moment
// for the mutex.
size_t ring_pop_batch(Ring *r, char out[][RING_REC_MAX], size_t *lens, size_t max);
// Returns how many records have been dropped in total. Any thread can call
// this at any time, because the counter is atomic and needs no lock.
unsigned long ring_dropped(Ring *r);

#endif
