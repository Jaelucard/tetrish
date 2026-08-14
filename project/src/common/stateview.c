// Shared STATE-stream client. stateview.h says why this exists and what stays
// out of it. Every function here started life inside tetrisu and moved over
// verbatim, with the file-scope globals swapped for the stateview_t the caller
// owns.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncurses.h>
#include "stateview.h"

int sv_send_request(stateview_t *v, htttp_method_t m, const char *path,
                    const char *body, const char *hdr, const char *hdr_val){
    htttp_builder_t b;
    htttp_builder_init_request(&b, m, path);
    if (v->player_id[0]) htttp_builder_add_header(&b, "Player-Id", v->player_id);
    if (hdr && hdr_val)  htttp_builder_add_header(&b, hdr, hdr_val);
    if (body){
        htttp_builder_add_header(&b, "Content-Type", "application/tetris-command");
        htttp_builder_set_body(&b, (const unsigned char *)body, strlen(body));
    }
    size_t wlen = 0;
    char *wire = htttp_serialise(&b, &wlen);
    if (wire == NULL) return -1;
    int rc = tetrissh_send(v->sess, v->sock, (unsigned char *)wire, wlen);
    free(wire);
    return rc;
}

// Only for the opening handshake, before the frame loop starts. Once the loop
// is running nothing waits for a reply: the authoritative board turns up in
// the next STATE broadcast anyway, so requests are fired and forgotten and
// their acks are read and thrown away.
int sv_read_until_response(stateview_t *v, htttp_msg_t *msg,
                           char *keep, size_t keep_sz){
    for (;;){
        size_t plen = 0;
        unsigned char *plain = tetrissh_recv(v->sess, v->sock, &plen);
        if (plain == NULL) return -1;
        if (plen >= 6 && memcmp(plain, "HTTTP/", 6) == 0){
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
//   ..........          <- VIEW_ROWS rows of VIEW_COLS chars
//   ..........             '.' is empty, a digit is a filled cell
//
// All of this arrives from the network, so every index is bounded before use
// and a malformed block gets dropped rather than trusted. The rule: no buffer
// size, loop bound or offset ever comes from a value the peer supplied.
static void sv_parse_state(stateview_t *v, const char *body, size_t len){
    int n = 0;
    board_t tmp[MAX_BOARDS];
    memset(tmp, 0, sizeof tmp);

    const char *p = body, *end = body + len;
    while (p < end && n < MAX_BOARDS){
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t linelen = nl ? (size_t)(nl - p) : (size_t)(end - p);

        if (linelen > 7 && memcmp(p, "player ", 7) == 0){
            char line[256];
            size_t cp = linelen < sizeof line - 1 ? linelen : sizeof line - 1;
            memcpy(line, p, cp);
            line[cp] = '\0';
            n++;
            board_t *b = &tmp[n - 1];
            // A short scan leaves the tail zeroed. The block still draws, which
            // matters more here than rejecting it.
            sscanf(line, "player %31s score %u level %u lines %u over %d",
                   b->id, &b->score, &b->level, &b->lines, &b->over);
        } else if (n > 0 && linelen > 0){
            board_t *b = &tmp[n - 1];
            if (b->rows_filled < VIEW_ROWS){
                size_t cols = linelen < VIEW_COLS ? linelen : VIEW_COLS;
                for (size_t x = 0; x < cols; x++)
                    b->cells[b->rows_filled][x] = p[x];
                b->rows_filled++;
            }
            // Rows past VIEW_ROWS get dropped, not written past the end of the
            // array. A server sending a taller board is a bug, not a licence
            // to overflow.
        }
        if (nl == NULL) break;
        p = nl + 1;
    }

    if (n > 0){ memcpy(v->boards, tmp, sizeof tmp); v->nboards = n; v->states++; }
}

// Read one frame and classify it. Starts with "HTTTP/" means a response to
// something we sent; anything else is a server-originated STATE request. That
// one memcmp is the entire disambiguation rule, and it is why the client can
// interleave its own requests with unsolicited broadcasts on one connection.
int sv_read_one_frame(stateview_t *v){
    size_t plen = 0;
    unsigned char *plain = tetrissh_recv(v->sess, v->sock, &plen);
    if (plain == NULL) return -1;              // closed, timed out, or bad frame

    if (plen >= 6 && memcmp(plain, "HTTTP/", 6) == 0){
        char keep[2048];
        size_t cp = plen < sizeof keep - 1 ? plen : sizeof keep - 1;
        memcpy(keep, plain, cp);
        htttp_msg_t msg;
        if (htttp_parse_response(keep, cp, &msg) == HTTTP_OK)
            v->last_status = (int)msg.status;
        v->acks++;
    } else {
        htttp_msg_t st;
        if (htttp_parse_request((const char *)plain, plen, &st) == HTTTP_OK &&
            st.method == HTTTP_METHOD_STATE && st.body != NULL)
            sv_parse_state(v, (const char *)st.body, st.body_len);
    }
    free(plain);
    return 0;
}

// --- rendering --------------------------------------------------------------

static void sv_draw_frame_at(int top, int left){
    int w = VIEW_COLS * CELL_W;
    mvaddch(top, left, ACS_ULCORNER);
    mvaddch(top, left + w + 1, ACS_URCORNER);
    mvaddch(top + VIEW_ROWS + 1, left, ACS_LLCORNER);
    mvaddch(top + VIEW_ROWS + 1, left + w + 1, ACS_LRCORNER);
    for (int c = 1; c <= w; c++){
        mvaddch(top, left + c, ACS_HLINE);
        mvaddch(top + VIEW_ROWS + 1, left + c, ACS_HLINE);
    }
    for (int r = 1; r <= VIEW_ROWS; r++){
        mvaddch(top + r, left, ACS_VLINE);
        mvaddch(top + r, left + w + 1, ACS_VLINE);
    }
}

static void sv_draw_board(const board_t *b, int top, int left, int is_me){
    sv_draw_frame_at(top, left);
    for (int y = 0; y < VIEW_ROWS; y++)
        for (int x = 0; x < VIEW_COLS; x++){
            int sy = top + 1 + y, sx = left + 1 + x * CELL_W;
            char c = (y < b->rows_filled) ? b->cells[y][x] : '.';
            if (c == '.' || c == '\0'){
                attron(A_DIM); mvprintw(sy, sx, " ."); attroff(A_DIM);
            } else {
                // Colour pair keyed off the cell digit, so the renderer knows
                // nothing about piece types. Reverse video on a terminal with
                // no colour.
                int pair = (c >= '1' && c <= '7') ? (c - '0') : 8;
                if (has_colors()) attron(COLOR_PAIR(pair));
                else              attron(A_REVERSE);
                mvprintw(sy, sx, "[]");
                if (has_colors()) attroff(COLOR_PAIR(pair));
                else              attroff(A_REVERSE);
            }
        }

    // The %-*u padding writes over whatever was there before, so a shrinking
    // number cannot leave a stale trailing digit and the region never needs
    // erasing. Name row sits above the board, on a tall enough terminal only.
    if (top >= 2)
        mvprintw(top - 1, left, "%s%-12s", is_me ? "> " : "  ", b->id);
    int scoreline = top + VIEW_ROWS + 2;
    if (scoreline < LINES)
        mvprintw(scoreline, left, "%c%-6u %-5u", is_me ? '>' : ' ',
                 b->score, b->lines);
    if (b->over){
        attron(A_BOLD);
        mvprintw(top + VIEW_ROWS / 2, left + 3, " GAME OVER ");
        attroff(A_BOLD);
    }
}

void sv_draw_all(const stateview_t *v, const char *title, const char *footer){
    erase();

    // Lay out upwards from the bottom of the mandatory block: the board always
    // fits, and the optional rows take whatever is left.
    int have_names = (LINES >= VIEW_ROWS + 6);
    int board_top  = have_names ? 2 : 1;      // the top border row

    mvprintw(0, BOARD_LEFT, "%s", title);

    int per = VIEW_COLS * CELL_W + 4;
    for (int i = 0; i < v->nboards; i++){
        int left = BOARD_LEFT + i * per;
        if (left + per > COLS) break;         // never draw off the screen edge
        sv_draw_board(&v->boards[i], board_top, left,
                      strcmp(v->boards[i].id, v->player_id) == 0);
    }

    int f = board_top + VIEW_ROWS + 3;
    if (f < LINES)
        mvprintw(f, BOARD_LEFT, "%s", footer);

    // One doupdate per frame instead of a refresh per element, so the screen
    // lands in one go and never tears between boards.
    wnoutrefresh(stdscr);
    doupdate();
}
