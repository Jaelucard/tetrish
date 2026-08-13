// Implementation of the tetrisu <-> tetrisd wire adapter. See net.h for the
// API contract. This file owns the socket, the tetrissh session, and the
// most recently parsed board state; main.c never touches libtetrissh or
// libhtttp directly.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "net.h"

// opens the TCP connection to tetrisd, tunes the socket, and runs the
// tetrissh client handshake. after this the session is encrypted and every
// other net_* call just moves frames over it.
int net_connect_and_handshake(const Config *cfg, net_ctx *ctx){
    memset(ctx, 0, sizeof *ctx);
    ctx->sock = -1;

    // plain TCP connect to the address/port from the rc file
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)cfg->listen_port);
    if (inet_pton(AF_INET, cfg->bind_addr, &sa.sin_addr) != 1){
        close(sock);
        return -1;
    }
    if (connect(sock, (struct sockaddr *)&sa, sizeof sa) < 0){
        close(sock);
        return -1;
    }

    // Nagle would hold a small keypress packet until the previous one was
    // acknowledged. Combined with the peer's delayed-ACK timer that is a
    // stall of tens of milliseconds, longer than an entire frame, and it
    // reads to the player as input lag. Redis sets this unconditionally on
    // every connection and does not even offer a switch to turn it off.
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    // The socket stays blocking because libtetrissh needs it that way. These
    // bound how long a half-delivered frame can hold up the caller.
    struct timeval to = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);

    // the crypto handshake: verifies the server cert against ca_path and
    // agrees on session keys. on failure the caller reads the reason via
    // tetrissh_strerror and then net_close()s.
    ctx->sock = sock;
    ctx->sess = tetrissh_session_alloc();
    if (ctx->sess == NULL) return -1;
    if (tetrissh_handshake_client(ctx->sess, sock, cfg->ca_path) != 0) return -1;

    return 0;
}

// builds one HTTTP request (Player-Id stamped once known, Content-Type on
// bodied commands), serialises it, and sends it as one encrypted frame.
int net_send_action(net_ctx *ctx, htttp_method_t m, const char *path, const char *body){
    htttp_builder_t b;
    htttp_builder_init_request(&b, m, path);
    if (ctx->player_id[0]) htttp_builder_add_header(&b, "Player-Id", ctx->player_id);
    if (body){
        htttp_builder_add_header(&b, "Content-Type", "application/tetris-command");
        htttp_builder_set_body(&b, (const unsigned char *)body, strlen(body));
    }
    size_t wlen = 0;
    char *wire = htttp_serialise(&b, &wlen);
    if (wire == NULL) return -1;
    int rc = tetrissh_send(ctx->sess, ctx->sock, (unsigned char *)wire, wlen);
    free(wire);
    return rc;
}

// blocks until an HTTTP RESPONSE frame arrives and returns its status.
// used only during the opening JOIN/START exchange, where the answer has
// to be known before continuing; STATE broadcasts that arrive first are
// simply discarded.
int net_await_response(net_ctx *ctx, htttp_msg_t *msg, char *keep, size_t keep_sz){
    for (;;){
        size_t plen = 0;
        unsigned char *plain = tetrissh_recv(ctx->sess, ctx->sock, &plen);
        if (plain == NULL) return -1;
        // responses start with the protocol tag; anything else is a broadcast
        if (plen >= 6 && memcmp(plain, "HTTTP/", 6) == 0){
            // copy into the caller's buffer first: htttp_msg_t points into
            // the parsed text, so the text must outlive this call
            if (plen >= keep_sz) plen = keep_sz - 1;
            memcpy(keep, plain, plen);
            free(plain);
            if (htttp_parse_response(keep, plen, msg) != HTTTP_OK) return -1;
            return (int)msg->status;
        }
        free(plain);                 // a STATE broadcast arriving early
    }
}

// Parse a STATE body into the board table.
//
// The body is one block per player:
//
//   player p7 score 0 level 1 lines 0 over 0
//   ..........          <- NET_VIEW_ROWS rows of NET_VIEW_COLS chars
//   ..........             '.' is empty, a digit is a filled cell
//
// Everything here arrives from the network, so every index is bounded before
// it is used and a malformed block is dropped rather than trusted. The rule
// being followed is that no buffer size, loop bound or offset is ever taken
// from a value the peer supplied.
static void parse_state(net_ctx *ctx, const char *body, size_t len){
    int n = 0;
    board_t tmp[NET_MAX_BOARDS];
    memset(tmp, 0, sizeof tmp);

    const char *p = body, *end = body + len;
    while (p < end && n < NET_MAX_BOARDS){
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t linelen = nl ? (size_t)(nl - p) : (size_t)(end - p);

        if (linelen > 7 && memcmp(p, "player ", 7) == 0){
            char line[256];
            size_t cp = linelen < sizeof line - 1 ? linelen : sizeof line - 1;
            memcpy(line, p, cp);
            line[cp] = '\0';
            n++;
            board_t *b = &tmp[n - 1];
            // -1 BEFORE the parse, not after: the memset above zeroes these,
            // and 0 is a real piece type (the O piece). A header that stops
            // early -- an older server, a truncated line -- would otherwise
            // leave the previews reading as "O" and draw two blocks that do
            // not exist. -1 is the only value that means "nothing to draw".
            b->next = b->hold = -1;
            b->held = 0;
            // A short read here just leaves the tail at those defaults; the
            // block is still usable for drawing, which matters more than
            // rejecting it.
            sscanf(line, "player %31s score %u level %u lines %u over %d "
                         "next %d hold %d held %d",
                   b->id, &b->score, &b->level, &b->lines, &b->over,
                   &b->next, &b->hold, &b->held);
            // Everything above came off the network, so the previews are
            // clamped to the piece-type range before anything indexes
            // tb_positions with them.
            if (b->next < 0 || b->next >= TB_NUM_PIECES) b->next = -1;
            if (b->hold < 0 || b->hold >= TB_NUM_PIECES) b->hold = -1;
        } else if (n > 0 && linelen > 0){
            board_t *b = &tmp[n - 1];
            if (b->rows_filled < NET_VIEW_ROWS){
                size_t cols = linelen < NET_VIEW_COLS ? linelen : NET_VIEW_COLS;
                for (size_t x = 0; x < cols; x++)
                    b->cells[b->rows_filled][x] = p[x];
                b->rows_filled++;
            }
            // Extra rows beyond NET_VIEW_ROWS are discarded rather than
            // written past the end of the array. A server that sent a
            // taller board would be a bug, not a licence to overflow.
        }
        if (nl == NULL) break;
        p = nl + 1;
    }

    if (n > 0){ memcpy(ctx->boards, tmp, sizeof tmp); ctx->nboards = n; ctx->states++; }
}

// reads exactly one frame the caller's select() said was ready and files
// it away: a response updates last_status/acks, a STATE broadcast replaces
// the board table. the render loop then draws from ctx without ever
// touching the wire itself.
int net_poll_state(net_ctx *ctx){
    size_t plen = 0;
    unsigned char *plain = tetrissh_recv(ctx->sess, ctx->sock, &plen);
    if (plain == NULL) return -1;              // closed, timed out, or bad frame

    // same tag test as net_await_response: response or broadcast?
    if (plen >= 6 && memcmp(plain, "HTTTP/", 6) == 0){
        char keep[2048];
        size_t cp = plen < sizeof keep - 1 ? plen : sizeof keep - 1;
        memcpy(keep, plain, cp);
        htttp_msg_t msg;
        if (htttp_parse_response(keep, cp, &msg) == HTTTP_OK)
            ctx->last_status = (int)msg.status;
        ctx->acks++;
    } else {
        htttp_msg_t st;
        if (htttp_parse_request((const char *)plain, plen, &st) == HTTTP_OK &&
            st.method == HTTTP_METHOD_STATE && st.body != NULL)
            parse_state(ctx, (const char *)st.body, st.body_len);
    }
    free(plain);
    return 0;
}

// tears the session and socket down. safe on a half-initialised ctx, so
// every error path can call it unconditionally.
void net_close(net_ctx *ctx){
    if (ctx->sess){
        tetrissh_close(ctx->sess);
        tetrissh_session_free(ctx->sess);
        ctx->sess = NULL;
    }
    if (ctx->sock >= 0){
        close(ctx->sock);
        ctx->sock = -1;
    }
}
