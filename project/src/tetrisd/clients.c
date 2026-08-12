// This file implements the client registry. See clients.h for the design notes.
#include <stdio.h>
#include <string.h>
#include "clients.h"

// This is the fd-indexed table. Each client_t is small (about 40 bytes), so
// the whole table only takes up a couple hundred KiB of static memory.
// Because the table is preallocated like this, we avoid calling malloc and
// free every time a connection opens or closes.
static client_t table[CLIENT_MAX_FD];
static int      n_clients = 0;
static int      initialised = 0;

static void ensure_init(void){
    if (initialised) return;
    for (int i = 0; i < CLIENT_MAX_FD; i++)
        table[i].fd = -1;
    initialised = 1;
}

client_t *client_add(int fd, tetrissh_session_t *sess){
    ensure_init();
    if (fd < 0 || fd >= CLIENT_MAX_FD)
        return NULL;
    client_t *c = &table[fd];
    c->fd   = fd;
    c->sess = sess;
    c->room = -1;
    c->seat = -1;
    c->rl_window_start_ms = 0;
    c->rl_count = 0;
    c->admin_role = 0;
    c->attach_room_id[0] = '\0';
    // This is the identity the server assigns and ties to this connection.
    // The fd is unique among all connections that are currently alive, and
    // that is the only uniqueness guarantee we need here.
    snprintf(c->player_id, sizeof c->player_id, "p%d", fd);
    n_clients++;
    return c;
}

client_t *client_get(int fd){
    ensure_init();
    if (fd < 0 || fd >= CLIENT_MAX_FD || table[fd].fd == -1)
        return NULL;
    return &table[fd];
}

void client_remove(int fd){
    client_t *c = client_get(fd);
    if (c == NULL) return;
    tetrissh_session_free(c->sess);   // This frees the AES key and any leftover EVP or X509 objects from the handshake.
    c->sess = NULL;
    c->fd   = -1;
    n_clients--;
}

int client_count(void){
    return n_clients;
}

void client_foreach(void (*fn)(client_t *c, void *arg), void *arg){
    ensure_init();
    for (int i = 0; i < CLIENT_MAX_FD; i++)
        if (table[i].fd != -1)
            fn(&table[i], arg);
}
