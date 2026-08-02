// This file implements the room table. See rooms.h for the full
// explanation of the locking rules.
#include <stdio.h>
#include <string.h>
#include "rooms.h"

static room_t table[ROOM_HARD_MAX];
static int cap_rooms   = 16;   // These two variables hold the runtime limits, which come from .tetrishrc.
static int cap_players = 6;
static int n_rooms     = 0;

void rooms_init(int max_rooms, int max_players_per_room){
    // Here we clamp the runtime limits so they can never be bigger than the
    // compile-time array sizes. This way, a typo in the config file, such
    // as setting max_rooms to 999, cannot make later code read or write
    // past the end of an array.
    cap_rooms   = (max_rooms   > 0 && max_rooms   <= ROOM_HARD_MAX)    ? max_rooms   : ROOM_HARD_MAX;
    cap_players = (max_players_per_room > 0 &&
                   max_players_per_room <= ROOM_MAX_PLAYERS)           ? max_players_per_room : ROOM_MAX_PLAYERS;
    memset(table, 0, sizeof table);
    for (int i = 0; i < ROOM_HARD_MAX; i++){
        table[i].timer_fd = -1;
        pthread_mutex_init(&table[i].mu, NULL);
    }
    n_rooms = 0;
}

room_t *room_find(const char *id){
    for (int i = 0; i < cap_rooms; i++)
        if (table[i].active && strcmp(table[i].id, id) == 0)
            return &table[i];
    return NULL;
}

int room_index(const room_t *r){
    return (int)(r - table);
}

room_t *room_at(int index){
    if (index < 0 || index >= cap_rooms || !table[index].active)
        return NULL;
    return &table[index];
}

room_t *room_by_timerfd(int tfd){
    for (int i = 0; i < cap_rooms; i++)
        if (table[i].active && table[i].timer_fd == tfd)
            return &table[i];
    return NULL;      // This is a plain linear scan over the table. That is fine because max_rooms is small, at most 32, by design.
}

room_t *room_join(const char *id, client_t *c, int *status){
    room_t *r = room_find(id);

    if (r == NULL){
        // The protocol has no separate CREATE method, so JOIN creates the
        // room automatically the first time someone tries to join it. We
        // let the client tell which case happened by returning 201 when a
        // new room was created, and 200 when the client joined a room that
        // already existed.
        for (int i = 0; i < cap_rooms; i++){
            if (!table[i].active){
                r = &table[i];
                break;
            }
        }
        if (r == NULL){ *status = 500; return NULL; }   // We get here if every slot in the room table is already in use.

        pthread_mutex_lock(&r->mu);
        r->active   = 1;
        snprintf(r->id, sizeof r->id, "%s", id);
        r->started  = 0;
        r->timer_fd = -1;
        r->ticks    = 0;
        r->nplayers = 0;
        memset(r->players, 0, sizeof r->players);
        pthread_mutex_unlock(&r->mu);
        n_rooms++;
        *status = 201;                                  // This means a new room was created.
    } else {
        if (r->started){ *status = 409; return NULL; }  // We do not allow a client to join a game that has already started.
        *status = 200;                                  // This means the client is joining a room that already existed.
    }

    pthread_mutex_lock(&r->mu);
    int seat = -1;
    for (int i = 0; i < cap_players; i++){
        if (r->players[i] == NULL){ seat = i; break; }
    }
    if (seat < 0){
        pthread_mutex_unlock(&r->mu);
        *status = 409;                                  // This means the room has no empty seats left.
        return NULL;
    }
    r->players[seat] = c;
    r->pending[seat] = TB_INPUT_NONE;
    r->nplayers++;
    pthread_mutex_unlock(&r->mu);

    c->room = room_index(r);
    c->seat = seat;
    return r;
}

int room_leave(client_t *c){
    room_t *r = room_at(c->room);
    if (r == NULL || c->seat < 0)
        return -1;

    pthread_mutex_lock(&r->mu);
    if (r->players[c->seat] == c){
        r->players[c->seat] = NULL;
        r->nplayers--;
    }
    int empty = (r->nplayers == 0);
    pthread_mutex_unlock(&r->mu);

    c->room = -1;
    c->seat = -1;

    if (!empty)
        return -1;

    // If the last player has just left, we destroy the room and hand the
    // now-orphaned timerfd back to the caller. We do this because removing
    // it from epoll and closing it are jobs for the event loop, not for
    // this data-structure module.
    int orphan_tfd = r->timer_fd;
    r->active   = 0;
    r->started  = 0;
    r->timer_fd = -1;
    n_rooms--;
    return orphan_tfd;
}

int rooms_count(void){
    return n_rooms;
}

void rooms_foreach(void (*fn)(room_t *r, void *arg), void *arg){
    for (int i = 0; i < cap_rooms; i++)
        if (table[i].active)
            fn(&table[i], arg);
}

room_t *room_pick_garbage_target(const room_t *exclude, uint32_t r){
    // Collect every room that could take a hit, then pick one. Building the
    // list first means every eligible room has an equal chance; picking by
    // scanning and stopping early would bias the choice toward low indices.
    room_t *eligible[ROOM_HARD_MAX];
    int n = 0;
    for (int i = 0; i < cap_rooms; i++){
        if (!table[i].active)      continue;   // empty slot
        if (!table[i].started)     continue;   // no game running, nothing to bury
        if (&table[i] == exclude)  continue;   // never send garbage to yourself
        eligible[n++] = &table[i];
    }
    if (n == 0) return NULL;                   // solo game, or nobody else started

    // We deliberately do NOT take each room's lock to check whether its players
    // are still alive. That would mean holding two room locks at once, which is
    // the one shape that could deadlock. Instead the injection site skips any
    // seat whose game is already over, so aiming at a dead room is harmless -
    // it just wastes one message.
    return eligible[r % (uint32_t)n];
}
