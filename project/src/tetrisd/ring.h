// The log ring buffer, and the mutex that guards it.

// A ring buffer is a fixed size array used as a circle. Records go in at one
// end and come out at the other, and the index wraps back to the start when it
// runs off the end of the array.

// Two kinds of code touch it. The producer is the game code inside tetrisd
// (the main thread), which calls ring_push(). The consumer is the logshipper
// thread, which calls ring_pop_batch() and then sends the records on to
// tetrislogd.

// The locking rule:
//   The producer uses pthread_mutex_trylock, so it takes the lock only if it
//   is free right now and otherwise gives up immediately. If the lock is busy,
//   or if the ring is already full, we throw the record away and add one to
//   the drop counter. The game must never wait for logging.
//   The consumer uses a normal pthread_mutex_lock and is allowed to wait,
//   because it has no real time deadline. While it holds the lock it only
//   copies records out of the ring. The slow part, actually sending the
//   records over the socket, happens after the lock is released. That is the
//   rule: never hold a mutex while doing a slow system call.

// The trylock is also the deadlock argument. Rooms DO log while holding their
// own lock (see rooms.h), so two mutexes are held at once in places, but
// nothing can ever block waiting on this one, so there is no cycle to close.
#ifndef RING_H
#define RING_H

#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>

#define RING_CAP     1024   // how many records the ring holds before it starts dropping
// Biggest a single record can be, in bytes. Longer ones get cut short.
//
// This was 256 until the replay log arrived. A SNAPSHOT record carries the
// whole visible board as one hex digit per cell, which is TB_ROWS * TB_COLS =
// 20 * 10 = 200 characters, plus the record type, timestamp, tick, room id,
// player id and dimensions. Call it 290 bytes, so 256 silently truncated every
// snapshot and made replay impossible to trust.
//
// The cost is memory: RING_CAP * RING_REC_MAX, so 1024 * 512 = 512 KiB of
// static storage, up from 256 KiB. Fair trade for a daemon, and the ring is
// preallocated precisely so the game path never calls malloc.
//
// Still well inside the AF_UNIX datagram size limit, which is what the
// logshipper's sendto has to fit into.
#define RING_REC_MAX 512

typedef struct {
    char   rec[RING_CAP][RING_REC_MAX];
    size_t len[RING_CAP];               // length of each record, not counting the '\0'
    size_t head;                        // oldest record, so the next one out
    size_t count;                       // records in the ring right now
    pthread_mutex_t mu;                 // guards everything above; rules at the top of this file
    atomic_ulong dropped;               // records thrown away: lock was busy, or ring was full
} Ring;

int    ring_init(Ring *r);                 // 0 ok, -1 if the mutex would not init
void   ring_destroy(Ring *r);              // destroys the mutex
// Add one record. 0 if it was stored, -1 if it was dropped (lock busy, or ring
// full). Never waits, which is the only reason the game path may call it.
int    ring_push(Ring *r, const char *msg, size_t n);
// Take up to `max` records, oldest first, into out[] with their lengths in
// lens[]. The records are removed from the ring. Returns how many were copied.
// May wait a short moment on the mutex.
size_t ring_pop_batch(Ring *r, char out[][RING_REC_MAX], size_t *lens, size_t max);
// Total dropped so far. The counter is atomic, so any thread can read it at
// any time without taking the mutex.
unsigned long ring_dropped(Ring *r);

#endif
