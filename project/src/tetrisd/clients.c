// The client registry. Design notes are in clients.h.
#include <stdio.h>
#include <string.h>
#include "clients.h"

// The fd-indexed table. A client_t is about 40 bytes, so the whole thing is a
// couple hundred KiB of static memory. Preallocated on purpose: no malloc and
// no free every time a connection opens or closes.
static client_t table[CLIENT_MAX_FD];
static int      n_clients = 0;
static int      initialised = 0;

static void ensure_init(void){
    if (initialised) return;
    for (int i = 0; i < CLIENT_MAX_FD; i++)
        table[i].fd = -1;
    initialised = 1;
}

client_t *client_add(int fd, tetrissh_session_t *sess, uint32_t addr){
    ensure_init();
    if (fd < 0 || fd >= CLIENT_MAX_FD)
        return NULL;
    client_t *c = &table[fd];
    c->fd   = fd;
    c->sess = sess;
    c->room = -1;
    c->seat = -1;
    c->addr = addr;
    c->rl_window = 0;                // first request opens the first window
    c->rl_count  = 0;
    // The identity the server assigns and ties to this connection. The fd is
    // unique among the connections currently alive, which is the only
    // uniqueness we need here.
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
    tetrissh_session_free(c->sess);   // frees the AES key and any EVP/X509 objects left from the handshake
    c->sess = NULL;
    c->fd   = -1;
    n_clients--;
}

int client_count(void){
    return n_clients;
}

int client_count_addr(uint32_t addr){
    ensure_init();
    int n = 0;
    for (int i = 0; i < CLIENT_MAX_FD; i++)
        if (table[i].fd != -1 && table[i].addr == addr)
            n++;
    return n;
}

void client_foreach(void (*fn)(client_t *c, void *arg), void *arg){
    ensure_init();
    for (int i = 0; i < CLIENT_MAX_FD; i++)
        if (table[i].fd != -1)
            fn(&table[i], arg);
}
