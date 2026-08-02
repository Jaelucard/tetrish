// tetrish-view replays a recorded game session from tetrislogd's log file.
//
// HOW REPLAY WORKS, AND WHY IT IS TRUSTWORTHY
//
// libtetrisbrain is deterministic by contract: no wall-clock time, no rand(),
// no I/O. A game is entirely determined by its seed plus the sequence of inputs
// it was fed and the tick each one landed on. tetrisd records exactly that:
//
//   E <ns> <tick> <room> <player> SEED    <seed>
//   E <ns> <tick> <room> <player> INPUT   <LEFT|RIGHT|ROTATE_CW|...>
//   E <ns> <tick> <room> <player> GARBAGE <rows> <hole_pattern_hex>
//   S <ns> <tick> <room> <player> <w> <h> <cells_hex>
//
// So replaying is mechanical: seed the engine, then walk tick 0..N calling
// tb_tick once per tick with whatever input that tick carried.
//
// The important design rule is borrowed from how Quake 3 plays back demos. Its
// CL_ReadDemoMessage feeds recorded packets into CL_ParseServerMessage, the
// very same function that handles live network traffic, so a demo cannot drift
// away from real play because there is only one code path. The equivalent here
// is that this program calls tb_tick, the same engine entry point tetrisd calls
// on every room tick. It does not reimplement gravity, locking or line clears.
// If replay and live ever disagree, that is a bug in one caller, not a
// disagreement between two rival implementations of the rules.
//
// WHAT SNAPSHOTS ARE FOR HERE
//
// Not seeking. A snapshot restores the visible board but NOT the random number
// generator or the position in the 7-piece bag, so jumping to one would make
// every subsequent piece differ from the real game. Quake 3 sidesteps the whole
// problem by refusing to seek at all. We do not have to: a few minutes at 20 Hz
// is a few thousand tb_tick calls, which costs microseconds, so seeking simply
// replays from tick 0 and is exactly correct.
//
// That frees snapshots to do something more valuable: verification. The
// reconstruction is compared against every recorded snapshot as it goes. A
// mismatch means a record never reached the log, which does happen under load
// because the logging path deliberately drops rather than blocking the game.
// Detecting a divergent replay is worth more than a fast one, so a mismatch is
// counted, reported on screen, and then resynced from the snapshot so the
// viewer keeps showing something truthful.
//
// usage: tetrish-view <log-file> [room] [player]
//        keys: space pause, left/right seek, +/- speed, q quit

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ncurses.h>
#include "tetrisbrain.h"

#define MAX_EVENTS  65536
#define CELL_W          2
#define BOARD_TOP       1
#define BOARD_LEFT      2

typedef enum { EV_SEED, EV_INPUT, EV_GARBAGE, EV_SNAPSHOT } ev_kind;

typedef struct {
    ev_kind  kind;
    uint64_t tick;
    uint32_t seed;                            // EV_SEED
    tb_input input;                           // EV_INPUT
    int      rows;                            // EV_GARBAGE
    uint16_t hole;                            // EV_GARBAGE
    char     cells[TB_ROWS * TB_COLS + 1];    // EV_SNAPSHOT
} ev_t;

static ev_t g_ev[MAX_EVENTS];
static int  g_nev = 0;
static volatile sig_atomic_t g_quit = 0;

static void on_sigint(int sig){ (void)sig; g_quit = 1; }

// Used instead of exit() once ncurses owns the terminal. Without restoring the
// terminal first, the message scrolls into a screen still in curses mode and
// the shell is left unusable afterwards.
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

// Encode the board the same way tetrisd encodes a snapshot, so a
// reconstruction can be compared against a recorded one byte for byte. Reads
// the locked stack only, exactly as the server does, so the in-flight piece
// never causes a false mismatch.
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

// Restore a board from a recorded snapshot. Used only to resync after a
// detected divergence. It fixes what the viewer draws, but it cannot restore
// the engine's PRNG or bag position, so pieces after this point are the
// reconstruction's own rather than the ones the player really saw.
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

// Read the log and keep the records for one room and player.
//
// Lines look like:  2026-08-02 10:25:23 E 1234 480 roomA p7 INPUT LEFT
// tetrislogd prepends its own date and time, so the record itself starts at the
// third whitespace-separated field. Anything that is not a well-formed E or S
// record for the requested player is skipped, which is what lets replay records
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
        // A line with no newline means the file was cut off mid-write, which
        // happens when the daemon is killed. Quake 3 checks its demo files the
        // same way and reports the truncation rather than trusting a partial
        // record.
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

        // Lock on to the first session we see and ignore every other one.
        // A single log file holds every room and player the daemon served, and
        // blending two of them produces a reconstruction that matches neither:
        // two SEED records means two different piece sequences fighting over
        // one board. Without this, running with no room named silently
        // replayed nonsense.
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
            // Exactly one hex digit per visible cell, so the field width, this
            // buffer and e->cells all have to agree at TB_ROWS * TB_COLS. A
            // wider sscanf width than the destination is how buffers get
            // overrun, and the compiler will say so.
            char hex[TB_ROWS * TB_COLS + 1];
            if (sscanf(rest, " %d %d %200s", &w, &h, hex) != 3) continue;
            if (w != TB_COLS || h != TB_ROWS) continue;  // a board we cannot draw
            e->kind = EV_SNAPSHOT;
            snprintf(e->cells, sizeof e->cells, "%s", hex);
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
// Deliberately always from the beginning. Starting from a snapshot would be
// faster but wrong: a snapshot carries the board and nothing else, so the piece
// sequence after it would be invented rather than replayed.
static void rebuild(tb_game *g, uint64_t target_tick, int *mismatches,
                    uint64_t *last_verified){
    uint32_t seed = 0;
    for (int i = 0; i < g_nev; i++)
        if (g_ev[i].kind == EV_SEED){ seed = g_ev[i].seed; break; }

    tb_init(g, seed);
    *mismatches    = 0;
    *last_verified = 0;

    int idx = 0;
    for (uint64_t t = 1; t <= target_tick; t++){
        tb_input in = TB_INPUT_NONE;

        while (idx < g_nev && g_ev[idx].tick < t) idx++;

        // Apply every record stamped with this tick. An input feeds the tick;
        // garbage is injected directly, exactly as the server injects it.
        for (int j = idx; j < g_nev && g_ev[j].tick == t; j++){
            if (g_ev[j].kind == EV_INPUT)
                in = g_ev[j].input;
            else if (g_ev[j].kind == EV_GARBAGE)
                tb_inject_garbage(g, g_ev[j].rows, g_ev[j].hole);
        }

        tb_tick(g, in);                       // the same call tetrisd makes

        // Verify against a snapshot for this tick, if the log recorded one.
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

    // Say plainly how much of this reconstruction is trustworthy. A viewer that
    // quietly shows a diverged board is worse than one that admits it.
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

int main(int argc, char **argv){
    if (argc < 2){
        fprintf(stderr,
                "usage: %s [--verify] <log-file> [room] [player]\n"
                "  replays a recorded session from tetrislogd's log file\n"
                "  --verify  reconstruct without ncurses and print the result,\n"
                "            so replay can be checked from a script or a test\n",
                argv[0]);
        return 2;
    }

    // --verify runs the whole reconstruction headless. It exists because the
    // interactive viewer needs a terminal, which means it cannot be exercised
    // by an automated test, and an unverifiable replay is not worth much. This
    // mode is what proves the recorded format actually contains enough to
    // rebuild a game.
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
                "  a session needs a SEED record, which tetrisd writes on START\n"
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
            // Advancing one tick means replaying up to it, which is O(tick)
            // rather than O(1). At a few thousand ticks that is still far under
            // one frame, and it keeps exactly one code path for play and seek.
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
