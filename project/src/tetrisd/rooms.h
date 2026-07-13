// Room data structure (Week 6, day 4-5).
//
// A room owns its id, up to ROOM_MAX_PLAYERS seated clients, one
// authoritative libtetrisbrain game per seat, one pending input per seat,
// and, once START has been received, a timerfd that drives the tick.
//
// Why each room has a mutex, and when it is held.
// Right now, every room access happens on the single epoll dispatch thread.
// That includes JOIN, LEAVE, and START coming from the client handlers, game
// advancement coming from the timerfd handler, and reads coming from the
// tetrisctl handlers. Since only one thread ever touches this data, the
// mutex is never actually contended, and the program would behave exactly
// the same today if we deleted it. We still keep it for two reasons. First,
// the design contract for this project says that every piece of shared
// state has to name its lock and explain the rules for using it, so the
// mutex documents that intent even though nothing is racing yet. Second, it
// prepares the code for the two realistic ways a second thread could touch
// room state later: a state-dump or metrics thread, or a thread that handles
// handshakes off the main loop. The rule is simple: the mutex is held around
// any read or write of the per-seat arrays, players[], games[], and
// pending[] (this happens in room_join, room_leave, the tick handler, and
// the ctl snapshot helpers), and it is never held across a slow, blocking
// system call. In particular, we never hold it across tetrissh_send. The
// tick handler copies and renders the state while holding the lock, and only
// sends the data after unlocking. This is the same discipline used by the
// log ring: copy the data while holding the lock, then do the actual
// input/output work outside the lock.
//
// Lock order note for the README. The system now has the ring buffer's
// mutex, plus one mutex per room. No code path ever holds both of these at
// the same time, because ring_push is only ever called from outside a room
// lock's critical section in this design. Even if that changed later, and
// logging happened while a room lock was held, it still could not deadlock,
// because ring_push only ever uses trylock and never blocks waiting for the
// ring mutex. We still write the order down for clarity: a room lock is
// always taken before the ring lock, never the other way around.
#ifndef ROOMS_H
#define ROOMS_H

#include <stdint.h>
#include <pthread.h>
#include "clients.h"
#include "tetrisbrain.h"

#define ROOM_ID_MAX      32
#define ROOM_MAX_PLAYERS 8     // This is the compile-time seat limit. The
                               // actual runtime cap is cfg->max_players_per_room,
                               // and it is checked to make sure it never
                               // exceeds this value.
#define ROOM_HARD_MAX    32    // This is the compile-time room limit. The
                               // actual runtime cap is cfg->max_rooms, and
                               // it is checked to make sure it never
                               // exceeds this value.

typedef struct room {
    int  active;                     // Set to 1 if this slot holds a real room, 0 if the slot is free.
    char id[ROOM_ID_MAX];            // The room's name, taken from the JOIN path /room/<id>.
    int  started;                    // Set to 1 once START has been accepted and the ticker is running.
    int  timer_fd;                   // This room's timerfd. It is -1 until the room starts, and the main loop owns it.
    uint64_t ticks;                  // How many ticks have happened since START.
    pthread_mutex_t mu;              // The per-room lock. See the big comment above for what it protects and when to hold it.
    int  nplayers;
    client_t *players[ROOM_MAX_PLAYERS];   // One entry per seat. NULL means that seat is empty.
    tb_game   games[ROOM_MAX_PLAYERS];     // The authoritative game state for each seat.
    tb_input  pending[ROOM_MAX_PLAYERS];   // The input queued for each seat's next tick. A new input replaces the old one.
} room_t;

// This sets the runtime limits, which come from .tetrishrc, and resets the
// whole room table back to empty.
void rooms_init(int max_rooms, int max_players_per_room);

room_t *room_find(const char *id);
int     room_index(const room_t *r);
room_t *room_at(int index);              // Returns NULL if the index is out of range or the slot is not active.
room_t *room_by_timerfd(int tfd);        // Looks up a room by its timerfd. This is a reverse lookup that the epoll loop uses.

// This handles a JOIN: it finds the room if one already exists with this id,
// or creates a new one if it does not, and then seats the client in an open
// slot. It writes the HTTTP status code to return into *status: 201 if the
// room was just created, 200 if the client joined an existing room, 409 if
// the room is full or the game has already started, and 500 if the whole
// room table is full. On success it returns the room, with *status set to
// 200 or 201. On failure it returns NULL, with *status explaining why.
room_t *room_join(const char *id, client_t *c, int *status);

// This handles a LEAVE, or a client disconnecting: it removes the client
// from its seat in its room. If that client was the last player, the room
// is destroyed, and this function returns the room's now-orphaned timerfd
// so the caller can remove it from epoll and close it. If the room still
// has players left, or the client was not seated in a room at all, this
// function returns -1.
int room_leave(client_t *c);

int rooms_count(void);
void rooms_foreach(void (*fn)(room_t *r, void *arg), void *arg);

#endif
