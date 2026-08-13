// Room data structure.

// A room owns its id, up to ROOM_MAX_PLAYERS seated clients, one
// authoritative libtetrisbrain game per seat, one pending input per seat,
// and, once START has been received, a timerfd that drives the tick.

// Why each room has a mutex, and when it is held.
//
// Right now every room access happens on the single epoll dispatch thread:
// JOIN, LEAVE and START from the client handlers, game advancement from the
// timerfd handler, reads from the tetrisctl handlers. Only one thread ever
// touches this data, so the mutex is never contended and the program would
// behave exactly the same today if we deleted it. It stays for two reasons.
// First, the design contract for this project says every piece of shared state
// names its lock and states the rules for using it, so the mutex documents
// that intent before anything is racing. Second, it prepares for the two
// realistic ways a second thread could touch room state later: a state-dump or
// metrics thread, or handshakes moved off the main loop.
//
// The rule: hold mu around any read or write of the per-seat arrays,
// players[], games[] and pending[]. That means room_join, room_leave, the tick
// handler and the ctl snapshot helpers. Never hold it across a slow blocking
// system call, and never across tetrissh_send in particular. The tick handler
// copies and renders the state under the lock and sends only after unlocking,
// which is the same discipline the log ring uses: copy under the lock, do the
// I/O outside it.

// Lock ordering. The system has the ring buffer's mutex, plus one mutex per
// room. Both ARE held at once, in several places: the tick handler and the
// garbage handler both call slog and rlog while holding a room lock, and so
// does START. The order is always the same one, a room lock first and then the
// ring lock, and never the reverse, because nothing that holds the ring lock
// ever touches a room.
//
// That ordering alone would be enough to rule out deadlock, but it is not what
// the design actually rests on. ring_push takes the ring mutex with trylock and
// gives up immediately if it is busy, so a thread holding a room lock can never
// wait on the ring lock at all. There is no cycle to close, whatever order the
// two are acquired in. (An earlier version of this comment claimed ring_push
// was never called under a room lock. It was wrong on the facts and it was
// resting the safety argument on the weaker of the two reasons.)
//
// The rule that IS load-bearing, and the one to check when adding code: a room
// lock is never held across a slow or blocking system call. Copy and render
// under the lock, then do the input and output after unlocking. The tick
// handler is the worked example, and it is why mq_send and tetrissh_send both
// sit below its unlock rather than above it.
#ifndef ROOMS_H
#define ROOMS_H

#include <stdint.h>
#include <pthread.h>
#include "clients.h"
#include "tetrisbrain.h"

#define ROOM_ID_MAX      32
#define ROOM_MAX_PLAYERS 8     // compile-time seat limit. The runtime cap is
                               // cfg->max_players_per_room, checked so it can
                               // never exceed this.
#define ROOM_MAX_SPECS   4     // Spectator slots per room (tetrish-view --live).
// Compile-time room limit. The runtime cap is cfg->max_rooms, checked so it can
// never exceed this.
//
// Raised from 32 to 128, because 32 rooms times 8 seats capped the whole server
// at 256 players no matter how it was configured. At 128 the ceiling is 1024.
//
// The cost is a static table of room_t, and a room_t is dominated by its eight
// tb_game structs at roughly 300 bytes each, so a room is about 2.5 KB and the
// table about 320 KB. Fair price for the headroom. Worth contrasting with
// Quake 3, whose equivalent per-client state runs to ~120 KB plus a snapshot
// ring costing ~1.6 MB per client slot: scaling that design this way would need
// most of a gigabyte, which is why its own client limit is a hard 64. Our game
// state is small enough that the table is simply not the constraint.
#define ROOM_HARD_MAX    128

typedef struct room {
    int  active;                     // 1 if this slot holds a real room, 0 if it is free
    char id[ROOM_ID_MAX];            // the room's name, taken from the JOIN path /room/<id>
    int  started;                    // 1 once START has been accepted and the ticker is running
    int  timer_fd;                   // this room's timerfd, -1 until START. The main loop owns it.
    int  tick_hz;                    // the rate this room was started at. See below.
    // tick_hz is captured per room, at START, rather than read from g_cfg
    // wherever it is needed. A SIGHUP reload replaces g_cfg while rooms are
    // already running, but a running room keeps the timerfd period it was armed
    // with, so the live config and the room's real tick rate diverge the moment
    // anyone reloads. Lock delay is measured in ticks and has to be converted
    // from half a second using the rate the room is ACTUALLY ticking at: reading
    // the new value from g_cfg gave a player who joined after a reload a lock
    // delay several times longer than everyone else's in the same room.
    uint64_t ticks;                  // ticks since START
    pthread_mutex_t mu;              // the per-room lock. What it covers is in the big comment above.
    int  nplayers;
    client_t *players[ROOM_MAX_PLAYERS];   // one per seat, NULL means empty
    tb_game   games[ROOM_MAX_PLAYERS];     // the authoritative game state per seat
    uint32_t  seeds[ROOM_MAX_PLAYERS];     // the seed each seat's game was tb_init'ed with
    // Kept because the engine only stores the *evolving* rng_state, so once a
    // game starts ticking its initial seed cannot be recovered from tb_game,
    // and the replay log has to re-state it in every SNAPSHOT record. The SEED
    // record on its own is written exactly once, at START, which is the log
    // socket's busiest moment; a dropped SEED used to make the whole session
    // unreplayable.
    tb_input  pending[ROOM_MAX_PLAYERS];   // input queued for each seat's next tick. A new one replaces the old.
    // Spectators: connections watching this room without holding a seat.
    // A spectator receives every STATE broadcast the players do, but owns no
    // game, no pending input, and no say in the room's lifetime: the room is
    // destroyed when its last PLAYER leaves, even if spectators remain (they
    // are sent back to the lobby, room = -1). A spectator is recognisable
    // anywhere by client fields alone: room >= 0 with seat == -1. Protected
    // by mu exactly like the seat arrays.
    client_t *specs[ROOM_MAX_SPECS];
} room_t;

// Sets the runtime limits (from .tetrishrc) and resets the room table.
void rooms_init(int max_rooms, int max_players_per_room);

room_t *room_find(const char *id);
int     room_index(const room_t *r);
room_t *room_at(int index);              // NULL if the index is out of range or the slot is inactive
room_t *room_by_timerfd(int tfd);        // reverse lookup, used by the epoll loop

// Handles a JOIN: finds the room with this id, creates it if there is none,
// then seats the client. Writes the HTTTP status to return into *status: 201
// if the room was just created, 200 if it already existed, 409 if the room is
// full, 500 if the whole room table is full. Returns the room on success and
// NULL on failure, with *status saying why either way.
room_t *room_join(const char *id, client_t *c, int *status);

// Attaches a client to a room as a spectator: no seat, no game, added to the
// STATE fan-out. Never creates the room. JOIN's auto-create exists so the
// first player has somewhere to sit, and a watcher with nothing to watch is an
// error, not a room. NULL on failure, with *status 404 (no such room) or 409
// (every spectator slot taken); the room with *status 200 on success.
room_t *room_spectate(const char *id, client_t *c, int *status);

// Handles a LEAVE, or a disconnect: takes the client out of its seat, or out
// of the spectator slots if it holds no seat. If that was the last player, the
// room is destroyed and this returns its now-orphaned timerfd, so the caller
// can pull it out of epoll and close it. Returns -1 if the room still has
// players, or the client was not in a room, or it was only spectating.
int room_leave(client_t *c);

int rooms_count(void);
void rooms_foreach(void (*fn)(room_t *r, void *arg), void *arg);

// Battle Royale targeting. Picks a room to send garbage to: any active, started
// room other than `exclude`. `r` is a random value supplied by the caller, so
// this function stays free of hidden state and is easy to test. Returns NULL
// when there is no other started room, which is the normal case for a solo
// game. The base rule is "a random other room"; retargeting by score is one of
// the live-extension tasks and is deliberately not implemented here.
room_t *room_pick_garbage_target(const room_t *exclude, uint32_t r);

#endif
