// Per-connection client state.
//
// One client_t per TCP connection that has finished the secure handshake. The
// registry is an array indexed by fd, so a lookup is O(1) and nothing on the
// hot path allocates. Indexing by fd works because epoll already hands us a
// unique fd per connection. A slot lives exactly as long as its connection:
// created right after a successful handshake, destroyed on disconnect.
//
// Identity model: the server picks the player_id at handshake time and ties it
// to that connection. A later request whose Player-Id header does not match
// the id we assigned gets 403. That is what stops a client from driving
// someone else's board by faking a header value.
#ifndef CLIENTS_H
#define CLIENTS_H

#include <stdint.h>
#include <time.h>
#include "tetrissh.h"

#define CLIENT_MAX_FD    4096   // registry bound; an fd at or above this is refused
#define CLIENT_ID_MAX    16

typedef struct client {
    int fd;                          // -1 means this slot is free
    tetrissh_session_t *sess;        // the secure session; this struct owns it and must free it
    char player_id[CLIENT_ID_MAX];   // the id the server assigned, e.g. "p7"
    int room;                        // index into the room table, -1 means the lobby
    int seat;                        // seat inside that room, -1 means no seat (lobby, or a spectator)
    uint32_t addr;                   // peer IPv4, network order, for the per-IP limit
    // Request rate limiting, one fixed one-second window per connection.
    // Going over the budget inside a window gets you a 429, not a disconnect:
    // flooding is usually a buggy or impatient client rather than an attack,
    // and dropping the session would lose a real player's game. See
    // CLIENT_MAX_REQ_PER_SEC in tetrisd/main.c.
    time_t   rl_window;              // start of the current window (seconds)
    int      rl_count;               // requests counted in this window
} client_t;

// Registers a connection that has just finished its handshake. Returns the new
// slot, or NULL if the fd is out of range, in which case the caller should
// close the connection.
client_t *client_add(int fd, tetrissh_session_t *sess, uint32_t addr);

// How many connected clients came from this address. The per-IP limit calls
// this at accept() time, before anything expensive happens. The registry is a
// flat array of CLIENT_MAX_FD slots, so this is a linear scan, and that is
// fine: it runs once per connection attempt, not per request. A hash table
// keyed by address would be more state to keep correct on every disconnect for
// no measurable gain at this scale.
int client_count_addr(uint32_t addr);

// O(1) lookup by fd. NULL if no client is stored there.
client_t *client_get(int fd);

// Frees the session and clears the slot. Does NOT close the fd: the socket
// belongs to the caller, and so does removing it from the epoll set.
void client_remove(int fd);

int client_count(void);

// Calls fn once for every connected client.
void client_foreach(void (*fn)(client_t *c, void *arg), void *arg);

#endif
