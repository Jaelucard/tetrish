// This file holds the per-connection client state (Week 6, day 1-2).
//
// We keep one client_t for every TCP connection that has finished the secure
// handshake. The registry is stored as an array indexed by fd, so looking up
// a client is O(1) and we never need to allocate memory on the hot path. We
// can use the fd as the index because epoll already hands us a unique fd for
// each connection. A slot's lifetime matches the connection's lifetime: we
// create the slot right after a successful handshake, and we destroy it when
// the client disconnects.
//
// Identity model: the server picks the player_id when the handshake happens
// and ties it to that connection. If a later request carries a Player-Id
// header that does not match the id we assigned to that connection, we
// reject it with 403. This stops a client from controlling another player's
// board just by faking the header value.
#ifndef CLIENTS_H
#define CLIENTS_H

#include "tetrissh.h"

#define CLIENT_MAX_FD    4096   // This is the upper bound for the registry array. Any fd at or above this value is refused.
#define CLIENT_ID_MAX    16

typedef struct client {
    int fd;                          // A value of -1 means this slot is not being used.
    tetrissh_session_t *sess;        // The secure session for this connection. This struct owns it and must free it.
    char player_id[CLIENT_ID_MAX];   // The player id the server assigned to this client, for example "p7".
    int room;                        // The index of this client's room in the room table. A value of -1 means the client is in the lobby.
    int seat;                        // The seat index for this client inside its room. A value of -1 means no seat has been assigned.
} client_t;

// Registers a connection that has just finished its handshake. It returns a
// pointer to the new slot, or NULL if the fd is out of range. If it returns
// NULL, the caller should close the connection.
client_t *client_add(int fd, tetrissh_session_t *sess);

// Looks up a client by fd in constant time (O(1)). Returns NULL if no client is stored at that fd.
client_t *client_get(int fd);

// Frees the session and clears the slot. This function does not close the
// fd. The caller owns the socket, and the caller is also responsible for
// removing it from the epoll set.
void client_remove(int fd);

int client_count(void);

// Calls fn once for every client that is currently connected. It loops over every slot in the table and calls fn on each one that is in use.
void client_foreach(void (*fn)(client_t *c, void *arg), void *arg);

#endif
