// The room table. The locking rules are explained at the top of rooms.h.
#include <stdio.h>
#include <string.h>
#include "rooms.h"

static room_t table[ROOM_HARD_MAX];
static int cap_rooms   = 16;   // the runtime limits, both from .tetrishrc
static int cap_players = 6;
static int n_rooms     = 0;

void rooms_init(int max_rooms, int max_players_per_room){
    // Clamp the runtime limits to the compile-time array sizes, so a typo in
    // the config file (max_rooms 999, say) cannot make later code run off the
    // end of an array.
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
    return NULL;      // plain linear scan; cap_rooms is at most ROOM_HARD_MAX (128), and this runs once per tick per room
}

room_t *room_join(const char *id, client_t *c, int *status){
    room_t *r = room_find(id);

    if (r == NULL){
        // The protocol has no CREATE method, so JOIN creates the room the
        // first time anyone tries to join it. The client tells the two cases
        // apart from the status: 201 for a room we just made, 200 for one that
        // was already there.
        for (int i = 0; i < cap_rooms; i++){
            if (!table[i].active){
                r = &table[i];
                break;
            }
        }
        if (r == NULL){ *status = 500; return NULL; }   // every slot in the table is in use

        pthread_mutex_lock(&r->mu);
        r->active   = 1;
        snprintf(r->id, sizeof r->id, "%s", id);
        r->started  = 0;
        r->timer_fd = -1;
        r->tick_hz  = 0;      // set at START, from the config live at that moment
        r->ticks    = 0;
        r->nplayers = 0;
        memset(r->players, 0, sizeof r->players);
        memset(r->specs,   0, sizeof r->specs);
        pthread_mutex_unlock(&r->mu);
        n_rooms++;
        *status = 201;                                  // created
    } else {
        // Joining a room whose game is already running is allowed.
        //
        // It used to return 409, and that made the server unusable at any real
        // scale: a room seals itself the instant the first player starts it,
        // so with players arriving at the same moment only whoever won the
        // race got a seat. Measured with 100 clients over 13 rooms, 63 were
        // turned away with 409.
        //
        // Letting them in is cheap here only because a STATE broadcast is a
        // complete board, not a delta. A server that delta-compressed its
        // updates would have to hand a late joiner a full baseline first and
        // hold it in a catching-up state until it acknowledged one (which is
        // exactly what Quake 3 does with its CS_PRIMED state and entity
        // baselines). We have no such problem: the next tick after a player
        // sits down hands them the whole board like everybody else.
        //
        // The one thing the caller MUST do is seed this seat's game, because
        // the seeding that normally happens on START has already been and
        // gone. See the join handler in tetrisd.
        *status = 200;                                  // joined one that already existed
    }

    pthread_mutex_lock(&r->mu);
    int seat = -1;
    for (int i = 0; i < cap_players; i++){
        if (r->players[i] == NULL){ seat = i; break; }
    }
    if (seat < 0){
        pthread_mutex_unlock(&r->mu);
        *status = 409;                                  // no empty seats left
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

room_t *room_spectate(const char *id, client_t *c, int *status){
    room_t *r = room_find(id);
    if (r == NULL){
        // Deliberately no auto-create here, unlike room_join. JOIN creates a
        // room so the first player has somewhere to sit; a spectator with
        // nothing to watch is a mistyped room name, and creating an empty
        // room for it would tick nothing and confuse everyone.
        *status = 404;
        return NULL;
    }

    pthread_mutex_lock(&r->mu);
    int slot = -1;
    for (int i = 0; i < ROOM_MAX_SPECS; i++)
        if (r->specs[i] == NULL){ slot = i; break; }
    if (slot < 0){
        pthread_mutex_unlock(&r->mu);
        *status = 409;
        return NULL;
    }
    r->specs[slot] = c;
    pthread_mutex_unlock(&r->mu);

    c->room = room_index(r);
    c->seat = -1;                 // no seat is what MAKES it a spectator
    *status = 200;
    return r;
}

int room_leave(client_t *c){
    room_t *r = room_at(c->room);
    if (r == NULL)
        return -1;

    if (c->seat < 0){
        // A spectator: clear the watch slot and go back to the lobby. No
        // seat means no vote in whether the room lives on, so this can
        // never be the leave that destroys the room.
        pthread_mutex_lock(&r->mu);
        for (int i = 0; i < ROOM_MAX_SPECS; i++)
            if (r->specs[i] == c) r->specs[i] = NULL;
        pthread_mutex_unlock(&r->mu);
        c->room = -1;
        return -1;
    }

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

    // The last player has just left, so destroy the room and hand the
    // now-orphaned timerfd back to the caller. Removing it from epoll and
    // closing it are jobs for the event loop, not for this module.
    //
    // Any spectators still watching go back to the lobby first. Their client_t
    // would otherwise keep pointing at this slot, so the next room created here
    // would inherit watchers it never admitted. Worse, a spectator that
    // disconnected in between would leave a freed pointer in specs[] for the
    // broadcast loop to chase.
    for (int i = 0; i < ROOM_MAX_SPECS; i++)
        if (r->specs[i] != NULL){
            r->specs[i]->room = -1;
            r->specs[i] = NULL;
        }
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
