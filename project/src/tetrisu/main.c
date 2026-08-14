// tetrisu is the player's client: an ncurses terminal that talks to tetrisd.
//
// THE ARCHITECTURE, IN ONE LINE: the client is a terminal, not a game.
//
// The server is authoritative. It owns gravity, collision, line clears and
// scoring, and it broadcasts the finished board as text at tick_hz. This
// program has exactly two jobs: draw what it is told, and send keystrokes. It
// deliberately does not link libtetrisbrain and holds no game state of its own,
// so there is no prediction, no rollback, and no way for the display to
// disagree with the server about what happened.
//
// THE LOOP
//
// One select() watches three descriptors and its timeout is the frame pacer:
//
//   STDIN_FILENO  the keyboard
//   sockfd        encrypted frames from tetrisd
//   sigfd         SIGINT, delivered as a readable fd rather than as a handler
//
// Several details here are not obvious and were taken from how Redis and
// ioquake3 drive their own select loops:
//
//  - The fd_set and the timeout are rebuilt from scratch every iteration.
//    select() is destructive: on return the set contains only the ready
//    descriptors and every other bit has been cleared, and on Linux the timeval
//    is overwritten with the unslept remainder. Redis keeps a master set and
//    memcpys a scratch copy for exactly this reason (ae_select.c, "it's not
//    safe to reuse FD sets after select()"), and declares its timeval as a
//    fresh local on every pass. Hoist either out of the loop and the client
//    quietly stops watching the keyboard and then spins at 100% CPU.
//
//  - Both descriptors are tested with independent ifs, never else-if. At 20 Hz
//    a STATE broadcast lands on nearly every frame, so an else-if chain that
//    checks the socket first would swallow most keypresses. redis-cli has that
//    shape and gets away with it only because it exits on any key.
//
//  - EINTR from select() is treated as a frame where nothing happened, not as
//    an error and not as something to retry. That is what Redis does, and it
//    costs at most one short frame.
//
//  - SIGINT arrives on a signalfd rather than through a handler. That removes
//    the whole EINTR problem class, and it means the shutdown path can call
//    endwin() and free memory, none of which is legal inside a real handler.
//    tetrisd uses signalfd for the same reason.
//
// THE SOCKET STAYS BLOCKING, WHICH IS A DELIBERATE EXCEPTION
//
// Everything above argues for a non-blocking socket. This one is blocking,
// because libtetrissh's receive path is an MSG_WAITALL loop that assumes it
// can sit and wait for a whole frame; handing it a non-blocking fd would make
// it fail partway through with EAGAIN. select() only promises the FIRST byte
// has arrived, so a half-delivered frame could otherwise freeze the display.
// SO_RCVTIMEO and SO_SNDTIMEO bound that wait instead, which is the same
// mechanism the server uses on its side of the same library.

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <ncurses.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include "rc.h"
#include "htttp.h"
#include "local.h"
#include "net.h"

#define VIEW_ROWS   NET_VIEW_ROWS
#define VIEW_COLS   NET_VIEW_COLS
#define MAX_BOARDS  NET_MAX_BOARDS
#define CELL_W          2        // two terminal columns per cell, so cells look square
#define BOARD_LEFT      2
#define FRAME_US    16000        // ~60 Hz display against a 20 Hz server

// Horizontal layout, deliberately identical to the offline client's so the
// two modes look the same: your board on the left, the NEXT/HOLD boxes and
// the control list beside it, and -- the one thing offline has no need for --
// the other players' boards after that.
#define BOARD_TOP       1
#define SIDE_LEFT   (BOARD_LEFT + VIEW_COLS * CELL_W + 4)
#define SIDE_W         22        // widest control line ("down     soft drop")
#define OPP_LEFT    (SIDE_LEFT + SIDE_W)

// Vertical budget. A classic terminal is 80x24 and the client must fit one,
// so the mandatory part is: one title row, the top border, VIEW_ROWS of board,
// and the bottom border. That is exactly 23 rows, leaving row 23 for a footer.
// Anything beyond that (per-board names, counters) is drawn only when the
// terminal is actually tall enough, rather than being demanded up front.
#define ROWS_REQUIRED  (VIEW_ROWS + 4)

static net_ctx  g_net;
static int      g_curses_up = 0;

// Registered with atexit BEFORE curses starts, so every exit path restores the
// terminal, including ones that have not been written yet. jserv's tetris does
// this and brenns10's does not, which is why the latter leaves your shell
// unusable if it dies on a small terminal.
static void restore_terminal(void){
    if (g_curses_up){ endwin(); g_curses_up = 0; }
}

// Fail with the terminal already restored, so the message is readable and the
// shell survives. Never call exit() directly once curses owns the screen.
static void die(const char *fmt, const char *arg){
    restore_terminal();
    fprintf(stderr, "tetrisu: ");
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
    exit(1);
}

// --- rendering --------------------------------------------------------------

// draws one board's box-drawing border at the given screen position.
static void draw_frame_at(int top, int left){
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

// A small 4x4 preview box for NEXT/HOLD. type < 0 (an empty hold slot, or a
// next piece the server withheld at a bag boundary) draws the label only.
//
// The shape comes from tb_positions, the same table the engine and the
// offline renderer use, so a preview here is drawn from the piece's real
// orientation-0 geometry rather than a second hand-maintained copy of it.
// Only the type index crosses the wire.
static void draw_piece_box(int top, int left, const char *label, int type, int dim){
    mvprintw(top, left, "%s", label);
    if (type < 0 || type >= TB_NUM_PIECES) return;
    const tb_position *p = &tb_positions[type][0];
    if (dim) attron(A_DIM);
    for (int i = 0; i < TB_CELLS_PER_PIECE; i++){
        int x = p->pos[i].x, y = p->pos[i].y;
        if (x >= 0 && x < 4 && y >= 0 && y < 4)
            mvprintw(top + 1 + y, left + x * CELL_W, "[]");
    }
    if (dim) attroff(A_DIM);
}

// The control list, kept as data so the drawing loop and any future width
// check share one source of truth. This is the offline list minus retry and
// pause: a room ticks for everyone at once, so neither has a meaning the
// server could honour for a single player.
static const char *g_controls[] = {
    "CONTROLS",
    "left/right move",
    "down     soft drop",
    "space    hard drop",
    "up / x   rotate cw",
    "z        rotate ccw",
    "a        rotate 180",
    "c        hold",
    "q        quit",
};

// draws one player's board exactly as the server broadcast it: border,
// cells (coloured by the digit the server sent), name row, score line, and
// a GAME OVER banner when their flag is set. is_me marks our own board
// with a '>' so it stands out among opponents.
static void draw_board(const board_t *b, int top, int left, int is_me){
    draw_frame_at(top, left);
    for (int y = 0; y < VIEW_ROWS; y++)
        for (int x = 0; x < VIEW_COLS; x++){
            int sy = top + 1 + y, sx = left + 1 + x * CELL_W;
            char c = (y < b->rows_filled) ? b->cells[y][x] : '.';
            if (c == '.' || c == '\0'){
                attron(A_DIM); mvprintw(sy, sx, " ."); attroff(A_DIM);
            } else if (c == 'g'){
                // The landing projection the server computed for us. Drawn
                // dim and uncoloured, exactly as the offline renderer draws
                // its locally-computed ghost.
                attron(A_DIM); mvprintw(sy, sx, "[]"); attroff(A_DIM);
            } else {
                // Colour pair keyed on the cell digit, so the renderer needs no
                // knowledge of piece types at all. Falls back to reverse video
                // on a terminal without colour.
                int pair = (c >= '1' && c <= '7') ? (c - '0') : 8;
                if (has_colors()) attron(COLOR_PAIR(pair));
                else              attron(A_REVERSE);
                mvprintw(sy, sx, "[]");
                if (has_colors()) attroff(COLOR_PAIR(pair));
                else              attroff(A_REVERSE);
            }
        }

    // %-*u pads over whatever was there before, so the numbers never leave a
    // stale trailing digit behind and no erase of the region is needed.
    // The name row sits above the board and only exists on a taller terminal.
    // Our own board is drawn at top == 1 precisely so this is skipped: the
    // title row already names us, and starting a row higher is what lines the
    // board up with the offline layout.
    if (top >= 2)
        mvprintw(top - 1, left, "%s%-12s", is_me ? "> " : "  ", b->id);
    // Opponents only. Our own score goes in the full-width footer draw_all
    // writes, on this exact row -- drawing both would put two score lines on
    // top of each other and leave whichever ran last showing.
    int scoreline = top + VIEW_ROWS + 2;
    if (!is_me && scoreline < LINES)
        mvprintw(scoreline, left, " %-6u %-5u", b->score, b->lines);
    // The banner over our own board is drawn by draw_all instead, in the
    // offline client's two-line form. This is the compact one for opponents.
    if (b->over && !is_me){
        attron(A_BOLD);
        mvprintw(top + VIEW_ROWS / 2, left + 3, " GAME OVER ");
        attroff(A_BOLD);
    }
}

// redraws the whole frame in the offline client's layout: title row, YOUR
// board with its NEXT/HOLD previews and control list beside it, a score
// footer, and then the other players' boards to the right of all that.
// everything drawn here comes from net_ctx -- this client holds no game state
// of its own, so the previews and the ghost are read off the wire rather than
// computed.
static void draw_all(const net_ctx *net, const char *room, int connected){
    erase();

    mvprintw(0, BOARD_LEFT, "tetriSH %s as %s  states %ld  acks %ld%s",
             room, net->player_id, net->states, net->acks,
             connected ? "" : "  [DISCONNECTED]");

    // Which board is ours. -1 until the first STATE frame arrives (and, for a
    // spectator, for the whole session), which is why every use of it below
    // is guarded rather than assumed.
    int me = -1;
    for (int i = 0; i < net->nboards; i++)
        if (strcmp(net->boards[i].id, net->player_id) == 0){ me = i; break; }

    if (me >= 0){
        const board_t *b = &net->boards[me];
        // No name row above our own board: the title row already says who we
        // are, and leaving it off puts the board at the same height offline
        // draws it.
        draw_board(b, BOARD_TOP, BOARD_LEFT, 1);

        if (SIDE_LEFT + SIDE_W <= COLS){
            draw_piece_box(BOARD_TOP,     SIDE_LEFT, "NEXT", b->next, 0);
            // Dimmed once hold is spent, so the box shows both what is in
            // there and that it cannot be swapped again until the next lock.
            draw_piece_box(BOARD_TOP + 6, SIDE_LEFT, "HOLD", b->hold, b->held);

            for (size_t i = 0; i < sizeof g_controls / sizeof g_controls[0]; i++){
                int row = BOARD_TOP + 12 + (int)i;
                if (row >= LINES) break;
                if (i > 0) attron(A_DIM);
                mvprintw(row, SIDE_LEFT, "%s", g_controls[i]);
                if (i > 0) attroff(A_DIM);
            }
        }

        // Unpadded, character for character the offline footer: draw_all
        // erases the screen at the top of every frame, so there is no stale
        // text underneath for a field width to have to cover.
        int footer = BOARD_TOP + VIEW_ROWS + 2;
        if (footer < LINES)
            mvprintw(footer, BOARD_LEFT,
                     "Score: %u  |  Lines: %u  |  Level: %u",
                     b->score, b->lines, b->level);

        if (b->over){
            int my = BOARD_TOP + VIEW_ROWS / 2;
            attron(A_BOLD);
            mvprintw(my,     BOARD_LEFT + 2, "    GAME OVER     ");
            mvprintw(my + 1, BOARD_LEFT + 2, "      q quit      ");
            attroff(A_BOLD);
        }
    }

    // The opponents, in wire order with our own board skipped, starting past
    // the sidebar. Each gets a name row and a compact score line; the loop
    // stops at the screen edge rather than drawing off it.
    int per = VIEW_COLS * CELL_W + 4;
    int col = 0;
    for (int i = 0; i < net->nboards; i++){
        if (i == me) continue;
        int left = OPP_LEFT + col * per;
        if (left + per > COLS) break;
        draw_board(&net->boards[i], BOARD_TOP + 1, left, 0);
        col++;
    }

    // One doupdate per frame rather than a refresh per element, so the screen
    // updates atomically and never tears between boards.
    wnoutrefresh(stdscr);
    doupdate();
}

// --- main -------------------------------------------------------------------

// the networked client's whole life: parse arguments (dispatching --local
// and --spectate first), take SIGINT/SIGTERM as a signalfd, connect and
// handshake, JOIN the room and learn our Player-Id, START (409 = someone
// already did = fine), then bring up curses and run the select() loop --
// keyboard to requests, socket frames to the screen -- until quit or
// disconnect, tearing down curses cleanly at the end.
int main(int argc, char **argv){
    if (argc < 2){
        fprintf(stderr,
                "usage: %s <rc-file> [room]\n"
                "       %s --local [seed] [start-level]\n"
                "       %s --spectate <rc-file> [room]\n"
                "  connects to tetrisd, joins a room, and plays it in the terminal;\n"
                "  --local instead plays a full game against an in-process brain, no\n"
                "  server required; --spectate is not yet supported by the server\n"
                "  (see REQUIRED_FILES.md)\n",
                argv[0], argv[0], argv[0]);
        return 2;
    }

    // --local never touches the network: it runs the same brain tetrisd runs,
    // in-process, for offline play and testing. Kept behind a flag so the
    // normal path stays server-authoritative, per the week-7 requirement.
    if (strcmp(argv[1], "--local") == 0){
        uint32_t seed  = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 0;
        uint32_t level = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 10) : 0;
        return tetrisu_local_run(seed, level);
    }

    // todo: requires tetrisd spectate join mode (REQUIRED_FILES.md). JOIN
    // always deals the caller a board today; there is no way to watch a room
    // without occupying a seat in it. Parsed and reported to the boundary
    // rather than silently falling through to a normal (participating) JOIN.
    if (strcmp(argv[1], "--spectate") == 0){
        fprintf(stderr,
                "tetrisu: --spectate requires server spectate support, "
                "which tetrisd does not yet have (see REQUIRED_FILES.md)\n");
        return 2;
    }

    const char *rc_path = argv[1];
    const char *room    = (argc > 2) ? argv[2] : "demo";

    Config cfg;
    if (rc_load(rc_path, &cfg) != 0)
        die("failed to load configuration from %s", rc_path);

    // Block SIGINT and take it as a readable descriptor instead. This has to
    // happen before anything else can be interrupted.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) die("sigprocmask: %s", strerror(errno));
    int sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigfd < 0) die("signalfd: %s", strerror(errno));

    if (net_connect_and_handshake(&cfg, &g_net) != 0){
        // Copy the reason out BEFORE net_close: tetrissh_strerror returns a
        // pointer into the session object ("valid until the next call on
        // sess", tetrissh.h), and net_close both overwrites that buffer
        // (tetrissh_close writes "session closed" into it) and then frees it.
        char why[256];
        snprintf(why, sizeof why, "%s",
                 g_net.sess ? tetrissh_strerror(g_net.sess) : strerror(errno));
        net_close(&g_net);
        die("could not connect/handshake: %s", why);
    }

    char path[64], keep[4096];
    htttp_msg_t msg;

    snprintf(path, sizeof path, "/room/%s", room);
    if (net_send_action(&g_net, HTTTP_METHOD_JOIN, path, NULL) != 0) die("JOIN send failed", "");
    int st = net_await_response(&g_net, &msg, keep, sizeof keep);
    if (st != 200 && st != 201){
        char what[64];
        snprintf(what, sizeof what, "status %d", st);
        die("JOIN refused by the server: %s", what);
    }

    size_t vlen = 0;
    const char *v = htttp_find_header(&msg, "Player-Id", &vlen);
    if (v == NULL || vlen == 0 || vlen >= sizeof g_net.player_id)
        die("server issued no Player-Id", "");
    memcpy(g_net.player_id, v, vlen);
    g_net.player_id[vlen] = '\0';

    // 409 means somebody already started this room, which is success from our
    // point of view: the ticker we needed is already running.
    if (net_send_action(&g_net, HTTTP_METHOD_START, path, NULL) != 0) die("START send failed", "");
    st = net_await_response(&g_net, &msg, keep, sizeof keep);
    if (st != 200 && st != 409) die("START refused by the server", "");

    // Curses comes up only now, so every failure above prints normally.
    setlocale(LC_ALL, "");
    atexit(restore_terminal);
    initscr();
    g_curses_up = 1;
    cbreak(); noecho(); keypad(stdscr, TRUE); nodelay(stdscr, TRUE);
    set_escdelay(0); curs_set(0);
    if (has_colors()){
        start_color();
        use_default_colors();
        short fg[9] = { 0, COLOR_CYAN, COLOR_BLUE, COLOR_WHITE, COLOR_YELLOW,
                        COLOR_GREEN, COLOR_MAGENTA, COLOR_RED, COLOR_WHITE };
        for (int i = 1; i <= 8; i++) init_pair((short)i, fg[i], -1);
    }
    if (LINES < ROWS_REQUIRED || COLS < VIEW_COLS * CELL_W + 6){
        char need[64];
        snprintf(need, sizeof need, "%d rows by %d columns, this one is %d by %d",
                 ROWS_REQUIRED, VIEW_COLS * CELL_W + 6, LINES, COLS);
        die("terminal too small: needs %s", need);
    }

    int running = 1, connected = 1;
    while (running){
        // Rebuilt every iteration. select() clears every bit that was not
        // ready, and on Linux it also overwrites the timeout with the unslept
        // remainder, so neither may be hoisted out of this loop.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(sigfd, &rfds);
        if (connected) FD_SET(g_net.sock, &rfds);

        int maxfd = sigfd > g_net.sock ? sigfd : g_net.sock;
        if (STDIN_FILENO > maxfd) maxfd = STDIN_FILENO;

        struct timeval tv = { .tv_sec = 0, .tv_usec = FRAME_US };
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0){
            // A signal beat select() to the punch. Treat it as a frame in which
            // nothing happened rather than retrying or failing: the signalfd
            // will still be readable next time round.
            if (errno == EINTR) continue;
            die("select: %s", strerror(errno));
        }

        // Independent ifs, never else-if. At 20 Hz the socket is ready on
        // almost every frame, and an else-if chain would starve the keyboard.
        // Drained in a loop (the fd is non-blocking): anything on this
        // descriptor is a request to quit.
        if (FD_ISSET(sigfd, &rfds)){
            struct signalfd_siginfo si;
            while (read(sigfd, &si, sizeof si) == (ssize_t)sizeof si)
                running = 0;
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)){
            // Drained in a loop, not once per frame. One getch per frame falls
            // behind a fast typist and leaves keys queued in the tty, which
            // shows up as the piece continuing to move after the key is
            // released.
            int ch;
            while ((ch = getch()) != ERR){
                htttp_method_t m = 0;
                const char *body = NULL;
                switch (ch){
                case KEY_LEFT:  m = HTTTP_METHOD_MOVE;   body = "LEFT";  break;
                case KEY_RIGHT: m = HTTTP_METHOD_MOVE;   body = "RIGHT"; break;
                case KEY_UP:
                case 'x': case 'X':
                                m = HTTTP_METHOD_ROTATE; body = "CW";    break;
                case 'z': case 'Z':
                                m = HTTTP_METHOD_ROTATE; body = "CCW";   break;
                case KEY_DOWN:  m = HTTTP_METHOD_DROP;   body = "SOFT";  break;
                case ' ':       m = HTTTP_METHOD_DROP;   body = "HARD";  break;
                case 'a': case 'A':
                                m = HTTTP_METHOD_ROTATE; body = "180";   break;
                // HOLD is the one action with no body: the method is the
                // whole instruction. It still has to set `body`, because that
                // is what the send below tests to decide a key produced an
                // action at all -- so it sends an empty body rather than none.
                case 'c': case 'C':
                                m = HTTTP_METHOD_HOLD;   body = "";      break;
                // Still no pause or retry: a room ticks for every player at
                // once, so neither has a meaning the server could honour for
                // one seat. Local mode has them.
                case 'q': case 'Q': running = 0; break;
                default: break;                       // ERR and unknown keys
                }
                if (body != NULL && connected){
                    char mpath[96];
                    snprintf(mpath, sizeof mpath, "/room/%s/player/%s",
                             room, g_net.player_id);
                    if (net_send_action(&g_net, m, mpath, body) != 0) connected = 0;
                }
            }
        }

        if (connected && FD_ISSET(g_net.sock, &rfds)){
            if (net_poll_state(&g_net) < 0) connected = 0;
        }

        draw_all(&g_net, room, connected);
    }

    if (connected)
        net_send_action(&g_net, HTTTP_METHOD_LEAVE, path, NULL);
    net_close(&g_net);
    close(sigfd);
    restore_terminal();
    printf("left room %s as %s: %ld state frames, %ld acks\n",
           room, g_net.player_id, g_net.states, g_net.acks);
    return 0;
}
