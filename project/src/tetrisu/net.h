// Thin adapter layer between tetrisu's render/input loop and the wire
// protocol (tetrissh session + HTTTP messages). Keeping this behind a small
// API means protocol churn stays out of main.c's render code: main.c only
// ever calls net_connect_and_handshake / net_send_action / net_poll_state
// (plus net_await_response for the opening JOIN/START handshake, and
// net_close for teardown).

#ifndef TETRISU_NET_H
#define TETRISU_NET_H

#include <stddef.h>
#include "rc.h"
#include "tetrissh.h"
#include "htttp.h"

#define NET_VIEW_ROWS  20     // must match the server's board, checked on arrival
#define NET_VIEW_COLS  10
#define NET_MAX_BOARDS  8     // one per seat in a room

// One player's board as most recently broadcast. Everything here comes off
// the wire, so nothing in it is trusted until it has been range checked
// (see net_poll_state's parsing).
typedef struct {
    char     id[32];
    unsigned score, level, lines;
    int      over;
    char     cells[NET_VIEW_ROWS][NET_VIEW_COLS];
    int      rows_filled;        // how many rows this block actually supplied
} board_t;

typedef struct {
    tetrissh_session_t *sess;
    int      sock;
    char     player_id[32];
    board_t  boards[NET_MAX_BOARDS];
    int      nboards;
    long     states, acks;
    int      last_status;
} net_ctx;

// Opens a TCP connection to cfg->bind_addr:cfg->listen_port, applies the same
// socket options tetrisd's peers are expected to use (TCP_NODELAY, 5s
// send/recv timeouts), and performs the tetrissh client handshake against
// cfg->ca_path. ctx is zeroed and ctx->sock/ctx->sess populated on success.
// Returns 0 on success, -1 on failure (ctx->sess may be allocated but
// unhandshaked; net_close is always safe to call afterward either way).
int net_connect_and_handshake(const Config *cfg, net_ctx *ctx);

// Sends one HTTTP request over the session, stamping Player-Id (once known,
// i.e. after JOIN) and, when body is non-NULL, Content-Type:
// application/tetris-command. Returns 0 on a successful send, -1 otherwise.
int net_send_action(net_ctx *ctx, htttp_method_t m, const char *path, const char *body);

// Blocks until an HTTTP response frame arrives, discarding any STATE
// broadcasts that arrive first. Used only during the opening JOIN/START
// exchange, before the main loop starts treating sends as fire-and-forget.
// Returns the response status code, or -1 on failure.
int net_await_response(net_ctx *ctx, htttp_msg_t *msg, char *keep, size_t keep_sz);

// Reads exactly one already-available frame (the caller is expected to have
// select()ed on ctx->sock first) and updates ctx: an HTTTP response updates
// last_status/acks, a STATE broadcast updates boards/nboards/states. Returns
// 0 on a frame handled, -1 if the connection is gone (closed, timed out, or
// an unreadable frame).
int net_poll_state(net_ctx *ctx);

// Closes the session and socket. Safe to call even after a failed
// net_connect_and_handshake.
void net_close(net_ctx *ctx);

#endif
