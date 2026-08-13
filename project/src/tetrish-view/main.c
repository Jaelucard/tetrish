// tetrish-view: replay a recorded session from tetrislogd's log file, or watch
// a live room. Two modes, one renderer.
//
// WHY THE REPLAY CAN BE TRUSTED
//
// libtetrisbrain is deterministic by contract: no wall clock, no rand(), no
// I/O. A game is its seed plus the inputs it was fed and the tick each one
// landed on. tetrisd records exactly that:
//
//   E <ns> <tick> <room> <player> SEED    <seed>
//   E <ns> <tick> <room> <player> INPUT   <LEFT|RIGHT|ROTATE_CW|...>
//   E <ns> <tick> <room> <player> GARBAGE <rows> <hole_pattern_hex>
//   S <ns> <tick> <room> <player> <w> <h> <cells_hex> <seed>
//
// so replay is mechanical: seed the engine, walk tick 0..N, feed tb_tick
// whatever input that tick carried.
//
// The rule is lifted from Quake 3 demo playback. Its CL_ReadDemoMessage pushes
// recorded packets through CL_ParseServerMessage, the same function that
// handles live traffic, so a demo cannot drift from real play; there is only
// one code path. Same idea here: this calls tb_tick, the engine entry point
// tetrisd calls on every room tick. Gravity, locking and line clears are not
// reimplemented. If replay and live disagree, one caller is wrong, rather than
// two rival copies of the rules.
//
// WHAT SNAPSHOTS ARE FOR HERE
//
// Not seeking. A snapshot restores the visible board but NOT the RNG or the
// position in the 7-piece bag, so jumping to one makes every piece after it
// differ from the real game. Quake 3 ducks this by refusing to seek at all. We
// do not have to: a few minutes at 20 Hz is a few thousand tb_tick calls, which
// costs microseconds, so a seek just replays from tick 0 and is exactly right.
//
// Which frees snapshots up for verification instead. The reconstruction is
// compared against every recorded snapshot as it goes. A mismatch means a
// record never reached the log, which does happen under load, because the
// logging path drops rather than blocking the game. Catching a diverged replay
// beats a fast one, so a mismatch is counted, said out loud on screen, then
// resynced from the snapshot so the viewer keeps showing something true.
//
// Snapshots also re-state the seat's seed, every time. The SEED record is
// written once, at START, which is when the logging path is busiest and most
// likely to drop, so a session with a lost SEED used to be unreplayable
// outright. Now one surviving snapshot is enough to seed the reconstruction.
//
// usage: tetrish-view <log-file> [room] [player]        (replay mode)
//        tetrish-view --live <rc-file> [room]           (live spectator mode)
//        keys: space pause, left/right seek, +/- speed, q quit
//
// Live mode is the other half (see live_main below). Instead of rebuilding a
// finished session from the log file it attaches to a running tetrisd as a
// spectator: full secure handshake, then a JOIN carrying an X-Spectate header
// so the server hands out no seat. STATE broadcasts are rendered as they
// arrive, through the same shared renderer tetrisu uses.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <ncurses.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "rc.h"
#include "stateview.h"
#include "tetrisbrain.h"

#define MAX_EVENTS  65536
#define CELL_W          2
#define BOARD_TOP       1
#define BOARD_LEFT      2

typedef enum { EV_SEED, EV_INPUT, EV_GARBAGE, EV_SNAPSHOT } ev_kind;

typedef struct {
    ev_kind  kind;
    uint64_t tick;
    uint32_t seed;                            // EV_SEED; also EV_SNAPSHOT (re-stated there, 0 in old logs)
    tb_input input;                           // EV_INPUT
    int      rows;                            // EV_GARBAGE
    uint16_t hole;                            // EV_GARBAGE
    char     cells[TB_ROWS * TB_COLS + 1];    // EV_SNAPSHOT
} ev_t;

static ev_t g_ev[MAX_EVENTS];
static int  g_nev = 0;
static volatile sig_atomic_t g_quit = 0;

static void on_sigint(int sig){ (void)sig; g_quit = 1; }

// Instead of exit(), once ncurses owns the terminal. Skip the endwin and the
// message scrolls into a screen still in curses mode, leaving the shell
// unusable afterwards.
static void die(const char *msg){
    endwin();
    fprintf(stderr, "tetrish-view: %s\n", msg);
    exit(1);
}

static tb_input input_from_name(const char *s){
    if (strcmp(s, "LEFT")       == 0) return TB_INPUT_LEFT;
    if (strcmp(s, "RIGHT")      == 0) return TB_INPUT_RIGHT;
    if (strcmp(s, "ROTATE_CW")  == 0) return TB_INPUT_ROTATE_CW;
    if (strcmp(s, "ROTATE_CCW") == 0) return TB_INPUT_ROTATE_CCW;
    if (strcmp(s, "SOFT")       == 0) return TB_INPUT_SOFT_DROP;
    if (strcmp(s, "HARD")       == 0) return TB_INPUT_HARD_DROP;
    return TB_INPUT_NONE;
}

// Encode the board the way tetrisd encodes a snapshot, so a reconstruction can
// be compared against a recorded one byte for byte. Locked stack only, exactly
// as the server does, or the in-flight piece would fake a mismatch.
static void board_hex(const tb_game *g, char *out, size_t cap){
    static const char digits[] = "0123456789abcdef";
    size_t n = 0;
    for (int y = 1; y <= TB_ROWS && n + 1 < cap; y++)
        for (int x = 1; x <= TB_COLS && n + 1 < cap; x++){
            int8_t c = g->cells[y][x];
            int v = (c == TB_CELL_EMPTY)   ? 0
                  : (c == TB_CELL_GARBAGE) ? 8
                  : (c >= 1 && c <= 7)     ? c : 0;
            out[n++] = digits[v];
        }
    out[n] = '\0';
}

// Restore a board from a recorded snapshot. Only used to resync after a
// divergence is spotted. Fixes what the viewer draws, but cannot put back the
// engine's PRNG or bag position, so pieces after this point are the
// reconstruction's own, not the ones the player really saw.
static void board_from_hex(tb_game *g, const char *hex){
    size_t n = 0;
    for (int y = 1; y <= TB_ROWS; y++)
        for (int x = 1; x <= TB_COLS; x++){
            char c = hex[n] ? hex[n] : '0';
            if (hex[n]) n++;
            int v = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
            g->cells[y][x] = (v == 0) ? TB_CELL_EMPTY
                           : (v == 8) ? TB_CELL_GARBAGE
                                      : (int8_t)v;
        }
}

// Read the log, keep the records for one room and player.
//
// Lines look like:  2026-08-02 10:25:23 E 1234 480 roomA p7 INPUT LEFT
// tetrislogd puts its own date and time on the front, so the record starts at
// the third whitespace-separated field. Anything that is not a well-formed E or
// S record for the player we want gets skipped, which is how replay records
// share a file with ordinary prose logging.
static int load_log(const char *path, const char *want_room,
                    const char *want_player, char *room_out, char *player_out,
                    int *truncated){
    FILE *f = fopen(path, "r");
    if (f == NULL) return -1;

    char line[1024];
    int  seen_seed = 0;
    *truncated = 0;

    while (fgets(line, sizeof line, f) != NULL){
        size_t len = strlen(line);
        // No trailing newline means the file was cut off mid-write, which is
        // what a killed daemon leaves behind. Quake 3 checks its demo files
        // the same way: report the truncation, do not trust a half record.
        if (len > 0 && line[len - 1] != '\n'){ *truncated = 1; break; }

        char date[32], hhmmss[32], type[8], room[64], player[64], action[32];
        unsigned long long ns, tick;
        int consumed = 0;
        if (sscanf(line, "%31s %31s %7s %llu %llu %63s %63s%n",
                   date, hhmmss, type, &ns, &tick, room, player, &consumed) != 7)
            continue;
        (void)ns;
        if (type[1] != '\0') continue;                  // not a bare "E" or "S"
        if (type[0] != 'E' && type[0] != 'S') continue;
        if (want_room[0]   && strcmp(room, want_room)     != 0) continue;
        if (want_player[0] && strcmp(player, want_player) != 0) continue;

        // Lock on to the first session seen, ignore the rest. One log file
        // holds every room and player the daemon served, and blending two of
        // them rebuilds neither: two SEED records means two piece sequences
        // fighting over one board. Before this, running with no room named
        // replayed nonsense and said nothing.
        if (room_out[0] == '\0'){
            snprintf(room_out, 64, "%s", room);
            snprintf(player_out, 64, "%s", player);
        } else if (strcmp(room, room_out) != 0 || strcmp(player, player_out) != 0){
            continue;
        }
        if (g_nev >= MAX_EVENTS) break;

        const char *rest = line + consumed;
        ev_t *e = &g_ev[g_nev];
        memset(e, 0, sizeof *e);
        e->tick = tick;

        if (type[0] == 'S'){
            int w, h;
            // One hex digit per visible cell, so the sscanf width, this buffer
            // and e->cells all have to agree at TB_ROWS * TB_COLS. A width
            // wider than the destination is how buffers get overrun.
            char hex[TB_ROWS * TB_COLS + 1];
            unsigned s = 0;
            // Trailing seed is optional: logs from before snapshots re-stated
            // it have three fields only. Such a snapshot still verifies a
            // board, it just cannot seed a replay on its own.
            int n = sscanf(rest, " %d %d %200s %u", &w, &h, hex, &s);
            if (n < 3) continue;
            if (w != TB_COLS || h != TB_ROWS) continue;  // a board we cannot draw
            e->kind = EV_SNAPSHOT;
            snprintf(e->cells, sizeof e->cells, "%s", hex);
            if (n == 4){ e->seed = s; seen_seed = 1; }
        } else {
            if (sscanf(rest, " %31s", action) != 1) continue;
            if (strcmp(action, "SEED") == 0){
                unsigned s;
                if (sscanf(rest, " SEED %u", &s) != 1) continue;
                e->kind = EV_SEED; e->seed = s; seen_seed = 1;
            } else if (strcmp(action, "INPUT") == 0){
                char nm[32];
                if (sscanf(rest, " INPUT %31s", nm) != 1) continue;
                e->kind = EV_INPUT; e->input = input_from_name(nm);
            } else if (strcmp(action, "GARBAGE") == 0){
                unsigned rows, hole;
                if (sscanf(rest, " GARBAGE %u %x", &rows, &hole) != 2) continue;
                e->kind = EV_GARBAGE;
                e->rows = (int)rows;
                e->hole = (uint16_t)hole;
            } else {
                continue;              // CLEAR and OVER are not needed to rebuild
            }
        }
        g_nev++;
    }
    fclose(f);
    return seen_seed ? 0 : -2;
}

// Replay from tick 0 up to and including target_tick.
//
// Always from the beginning, on purpose. Starting at a snapshot would be
// faster and wrong: a snapshot carries the board and nothing else, so every
// piece after it would be invented rather than replayed.
static void rebuild(tb_game *g, uint64_t target_tick, int *mismatches,
                    uint64_t *last_verified){
    uint32_t seed = 0;
    for (int i = 0; i < g_nev; i++)
        if (g_ev[i].kind == EV_SEED){ seed = g_ev[i].seed; break; }
    // SEED is written once, into the busiest burst the logging path ever
    // sees, so it is the record most likely to be missing. Every snapshot
    // re-states it; any survivor will do. load_log only admits a session that
    // produced a seed one way or the other.
    if (seed == 0)
        for (int i = 0; i < g_nev; i++)
            if (g_ev[i].kind == EV_SNAPSHOT && g_ev[i].seed != 0){
                seed = g_ev[i].seed; break;
            }

    tb_init(g, seed);
    *mismatches    = 0;
    *last_verified = 0;

    int idx = 0;
    for (uint64_t t = 1; t <= target_tick; t++){
        tb_input in = TB_INPUT_NONE;

        while (idx < g_nev && g_ev[idx].tick < t) idx++;

        // Every record stamped with this tick. An input feeds the tick,
        // garbage goes in directly, exactly as the server injects it.
        for (int j = idx; j < g_nev && g_ev[j].tick == t; j++){
            if (g_ev[j].kind == EV_INPUT)
                in = g_ev[j].input;
            else if (g_ev[j].kind == EV_GARBAGE)
                tb_inject_garbage(g, g_ev[j].rows, g_ev[j].hole);
        }

        tb_tick(g, in);                       // the same call tetrisd makes

        // Check against this tick's snapshot, if the log recorded one.
        for (int j = idx; j < g_nev && g_ev[j].tick == t; j++){
            if (g_ev[j].kind != EV_SNAPSHOT) continue;
            char mine[TB_ROWS * TB_COLS + 1];
            board_hex(g, mine, sizeof mine);
            if (strcmp(mine, g_ev[j].cells) != 0){
                (*mismatches)++;
                board_from_hex(g, g_ev[j].cells);   // resync, keep the view honest
            } else {
                *last_verified = t;
            }
        }
    }
}

static void draw_frame(int top, int left){
    int w = TB_COLS * CELL_W;
    mvaddch(top, left, ACS_ULCORNER);
    mvaddch(top, left + w + 1, ACS_URCORNER);
    mvaddch(top + TB_ROWS + 1, left, ACS_LLCORNER);
    mvaddch(top + TB_ROWS + 1, left + w + 1, ACS_LRCORNER);
    for (int c = 1; c <= w; c++){
        mvaddch(top, left + c, ACS_HLINE);
        mvaddch(top + TB_ROWS + 1, left + c, ACS_HLINE);
    }
    for (int r = 1; r <= TB_ROWS; r++){
        mvaddch(top + r, left, ACS_VLINE);
        mvaddch(top + r, left + w + 1, ACS_VLINE);
    }
}

static void draw(const tb_game *g, const char *room, const char *player,
                 uint64_t tick, uint64_t last_tick, int paused, double speed,
                 int mismatches, uint64_t last_verified, int truncated){
    int8_t view[TB_ROWS][TB_COLS];
    tb_render(g, view);

    erase();
    draw_frame(BOARD_TOP, BOARD_LEFT);
    for (int y = 0; y < TB_ROWS; y++)
        for (int x = 0; x < TB_COLS; x++){
            int sy = BOARD_TOP + 1 + y;
            int sx = BOARD_LEFT + 1 + x * CELL_W;
            if (view[y][x] < 0){
                attron(A_DIM); mvprintw(sy, sx, " ."); attroff(A_DIM);
            } else {
                mvprintw(sy, sx, "[]");
            }
        }

    int f = BOARD_TOP + TB_ROWS + 2;
    mvprintw(f,     BOARD_LEFT, "REPLAY  %s / %s", room, player);
    mvprintw(f + 1, BOARD_LEFT, "tick %llu / %llu   %s   speed %.3gx",
             (unsigned long long)tick, (unsigned long long)last_tick,
             paused ? "PAUSED " : "playing", speed);
    mvprintw(f + 2, BOARD_LEFT, "score %u  lines %u", g->score, g->lines_total);

    // Say how much of this reconstruction can be trusted. A viewer that shows
    // a diverged board and keeps quiet about it is worse than one that owns up.
    if (mismatches > 0){
        attron(A_BOLD);
        mvprintw(f + 3, BOARD_LEFT,
                 "%d snapshot mismatch(es): records were lost, resynced",
                 mismatches);
        attroff(A_BOLD);
    } else {
        mvprintw(f + 3, BOARD_LEFT, "verified against snapshot at tick %llu",
                 (unsigned long long)last_verified);
    }
    if (truncated)
        mvprintw(f + 4, BOARD_LEFT, "log was truncated: session ends early");
    mvprintw(f + 5, BOARD_LEFT,
             "space pause - left/right seek - +/- speed - q quit");
    refresh();
}

// --- live spectator mode (50.003) -------------------------------------------

// Tear down a live connection. Safe on a half-open one, which is what keeps
// the reconnect loop simple: every failure path funnels through here.
static void live_drop(stateview_t *v){
    if (v->sess){ tetrissh_session_free(v->sess); v->sess = NULL; }
    if (v->sock >= 0){ close(v->sock); v->sock = -1; }
}

// Connect, handshake, JOIN as a spectator. On failure it writes a readable
// reason into err and leaves v fully torn down. The socket timeouts bound
// every blocking step, so one attempt costs seconds at worst, never forever.
static int live_connect(const Config *cfg, const char *room, stateview_t *v,
                        char *err, size_t errsz){
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0){ snprintf(err, errsz, "socket: %s", strerror(errno)); return -1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)cfg->listen_port);
    if (inet_pton(AF_INET, cfg->bind_addr, &sa.sin_addr) != 1){
        snprintf(err, errsz, "bad server address %s", cfg->bind_addr);
        close(fd); return -1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0){
        snprintf(err, errsz, "connect: %s", strerror(errno));
        close(fd); return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval to = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);

    v->sess = tetrissh_session_alloc();
    if (v->sess == NULL){
        snprintf(err, errsz, "out of memory");
        close(fd); return -1;
    }
    v->sock = fd;
    if (tetrissh_handshake_client(v->sess, v->sock, cfg->ca_path) != 0){
        snprintf(err, errsz, "handshake: %s", tetrissh_strerror(v->sess));
        live_drop(v); return -1;
    }

    char path[64], keep[4096];
    htttp_msg_t msg;
    snprintf(path, sizeof path, "/room/%s", room);
    if (sv_send_request(v, HTTTP_METHOD_JOIN, path, NULL, "X-Spectate", "1") != 0){
        snprintf(err, errsz, "JOIN send failed");
        live_drop(v); return -1;
    }
    int st = sv_read_until_response(v, &msg, keep, sizeof keep);
    if (st != 200){
        snprintf(err, errsz, "spectator JOIN refused: status %d%s", st,
                 st == 404 ? " (no such room - is a game running?)" : "");
        live_drop(v); return -1;
    }
    size_t vlen = 0;
    const char *pid = htttp_find_header(&msg, "Player-Id", &vlen);
    if (pid != NULL && vlen > 0 && vlen < sizeof v->player_id){
        memcpy(v->player_id, pid, vlen);
        v->player_id[vlen] = '\0';
    }
    return 0;
}

// Watch a room live. The loop is select() over stdin and the socket, tetrisu's
// shape minus the game keys: the only input is q.
//
// Reconnection is the one piece of real logic in this mode. A viewer exists to
// watch somebody ELSE's game, so a tetrisd restart must not kill it. On a dead
// connection it keeps the terminal, says what happened, and retries with
// exponential backoff: 1s doubling to a 30s cap, reset once a connection
// sticks. That is the usual shape for not hammering a server that is busy
// coming back up. The FIRST connection is the exception and fails loud and
// fast, because a mistyped room name deserves an error message, not a silent
// retry forever.
static int live_main(const char *rc_path, const char *room){
    Config cfg;
    if (rc_load(rc_path, &cfg) != 0){
        fprintf(stderr, "tetrish-view: failed to load configuration from %s\n",
                rc_path);
        return 1;
    }
    signal(SIGINT, on_sigint);
    // A server dying mid-send should come back as -1 from write, not kill us.
    signal(SIGPIPE, SIG_IGN);

    stateview_t v;
    memset(&v, 0, sizeof v);
    v.sock = -1;

    char err[256];
    if (live_connect(&cfg, room, &v, err, sizeof err) != 0){
        fprintf(stderr, "tetrish-view: %s\n", err);
        return 1;
    }

    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE); set_escdelay(0); curs_set(0);
    if (has_colors()){
        start_color();
        use_default_colors();
        short fg[9] = { 0, COLOR_CYAN, COLOR_BLUE, COLOR_WHITE, COLOR_YELLOW,
                        COLOR_GREEN, COLOR_MAGENTA, COLOR_RED, COLOR_WHITE };
        for (int i = 1; i <= 8; i++) init_pair((short)i, fg[i], -1);
    }

    int    connected = 1, backoff = 1;
    time_t retry_at  = 0;
    while (!g_quit){
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        if (connected) FD_SET(v.sock, &rfds);
        int maxfd = (connected && v.sock > STDIN_FILENO) ? v.sock : STDIN_FILENO;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };   // 20 Hz redraw
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0){
            if (errno == EINTR) continue;    // SIGINT: g_quit ends the loop
            break;
        }

        int ch;
        while ((ch = getch()) != ERR)
            if (ch == 'q') g_quit = 1;

        if (connected && FD_ISSET(v.sock, &rfds)){
            if (sv_read_one_frame(&v) < 0){
                live_drop(&v);
                connected = 0;
                backoff   = 1;
                retry_at  = time(NULL) + backoff;
            }
        }

        if (!connected && time(NULL) >= retry_at){
            if (live_connect(&cfg, room, &v, err, sizeof err) == 0){
                connected = 1;
                backoff   = 1;
            } else {
                backoff  = backoff < 30 ? backoff * 2 : 30;
                retry_at = time(NULL) + backoff;
            }
        }

        char title[160];
        if (connected)
            snprintf(title, sizeof title, "SPECTATING %s  states %ld",
                     room, v.states);
        else {
            long wait = (long)(retry_at - time(NULL));
            snprintf(title, sizeof title,
                     "SPECTATING %s  [DISCONNECTED - retry in %lds]",
                     room, wait > 0 ? wait : 0);
        }
        sv_draw_all(&v, title, "q quit - reconnects with backoff by itself");
    }

    if (connected){
        char path[64];
        snprintf(path, sizeof path, "/room/%s", room);
        sv_send_request(&v, HTTTP_METHOD_LEAVE, path, NULL, NULL, NULL);
        tetrissh_close(v.sess);
    }
    live_drop(&v);
    endwin();
    printf("spectated %s: %ld state frames\n", room, v.states);
    return 0;
}

int main(int argc, char **argv){
    if (argc < 2){
        fprintf(stderr,
                "usage: %s [--verify] <log-file> [room] [player]\n"
                "       %s --live <rc-file> [room]\n"
                "  replays a recorded session from tetrislogd's log file,\n"
                "  or spectates a running room live over a secure session\n"
                "  --verify  reconstruct without ncurses and print the result,\n"
                "            so replay can be checked from a script or a test\n",
                argv[0], argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "--live") == 0){
        if (argc < 3){
            fprintf(stderr, "tetrish-view: --live needs an rc file\n");
            return 2;
        }
        return live_main(argv[2], argc > 3 ? argv[3] : "demo");
    }

    // --verify runs the whole reconstruction headless. The interactive viewer
    // needs a terminal, so no automated test can drive it, and a replay nobody
    // can check is not worth much. This mode is what proves the recorded format
    // really does carry enough to rebuild a game.
    int verify = 0, argi = 1;
    if (strcmp(argv[argi], "--verify") == 0){ verify = 1; argi++; }
    if (argi >= argc){ fprintf(stderr, "tetrish-view: no log file given\n"); return 2; }

    const char *logpath     = argv[argi];
    const char *want_room   = (argi + 1 < argc) ? argv[argi + 1] : "";
    const char *want_player = (argi + 2 < argc) ? argv[argi + 2] : "";

    char room[64] = "", player[64] = "";
    int truncated = 0;
    int rc = load_log(logpath, want_room, want_player, room, player, &truncated);
    if (rc == -1){
        fprintf(stderr, "tetrish-view: cannot open %s\n", logpath);
        return 1;
    }
    if (rc == -2 || g_nev == 0){
        fprintf(stderr,
                "tetrish-view: no replayable session found in %s\n"
                "  a session needs a seed: a SEED record (written at START) or\n"
                "  any snapshot record, each of which re-states it\n"
                "  try naming a room and player, e.g. %s %s roomA p7\n",
                logpath, argv[0], logpath);
        return 1;
    }

    uint64_t last_tick = 0;
    for (int i = 0; i < g_nev; i++)
        if (g_ev[i].tick > last_tick) last_tick = g_ev[i].tick;

    if (verify){
        int nseed = 0, ninput = 0, ngarbage = 0, nsnap = 0;
        for (int i = 0; i < g_nev; i++)
            switch (g_ev[i].kind){
            case EV_SEED:     nseed++;    break;
            case EV_INPUT:    ninput++;   break;
            case EV_GARBAGE:  ngarbage++; break;
            case EV_SNAPSHOT: nsnap++;    break;
            }

        tb_game  g;
        int      mismatches = 0;
        uint64_t last_verified = 0;
        rebuild(&g, last_tick, &mismatches, &last_verified);

        printf("session      : %s / %s\n", room, player);
        printf("records      : %d seed, %d input, %d garbage, %d snapshot\n",
               nseed, ninput, ngarbage, nsnap);
        printf("ticks        : 0 to %llu\n", (unsigned long long)last_tick);
        printf("final board  : score %u, lines %u, %s\n",
               g.score, g.lines_total, g.game_over ? "game over" : "still alive");
        printf("snapshots    : %d checked, %d mismatched\n", nsnap, mismatches);
        printf("last verified: tick %llu\n", (unsigned long long)last_verified);
        if (truncated) printf("WARNING      : log was truncated\n");
        if (nsnap == 0)
            printf("WARNING      : no snapshots, so nothing was actually verified\n");
        printf("%s\n", (mismatches == 0 && nsnap > 0)
               ? "REPLAY VERIFIED: reconstruction matches every recorded snapshot"
               : "REPLAY UNVERIFIED: see warnings above");
        return (mismatches == 0 && nsnap > 0) ? 0 : 1;
    }

    signal(SIGINT, on_sigint);
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE); set_escdelay(0); curs_set(0);
    if (LINES < TB_ROWS + 10 || COLS < TB_COLS * CELL_W + 8)
        die("terminal too small: needs about 30 rows by 30 columns");

    tb_game  g;
    uint64_t tick = 0, last_verified = 0;
    int      paused = 0, mismatches = 0;
    double   speed = 1.0;
    const int base_ms = 50;                 // 20 Hz, matching the default tick rate

    rebuild(&g, tick, &mismatches, &last_verified);

    while (!g_quit){
        int ch;
        while ((ch = getch()) != ERR){
            if      (ch == 'q')  g_quit = 1;
            else if (ch == ' ')  paused = !paused;
            else if (ch == '+' || ch == '=') speed = speed < 8   ? speed * 2 : speed;
            else if (ch == '-' || ch == '_') speed = speed > 0.125 ? speed / 2 : speed;
            else if (ch == KEY_RIGHT || ch == KEY_LEFT){
                uint64_t step = 100;         // 5 seconds of game time at 20 Hz
                tick = (ch == KEY_RIGHT)
                     ? (tick + step > last_tick ? last_tick : tick + step)
                     : (tick > step ? tick - step : 0);
                rebuild(&g, tick, &mismatches, &last_verified);
            }
        }

        if (!paused && tick < last_tick){
            tick++;
            // One tick forward means replaying up to it, so O(tick) not O(1).
            // At a few thousand ticks that is still well under a frame, and it
            // keeps play and seek on one code path.
            rebuild(&g, tick, &mismatches, &last_verified);
        }

        draw(&g, room, player, tick, last_tick, paused, speed,
             mismatches, last_verified, truncated);
        napms((int)(base_ms / speed));
    }

    endwin();
    printf("replayed %s / %s up to tick %llu of %llu\n",
           room, player, (unsigned long long)tick, (unsigned long long)last_tick);
    printf("%d events, %d snapshot mismatch(es)%s\n",
           g_nev, mismatches, truncated ? ", log truncated" : "");
    return 0;
}
