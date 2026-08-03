// Implementation of attach.h. Reuses src/tetrisu/net.c's adapter (connect,
// handshake, send, poll) rather than a second copy of the wire protocol --
// both directories are owned, and tetrisu already proved this API works
// for exactly this JOIN/START/STATE/LEAVE sequence.

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>

#include "attach.h"
#include "net.h"

// Redraws every board currently known, plain text (no ncurses, so this
// stays inside the console's own terminal mode instead of switching in and
// out of curses on every attach/detach). "[]" for a filled cell, " ." for
// empty, matching the visual convention tetrisu/main.c and the replay
// viewer already use.
static void draw_boards(const net_ctx *ctx, const char *room){
    printf("\x1b[2J\x1b[H");   // clear screen, cursor home
    printf("attached: room %s as %s (states %ld) -- press any key to detach\n\n",
           room, ctx->player_id, ctx->states);

    for (int i = 0; i < ctx->nboards; i++){
        const board_t *b = &ctx->boards[i];
        printf("%s%-12s score %-6u level %-3u lines %-4u%s\n",
               strcmp(b->id, ctx->player_id) == 0 ? "> " : "  ",
               b->id, b->score, b->level, b->lines, b->over ? "  GAME OVER" : "");

        int rows = b->rows_filled < NET_VIEW_ROWS ? b->rows_filled : NET_VIEW_ROWS;
        for (int y = 0; y < rows; y++){
            printf("  ");
            for (int x = 0; x < NET_VIEW_COLS; x++){
                char c = b->cells[y][x];
                printf("%s", (c != '.' && c != '\0') ? "[]" : " .");
            }
            printf("\n");
        }
        printf("\n");
    }
    fflush(stdout);
}

// joins a room as a live viewer and streams its boards until any key
// detaches. connect -> JOIN -> START -> select() over stdin + socket ->
// LEAVE. full contract and the seat-occupying caveat are in attach.h.
int attach_run(const Config *cfg, const char *room){
    // game-plane connection: TCP + tetrissh handshake, same as tetrisu
    net_ctx ctx;
    if (net_connect_and_handshake(cfg, &ctx) != 0){
        printf("attach: could not connect/handshake with tetrisd\n");
        net_close(&ctx);
        return 1;
    }

    char path[80];
    snprintf(path, sizeof path, "/room/%s", room);

    // JOIN the room (tetrisd creates it if it doesn't exist yet); 200/201
    // both mean we're in
    if (net_send_action(&ctx, HTTTP_METHOD_JOIN, path, NULL) != 0){
        printf("attach: JOIN send failed\n");
        net_close(&ctx);
        return 1;
    }
    htttp_msg_t msg;
    char keep[4096];
    int st = net_await_response(&ctx, &msg, keep, sizeof keep);
    if (st != 200 && st != 201){
        printf("attach: JOIN refused by the server (status %d)\n", st);
        net_close(&ctx);
        return 1;
    }

    // the JOIN response's Player-Id header is the identity the server just
    // assigned us; every later request must carry it
    size_t vlen = 0;
    const char *v = htttp_find_header(&msg, "Player-Id", &vlen);
    if (v == NULL || vlen == 0 || vlen >= sizeof ctx.player_id){
        printf("attach: server issued no Player-Id\n");
        net_close(&ctx);
        return 1;
    }
    memcpy(ctx.player_id, v, vlen);
    ctx.player_id[vlen] = '\0';

    // Start the room if nobody has yet. 409 (already started) is the normal
    // case for attaching to a game already in progress -- not a failure.
    if (net_send_action(&ctx, HTTTP_METHOD_START, path, NULL) != 0){
        printf("attach: START send failed\n");
        net_close(&ctx);
        return 1;
    }
    st = net_await_response(&ctx, &msg, keep, sizeof keep);
    if (st != 200 && st != 409)
        printf("attach: START refused by the server (status %d), watching anyway\n", st);

    printf("attached to room %s as %s -- press any key to detach\n", room, ctx.player_id);

    // the watch loop: block in select() until either a frame arrives on
    // the game socket (redraw) or the user presses a key (detach)
    int lost = 0;    // connection to tetrisd known dead: no LEAVE round-trip
    for (;;){
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(ctx.sock, &rfds);
        int maxfd = ctx.sock > STDIN_FILENO ? ctx.sock : STDIN_FILENO;

        int r = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (r < 0){
            if (errno == EINTR) continue;
            printf("attach: select: %s\n", strerror(errno));
            break;
        }

        if (FD_ISSET(ctx.sock, &rfds)){
            long prev_states = ctx.states;
            if (net_poll_state(&ctx) != 0){
                printf("\nattach: connection to tetrisd was lost\n");
                lost = 1;
                break;
            }
            if (ctx.states != prev_states) draw_boards(&ctx, room);
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)){
            char drain[256];
            ssize_t n = read(STDIN_FILENO, drain, sizeof drain);
            (void)n;    // best-effort: the bytes themselves are just the detach signal
            break;
        }
    }

    // A clean detach LEAVEs so the seat frees immediately. On a lost
    // connection both calls would just block against a dead socket (up to
    // the 5s receive timeout, with the console apparently frozen) before
    // failing -- tetrisd reaps the seat on disconnect anyway, so skip them.
    if (!lost){
        net_send_action(&ctx, HTTTP_METHOD_LEAVE, path, NULL);
        net_await_response(&ctx, &msg, keep, sizeof keep);   // best-effort ack
    }
    net_close(&ctx);
    printf("\ndetached from room %s\n", room);
    return 0;
}
