// tetrisu is the player's client: an ncurses terminal that talks to tetrisd.
//
// The whole architecture in one line: the client is a terminal, not a game.
//
// The server is authoritative. It owns gravity, collision, line clears and
// scoring, and broadcasts the finished board as text at tick_hz. This program
// draws what it is told and sends keystrokes. It does not link libtetrisbrain
// and holds no game state, so there is no prediction, no rollback, and no way
// for the display to disagree with the server about what happened.
//
// THE LOOP
//
// One select() over three descriptors, its timeout doubling as the frame pacer:
//
//   STDIN_FILENO  the keyboard
//   sockfd        encrypted frames from tetrisd
//   sigfd         SIGINT, as a readable fd instead of a handler
//
// Four details here are not obvious. All four come from how Redis and ioquake3
// drive their own select loops.
//
//  - The fd_set and the timeout are rebuilt from scratch every iteration.
//    select() is destructive: on return the set holds only the ready
//    descriptors, every other bit cleared, and on Linux the timeval is
//    overwritten with the unslept remainder. Redis keeps a master set and
//    memcpys a scratch copy for this exact reason (ae_select.c, "it's not safe
//    to reuse FD sets after select()"), and declares its timeval fresh on every
//    pass. Hoist either out of the loop and the client stops watching the
//    keyboard, then spins at 100% CPU.
//
//  - Independent ifs, never else-if. At 20 Hz a STATE broadcast lands on nearly
//    every frame, so an else-if chain testing the socket first would eat most
//    keypresses. redis-cli has that shape and only gets away with it because it
//    exits on any key.
//
//  - EINTR from select() is a frame where nothing happened. Not an error, not
//    something to retry. Redis does the same and it costs one short frame.
//
//  - SIGINT arrives on a signalfd, not through a handler. Kills the whole EINTR
//    problem class, and the shutdown path can then call endwin() and free
//    memory, none of which is legal inside a real handler. tetrisd uses
//    signalfd for the same reason.
//
// THE SOCKET STAYS BLOCKING, ON PURPOSE
//
// Everything above argues for a non-blocking socket. This one blocks, because
// libtetrissh receives with an MSG_WAITALL loop that assumes it can sit and
// wait for a whole frame; hand it a non-blocking fd and it fails partway
// through with EAGAIN. select() only promises the FIRST byte arrived, so a
// half-delivered frame would otherwise freeze the display.
//
// What bounds that wait is libtetrissh's per-operation budget, not the socket
// timeout, and the difference matters here as much as on the server.
// SO_RCVTIMEO expires per syscall, and a recv() that times out after copying
// some bytes returns the short count instead of failing, so the reassembly loop
// just starts a fresh timeout. A server (or a box in the middle) feeding us one
// byte at a time would hold this process inside read_one_frame forever, with
// the terminal in curses mode and SIGINT already blocked for the signalfd: no
// way out but SIGKILL from another terminal. libtetrissh instead starts a clock
// on the frame's first byte and gives the rest a fixed budget. Waiting for a
// frame to BEGIN stays unbounded, deliberately, because a gap between STATE
// broadcasts is the server being quiet, not the server being stuck.

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
#include "stateview.h"

#define FRAME_US    16000        // ~60 Hz display against a 20 Hz server

// Vertical budget. A classic terminal is 80x24 and this has to fit one. The
// mandatory part is a title row, the top border, VIEW_ROWS of board and the
// bottom border: 23 rows, leaving row 23 for a footer. Per-board names and
// counters are drawn only when the terminal is actually taller, instead of
// being demanded up front.
#define ROWS_REQUIRED  (VIEW_ROWS + 4)

// Connection, board table and counters live in a shared stateview_t. The frame
// classifier, STATE parser and renderer moved out to src/common/stateview.c
// when tetrish-view grew its live spectator mode: player and spectator have to
// classify frames and draw boards identically, and one implementation with two
// callers is the only way that stays true.
static stateview_t g_v;
static int g_curses_up = 0;

// Registered with atexit BEFORE curses starts, so every exit path restores the
// terminal, including exit paths not written yet. jserv's tetris does this,
// brenns10's does not, which is why brenns10's leaves your shell unusable when
// it dies on a small terminal.
static void restore_terminal(void){
    if (g_curses_up){ endwin(); g_curses_up = 0; }
}

// Fail with the terminal already put back, so the message is readable and the
// shell survives. Never exit() directly once curses owns the screen.
static void die(const char *fmt, const char *arg){
    restore_terminal();
    fprintf(stderr, "tetrisu: ");
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
    exit(1);
}

// The two lines that are tetrisu's own (who am I, what keys work), then hand
// the board table to the shared renderer.
static void draw_all(const char *room, int connected){
    char title[160];
    snprintf(title, sizeof title, "tetriSH %s as %s  states %ld  acks %ld%s",
             room, g_v.player_id, g_v.states, g_v.acks,
             connected ? "" : "  [DISCONNECTED]");
    sv_draw_all(&g_v, title,
                "arrows move - up rot cw - z ccw - space drop - q quit");
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

    // Block SIGINT, take it as a readable descriptor instead. Has to happen
    // before anything else can be interrupted.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) die("sigprocmask: %s", strerror(errno));
    int sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigfd < 0) die("signalfd: %s", strerror(errno));

    g_v.sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_v.sock < 0) die("socket: %s", strerror(errno));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)cfg.listen_port);
    if (inet_pton(AF_INET, cfg.bind_addr, &sa.sin_addr) != 1)
        die("bad server address %s", cfg.bind_addr);
    if (connect(g_v.sock, (struct sockaddr *)&sa, sizeof sa) < 0)
        die("cannot reach tetrisd: %s", strerror(errno));

    // Nagle would sit on a small keypress packet until the previous one was
    // acked. Against the peer's delayed-ACK timer that is tens of milliseconds,
    // longer than a whole frame, and the player reads it as input lag. Redis
    // sets this on every connection and does not even offer a switch for it.
    int one = 1;
    setsockopt(g_v.sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    // Socket stays blocking because libtetrissh needs it that way. These cap
    // how long a half-delivered frame can hold up the display.
    struct timeval to = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(g_v.sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    setsockopt(g_v.sock, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);

    g_v.sess = tetrissh_session_alloc();
    if (g_v.sess == NULL) die("out of memory allocating the session", "");
    if (tetrissh_handshake_client(g_v.sess, g_v.sock, cfg.ca_path) != 0)
        die("handshake failed: %s", tetrissh_strerror(g_v.sess));

    char path[64], keep[4096];
    htttp_msg_t msg;

    snprintf(path, sizeof path, "/room/%s", room);
    if (sv_send_request(&g_v, HTTTP_METHOD_JOIN, path, NULL, NULL, NULL) != 0)
        die("JOIN send failed", "");
    int st = sv_read_until_response(&g_v, &msg, keep, sizeof keep);
    if (st != 200 && st != 201){
        char what[64];
        snprintf(what, sizeof what, "status %d", st);
        die("JOIN refused by the server: %s", what);
    }

    size_t vlen = 0;
    const char *v = htttp_find_header(&msg, "Player-Id", &vlen);
    if (v == NULL || vlen == 0 || vlen >= sizeof g_v.player_id)
        die("server issued no Player-Id", "");
    memcpy(g_v.player_id, v, vlen);
    g_v.player_id[vlen] = '\0';

    // 409 means somebody already started this room, which counts as success
    // here: the ticker we wanted is running either way.
    if (sv_send_request(&g_v, HTTTP_METHOD_START, path, NULL, NULL, NULL) != 0)
        die("START send failed", "");
    st = sv_read_until_response(&g_v, &msg, keep, sizeof keep);
    if (st != 200 && st != 409) die("START refused by the server", "");

    // Curses only comes up now, so every failure above prints normally.
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
        // Rebuilt every pass. select() clears every bit that was not ready and
        // on Linux also overwrites the timeout with the unslept remainder, so
        // neither can be hoisted out of this loop.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(sigfd, &rfds);
        if (connected) FD_SET(g_v.sock, &rfds);

        int maxfd = sigfd > g_v.sock ? sigfd : g_v.sock;
        if (STDIN_FILENO > maxfd) maxfd = STDIN_FILENO;

        struct timeval tv = { .tv_sec = 0, .tv_usec = FRAME_US };
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0){
            // A signal beat select() to it. Count that as a frame where
            // nothing happened; no retry, no failure. The signalfd is still
            // readable next time round.
            if (errno == EINTR) continue;
            die("select: %s", strerror(errno));
        }

        // Independent ifs, never else-if. At 20 Hz the socket is ready on
        // almost every frame, so an else-if chain would starve the keyboard.
        if (FD_ISSET(sigfd, &rfds)){
            struct signalfd_siginfo si;
            if (read(sigfd, &si, sizeof si) == (ssize_t)sizeof si) running = 0;
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)){
            // Drained in a loop, not one getch per frame. One per frame falls
            // behind a fast typist and leaves keys queued in the tty, which
            // looks like the piece carrying on moving after you let go.
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
                             room, g_v.player_id);
                    if (sv_send_request(&g_v, m, mpath, body, NULL, NULL) != 0)
                        connected = 0;
                }
            }
        }

        if (connected && FD_ISSET(g_v.sock, &rfds)){
            if (sv_read_one_frame(&g_v) < 0) connected = 0;
        }

        draw_all(room, connected);
    }

    if (connected){
        sv_send_request(&g_v, HTTTP_METHOD_LEAVE, path, NULL, NULL, NULL);
        tetrissh_close(g_v.sess);
    }
    tetrissh_session_free(g_v.sess);
    close(g_v.sock);
    close(sigfd);
    restore_terminal();
    printf("left room %s as %s: %ld state frames, %ld acks\n",
           room, g_v.player_id, g_v.states, g_v.acks);
    return 0;
}
