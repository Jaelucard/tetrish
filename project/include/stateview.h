// Shared STATE-stream client: what a program needs to consume tetrisd's STATE
// broadcasts over a libtetrissh session and draw the boards they carry.
//
// Two binaries use it, tetrisu (a player) and tetrish-view --live (a
// spectator). Extracted from tetrisu instead of written twice: the 50.003 spec
// says the viewer renders "via the same ncurses renderer as tetrisu", and one
// renderer with two callers is the only arrangement that cannot drift apart.
// Same rule the replay engine takes from Quake 3 (one code path for live and
// recorded play), one layer up: one code path for playing and watching.
//
// What is deliberately NOT here: connection setup, the select() loop, key
// handling, curses init and teardown. Those differ between a player and a
// watcher (different keys, different reconnect policy), so each binary keeps
// its own. This module owns the two things that must never disagree: how a
// frame is classified and parsed, and how a board is drawn.
#ifndef STATEVIEW_H
#define STATEVIEW_H

#include <stddef.h>
#include "tetrissh.h"
#include "htttp.h"

#define VIEW_ROWS      20        // must match the server's board, checked on arrival
#define VIEW_COLS      10
#define MAX_BOARDS      8        // one per seat in a room
#define CELL_W          2        // two terminal columns per cell, so cells look square
#define BOARD_LEFT      2

// One player's board, as most recently broadcast. All of it comes off the
// wire, so nothing here is trusted until it has been range checked.
typedef struct {
    char     id[32];
    unsigned score, level, lines;
    int      over;
    char     cells[VIEW_ROWS][VIEW_COLS];
    int      rows_filled;        // how many rows this block actually supplied
} board_t;

// One connection's worth of STATE consumption. A struct rather than a pile of
// file-scope globals, so each binary holds its own.
typedef struct {
    tetrissh_session_t *sess;
    int      sock;
    char     player_id[32];      // as issued by the server's JOIN response; "" until then
    board_t  boards[MAX_BOARDS];
    int      nboards;
    long     states, acks;
    int      last_status;        // status of the most recent response frame seen
} stateview_t;

// Serialise and send one request on the session. Player-Id goes on by itself
// once v->player_id is set. hdr/hdr_val add one extra header when both are
// non-NULL (the spectator JOIN uses it for X-Spectate); pass NULL, NULL
// otherwise.
int sv_send_request(stateview_t *v, htttp_method_t m, const char *path,
                    const char *body, const char *hdr, const char *hdr_val);

// Read frames until a RESPONSE arrives, discarding STATE broadcasts that
// beat it to the socket. Only for the opening handshake, before the frame
// loop starts; returns the response's status, or -1 on a dead connection.
// keep must outlive msg, whose fields point into it.
int sv_read_until_response(stateview_t *v, htttp_msg_t *msg,
                           char *keep, size_t keep_sz);

// Read exactly one frame and classify it: a response updates last_status
// and acks, a STATE broadcast updates the board table and states. Returns
// -1 when the connection is closed, timed out or delivered a bad frame.
int sv_read_one_frame(stateview_t *v);

// Draw the whole board table plus a caller-supplied title (row 0) and
// footer (below the boards). The board whose id equals v->player_id gets
// highlighted. For a spectator none of them ever match, which is correct.
void sv_draw_all(const stateview_t *v, const char *title, const char *footer);

#endif
