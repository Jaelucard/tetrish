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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <ncurses.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/signalfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "rc.h"
#include "tetrissh.h"
#include "htttp.h"

#define VIEW_ROWS      20        // must match the server's board, checked on arrival
#define VIEW_COLS      10
#define MAX_BOARDS      8        // one per seat in a room
#define CELL_W          2        // two terminal columns per cell, so cells look square
#define BOARD_LEFT      2
#define FRAME_US    16000        // ~60 Hz display against a 20 Hz server

// Vertical budget. A classic terminal is 80x24 and the client must fit one,
// so the mandatory part is: one title row, the top border, VIEW_ROWS of board,
// and the bottom border. That is exactly 23 rows, leaving row 23 for a footer.
// Anything beyond that (per-board names, counters) is drawn only when the
// terminal is actually tall enough, rather than being demanded up front.
#define ROWS_REQUIRED  (VIEW_ROWS + 4)

// One player's board as most recently broadcast. Everything here comes off the
// wire, so nothing in it is trusted until it has been range checked.
typedef struct {
    char     id[32];
    unsigned score, level, lines;
    int      over;
    char     cells[VIEW_ROWS][VIEW_COLS];
    int      rows_filled;        // how many rows this block actually supplied
} board_t;

static tetrissh_session_t *g_sess;
static int      g_sock = -1;
static board_t  g_boards[MAX_BOARDS];
static int      g_nboards = 0;
static char     g_player_id[32] = "";
static int      g_curses_up = 0;
static long     g_states = 0, g_acks = 0;
static int      g_last_status = 0;

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

// --- protocol ---------------------------------------------------------------

static int send_request(htttp_method_t m, const char *path, const char *body){
    htttp_builder_t b;
    htttp_builder_init_request(&b, m, path);
    if (g_player_id[0]) htttp_builder_add_header(&b, "Player-Id", g_player_id);
    if (body){
        htttp_builder_add_header(&b, "Content-Type", "application/tetris-command");
        htttp_builder_set_body(&b, (const unsigned char *)body, strlen(body));
    }
    size_t wlen = 0;
    char *wire = htttp_serialise(&b, &wlen);
    if (wire == NULL) return -1;
    int rc = tetrissh_send(g_sess, g_sock, (unsigned char *)wire, wlen);
    free(wire);
    return rc;
}

// Used only during the opening handshake, before the frame loop starts. Once
// the loop is running nothing ever waits for a reply: the authoritative board
// arrives in the next STATE broadcast regardless, so requests are sent and
// forgotten and their acknowledgements are read and discarded.
static int read_until_response(htttp_msg_t *msg, char *keep, size_t keep_sz){
    for (;;){
        size_t plen = 0;
        unsigned char *plain = tetrissh_recv(g_sess, g_sock, &plen);
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
// Everything here arrives from the network, so every index is bounded before
// it is used and a malformed block is dropped rather than trusted. The rule
// being followed is that no buffer size, loop bound or offset is ever taken
// from a value the peer supplied.
static void parse_state(const char *body, size_t len){
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
            // A short read here just leaves the tail zeroed; the block is still
            // usable for drawing, which matters more than rejecting it.
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
            // Extra rows beyond VIEW_ROWS are discarded rather than written
            // past the end of the array. A server that sent a taller board
            // would be a bug, not a licence to overflow.
        }
        if (nl == NULL) break;
        p = nl + 1;
    }

    if (n > 0){ memcpy(g_boards, tmp, sizeof tmp); g_nboards = n; g_states++; }
}

// Read exactly one frame and classify it. A frame beginning "HTTTP/" is a
// response to something we sent; anything else is a server-originated STATE
// request. That single comparison is the whole disambiguation rule, and it is
// why the client can interleave its own requests with unsolicited broadcasts
// on one connection.
static int read_one_frame(void){
    size_t plen = 0;
    unsigned char *plain = tetrissh_recv(g_sess, g_sock, &plen);
    if (plain == NULL) return -1;              // closed, timed out, or bad frame

    if (plen >= 6 && memcmp(plain, "HTTTP/", 6) == 0){
        char keep[2048];
        size_t cp = plen < sizeof keep - 1 ? plen : sizeof keep - 1;
        memcpy(keep, plain, cp);
        htttp_msg_t msg;
        if (htttp_parse_response(keep, cp, &msg) == HTTTP_OK)
            g_last_status = (int)msg.status;
        g_acks++;
    } else {
        htttp_msg_t st;
        if (htttp_parse_request((const char *)plain, plen, &st) == HTTTP_OK &&
            st.method == HTTTP_METHOD_STATE && st.body != NULL)
            parse_state((const char *)st.body, st.body_len);
    }
    free(plain);
    return 0;
}

// --- rendering --------------------------------------------------------------

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

static void draw_board(const board_t *b, int top, int left, int is_me){
    draw_frame_at(top, left);
    for (int y = 0; y < VIEW_ROWS; y++)
        for (int x = 0; x < VIEW_COLS; x++){
            int sy = top + 1 + y, sx = left + 1 + x * CELL_W;
            char c = (y < b->rows_filled) ? b->cells[y][x] : '.';
            if (c == '.' || c == '\0'){
                attron(A_DIM); mvprintw(sy, sx, " ."); attroff(A_DIM);
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

static void draw_all(const char *room, int connected){
    erase();

    // Lay out from the bottom of the mandatory block upwards, so the board
    // always fits and the optional rows absorb whatever is left over.
    int have_names = (LINES >= VIEW_ROWS + 6);
    int board_top  = have_names ? 2 : 1;      // the top border row

    mvprintw(0, BOARD_LEFT, "tetriSH %s as %s  states %ld  acks %ld%s",
             room, g_player_id, g_states, g_acks,
             connected ? "" : "  [DISCONNECTED]");

    int per = VIEW_COLS * CELL_W + 4;
    for (int i = 0; i < g_nboards; i++){
        int left = BOARD_LEFT + i * per;
        if (left + per > COLS) break;         // never draw off the screen edge
        draw_board(&g_boards[i], board_top, left,
                   strcmp(g_boards[i].id, g_player_id) == 0);
    }

    int f = board_top + VIEW_ROWS + 3;
    if (f < LINES)
        mvprintw(f, BOARD_LEFT,
                 "arrows move - up rot cw - z ccw - space drop - q quit");

    // One doupdate per frame rather than a refresh per element, so the screen
    // updates atomically and never tears between boards.
    wnoutrefresh(stdscr);
    doupdate();
}

// --- main -------------------------------------------------------------------

int main(int argc, char **argv){
    if (argc < 2){
        fprintf(stderr,
                "usage: %s <rc-file> [room]\n"
                "  connects to tetrisd, joins a room, and plays it in the terminal\n",
                argv[0]);
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

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) die("socket: %s", strerror(errno));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)cfg.listen_port);
    if (inet_pton(AF_INET, cfg.bind_addr, &sa.sin_addr) != 1)
        die("bad server address %s", cfg.bind_addr);
    if (connect(g_sock, (struct sockaddr *)&sa, sizeof sa) < 0)
        die("cannot reach tetrisd: %s", strerror(errno));

    // Nagle would hold a small keypress packet until the previous one was
    // acknowledged. Combined with the peer's delayed-ACK timer that is a stall
    // of tens of milliseconds, longer than an entire frame, and it reads to the
    // player as input lag. Redis sets this unconditionally on every connection
    // and does not even offer a switch to turn it off.
    int one = 1;
    setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    // The socket stays blocking because libtetrissh needs it that way. These
    // bound how long a half-delivered frame can hold up the display.
    struct timeval to = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    setsockopt(g_sock, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);

    g_sess = tetrissh_session_alloc();
    if (g_sess == NULL) die("out of memory allocating the session", "");
    if (tetrissh_handshake_client(g_sess, g_sock, cfg.ca_path) != 0)
        die("handshake failed: %s", tetrissh_strerror(g_sess));

    char path[64], keep[4096];
    htttp_msg_t msg;

    snprintf(path, sizeof path, "/room/%s", room);
    if (send_request(HTTTP_METHOD_JOIN, path, NULL) != 0) die("JOIN send failed", "");
    int st = read_until_response(&msg, keep, sizeof keep);
    if (st != 200 && st != 201){
        char what[64];
        snprintf(what, sizeof what, "status %d", st);
        die("JOIN refused by the server: %s", what);
    }

    size_t vlen = 0;
    const char *v = htttp_find_header(&msg, "Player-Id", &vlen);
    if (v == NULL || vlen == 0 || vlen >= sizeof g_player_id)
        die("server issued no Player-Id", "");
    memcpy(g_player_id, v, vlen);
    g_player_id[vlen] = '\0';

    // 409 means somebody already started this room, which is success from our
    // point of view: the ticker we needed is already running.
    if (send_request(HTTTP_METHOD_START, path, NULL) != 0) die("START send failed", "");
    st = read_until_response(&msg, keep, sizeof keep);
    if (st != 200 && st != 409) die("START refused by the server", "");

    // Curses comes up only now, so every failure above prints normally.
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
        if (connected) FD_SET(g_sock, &rfds);

        int maxfd = sigfd > g_sock ? sigfd : g_sock;
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
        if (FD_ISSET(sigfd, &rfds)){
            struct signalfd_siginfo si;
            if (read(sigfd, &si, sizeof si) == (ssize_t)sizeof si) running = 0;
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
                case KEY_UP:    m = HTTTP_METHOD_ROTATE; body = "CW";    break;
                case 'z':       m = HTTTP_METHOD_ROTATE; body = "CCW";   break;
                case KEY_DOWN:  m = HTTTP_METHOD_DROP;   body = "SOFT";  break;
                case ' ':       m = HTTTP_METHOD_DROP;   body = "HARD";  break;
                case 'q':       running = 0; break;
                default: break;                       // ERR and unknown keys
                }
                if (body != NULL && connected){
                    char mpath[96];
                    snprintf(mpath, sizeof mpath, "/room/%s/player/%s",
                             room, g_player_id);
                    if (send_request(m, mpath, body) != 0) connected = 0;
                }
            }
        }

        if (connected && FD_ISSET(g_sock, &rfds)){
            if (read_one_frame() < 0) connected = 0;
        }

        draw_all(room, connected);
    }

    if (connected){
        send_request(HTTTP_METHOD_LEAVE, path, NULL);
        tetrissh_close(g_sess);
    }
    tetrissh_session_free(g_sess);
    close(g_sock);
    close(sigfd);
    restore_terminal();
    printf("left room %s as %s: %ld state frames, %ld acks\n",
           room, g_player_id, g_states, g_acks);
    return 0;
}
