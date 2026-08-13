// tetrisu.c: playable ncurses Tetris client (dot-grid / bracket style),
// against a purely LOCAL tb_game (no sockets). Reached via `tetrisu --local`.
//
// The engine is tick-driven, so this runs a fixed ~60 Hz loop: each frame it
// reads at most one key, turns it into a tb_input (or TB_INPUT_NONE if no key
// was pressed), advances the game by one tick, and redraws. Gravity needs no
// special handling. The ticks themselves drive it.

#include <locale.h>
#include <ncurses.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tetrisbrain.h"
#include "local.h"
#include "viz.h"

#define FRAME_MS    16       // ~60 Hz; also the gravity time base
// The engine's default lock delay (TB_LOCK_DELAY_TICKS = 10) is "500 ms at
// tick_hz 20" -- the SERVER's tick rate. This loop ticks the engine at ~60
// Hz, where 10 ticks is a barely-perceptible 170 ms, so pieces feel like
// they cement on contact. Re-express the same half second in local frames:
// touching down starts this timer, and each successful shift/rotate resets
// it (up to TB_LOCK_MAX_RESETS), which is the slide-into-the-gap grace the
// move-reset rule exists for.
#define LOCK_DELAY_FRAMES  (500 / FRAME_MS)
#define CELL_W       2       // each board cell is 2 terminal columns wide
#define BOARD_TOP    1
#define BOARD_LEFT   2
#define SIDE_LEFT    (BOARD_LEFT + TB_COLS * CELL_W + 4)   // NEXT/HOLD boxes

static volatile sig_atomic_t g_quit = 0;

// SIGINT handler: just raise a flag for the main loop to notice. ncurses
// teardown is not signal-safe, so nothing else may happen here.
static void on_sigint(int sig)
{
    (void)sig;
    g_quit = 1;              // handled in the loop; never call endwin() here
}

// monotonic milliseconds for the visualizer clock; unsigned difference
// arithmetic keeps elapsed exact across the 32-bit wrap.
static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

// filled garbage cells on the board right now. the engine marks them
// with their own cell value, so unlike the networked client this count
// is exact, and an increase between frames is a garbage injection.
static int count_garbage(const tb_game *g)
{
    int n = 0;
    for (int y = 1; y <= TB_ROWS; y++)
        for (int x = 1; x <= TB_COLS; x++)
            if (g->cells[y][x] == TB_CELL_GARBAGE)
                n++;
    return n;
}

// draws the board's box-drawing border around a TB_COLS x TB_ROWS area.
static void draw_frame(int top, int left)
{
    int w = TB_COLS * CELL_W;
    mvaddch(top, left, ACS_ULCORNER);
    mvaddch(top, left + w + 1, ACS_URCORNER);
    mvaddch(top + TB_ROWS + 1, left, ACS_LLCORNER);
    mvaddch(top + TB_ROWS + 1, left + w + 1, ACS_LRCORNER);
    for (int c = 1; c <= w; c++) {
        mvaddch(top, left + c, ACS_HLINE);
        mvaddch(top + TB_ROWS + 1, left + c, ACS_HLINE);
    }
    for (int r = 1; r <= TB_ROWS; r++) {
        mvaddch(top + r, left, ACS_VLINE);
        mvaddch(top + r, left + w + 1, ACS_VLINE);
    }
}

// Which visible cells the active piece would occupy if hard-dropped right
// now. Mirrors tb_render's own active-piece placement (same tb_positions
// table, same bounds check) but at tb_ghost_y's row instead of the piece's
// actual row, so it never needs to touch tb_game.
static void mark_ghost(const tb_game *g, bool ghost[TB_ROWS][TB_COLS])
{
    memset(ghost, 0, TB_ROWS * TB_COLS * sizeof(bool));
    if (g->game_over) return;
    int gy = tb_ghost_y(g);
    const tb_position *p = &tb_positions[g->active.type][g->active.orientation];
    for (int i = 0; i < TB_CELLS_PER_PIECE; i++) {
        int x = g->active.origin.x + p->pos[i].x;
        int y = gy + p->pos[i].y;
        if (x >= 0 && x < TB_COLS && y >= 0 && y < TB_ROWS)
            ghost[y][x] = true;
    }
}

// A small 4x4 box for the NEXT/HOLD previews. type < 0 (empty hold, or the
// bag-boundary -1 from tb_next_piece) draws nothing but the label.
static void draw_piece_box(int top, int left, const char *label, int type, bool dim)
{
    mvprintw(top, left, "%s", label);
    if (type < 0) return;
    const tb_position *p = &tb_positions[type][0];
    if (dim) attron(A_DIM);
    for (int i = 0; i < TB_CELLS_PER_PIECE; i++) {
        int x = p->pos[i].x, y = p->pos[i].y;
        if (x >= 0 && x < 4 && y >= 0 && y < 4)
            mvprintw(top + 1 + y, left + x * CELL_W, "[]");
    }
    if (dim) attroff(A_DIM);
}

// redraws the whole screen for one frame: board (with ghost overlay),
// NEXT/HOLD side boxes, the control list, score footer, and the
// game-over/paused banner. reads the game, never mutates it.
static void draw(const tb_game *g, bool paused, const viz_t *vz)
{
    // ask the engine for the merged board, and compute the ghost overlay
    int8_t view[TB_ROWS][TB_COLS];
    tb_render(g, view);
    bool ghost[TB_ROWS][TB_COLS];
    mark_ghost(g, ghost);

    erase();
    draw_frame(BOARD_TOP, BOARD_LEFT);

    for (int y = 0; y < TB_ROWS; y++) {
        for (int x = 0; x < TB_COLS; x++) {
            int sy = BOARD_TOP + 1 + y;
            int sx = BOARD_LEFT + 1 + x * CELL_W;
            if (view[y][x] >= 0) {
                mvprintw(sy, sx, "[]");         // locked or active: full block
            } else if (ghost[y][x]) {
                attron(A_DIM);
                mvprintw(sy, sx, "[]");         // ghost: dim block
                attroff(A_DIM);
            } else {
                attron(A_DIM);
                mvprintw(sy, sx, " .");         // empty: dim dot
                attroff(A_DIM);
            }
        }
    }

    draw_piece_box(BOARD_TOP,     SIDE_LEFT, "NEXT", tb_next_piece(g), false);
    draw_piece_box(BOARD_TOP + 6, SIDE_LEFT, "HOLD", g->hold, g->held_this_turn);

    // The control list, always on screen next to the board. Kept as data so
    // the drawing loop and any future width checks share one source of truth.
    static const char *controls[] = {
        "CONTROLS",
        "left/right move",
        "down     soft drop",
        "space    hard drop",
        "up / x   rotate cw",
        "z        rotate ccw",
        "a        rotate 180",
        "c        hold",
        "v        visualizer",
        "r        retry",
        "q / esc  pause",
    };
    for (size_t i = 0; i < sizeof controls / sizeof controls[0]; i++) {
        if (i > 0) attron(A_DIM);
        mvprintw(BOARD_TOP + 12 + (int)i, SIDE_LEFT, "%s", controls[i]);
        if (i > 0) attroff(A_DIM);
    }

    int footer = BOARD_TOP + TB_ROWS + 2;
    mvprintw(footer,     BOARD_LEFT, "Score: %u  |  Lines: %u  |  Level: %u",
             g->score, g->lines_total, g->level);

    // the visualizer strip, below everything else and only when the
    // terminal has the rows for it. this frame was erase()d above, so
    // skipping the draw when toggled off also clears it.
    if (vz->enabled && vz->running &&
        footer + 2 + VIZ_STRIP_ROWS <= LINES)
        viz_draw(vz, footer + 2, COLS);

    if (g->game_over) {
        int my = BOARD_TOP + TB_ROWS / 2;
        attron(A_BOLD);
        mvprintw(my,     BOARD_LEFT + 2, "    GAME OVER     ");
        mvprintw(my + 1, BOARD_LEFT + 2, " r retry - q quit ");
        attroff(A_BOLD);
    } else if (paused) {
        int my = BOARD_TOP + TB_ROWS / 2;
        attron(A_BOLD);
        mvprintw(my,     BOARD_LEFT + 2, "      PAUSED      ");
        mvprintw(my + 1, BOARD_LEFT + 2, " q resume  r retry");
        mvprintw(my + 2, BOARD_LEFT + 2, " shift-q   quit   ");
        attroff(A_BOLD);
    }
    refresh();
}

// Map a keypress to an engine input. ERR (no key this frame) -> NONE.
// Letters accept both cases EXCEPT q/Q, which are two different commands
// (pause vs quit) and are handled in the loop, not here.
static tb_input key_to_input(int ch)
{
    switch (ch) {
    case KEY_LEFT:            return TB_INPUT_LEFT;
    case KEY_RIGHT:           return TB_INPUT_RIGHT;
    case KEY_UP:
    case 'x': case 'X':       return TB_INPUT_ROTATE_CW;
    case 'z': case 'Z':       return TB_INPUT_ROTATE_CCW;
    case 'a': case 'A':       return TB_INPUT_ROTATE_180;
    case KEY_DOWN:            return TB_INPUT_SOFT_DROP;
    case ' ':                 return TB_INPUT_HARD_DROP;
    case 'c': case 'C':       return TB_INPUT_HOLD;
    default:                  return TB_INPUT_NONE;
    }
}

// the whole offline game: bring up ncurses, seed a local engine, then run
// the fixed-rate loop -- one keypress in, one tb_tick, one redraw per
// frame -- until the player quits. this is the only mode that owns its own
// tb_game; the networked client just draws what the server broadcasts.
int tetrisu_local_run(uint32_t seed, uint32_t start_level)
{
    signal(SIGINT, on_sigint);

    // standard ncurses game setup: raw-ish keys, no echo, non-blocking
    // getch so a frame never waits for input, hidden cursor. setlocale
    // first so the visualizer can probe for utf-8 block characters.
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    set_escdelay(0);
    curs_set(0);

    // a 0 seed means "give me a different game each run"; an explicit seed
    // reproduces the exact same piece sequence (that is the determinism
    // contract, and it is how replays work). Retry follows the same rule:
    // an explicit seed replays the identical piece sequence, no seed rolls
    // a fresh one, so `--local 42` is practice mode and `--local` is arcade.
    tb_game g;
    tb_init(&g, seed ? seed : (uint32_t)time(NULL));
    tb_set_lock_delay(&g, LOCK_DELAY_FRAMES);
    if (start_level > 0)
        tb_set_start_level(&g, start_level);

    // the game is active from here: arm the visualizer. its baseline
    // animation runs off the monotonic clock only; game events reach it
    // as accents by diffing the brain's own counters between frames.
    viz_t vz;
    viz_init(&vz);
    viz_begin(&vz, now_ms());
    uint32_t viz_prev_lines = 0;
    int      viz_prev_garb  = 0;

    // ~60 Hz frame loop; the engine only advances while the game is live
    // and unpaused, but drawing continues so the banners stay up.
    // q (or esc) pauses; from pause or game over, r restarts; shift-q (or
    // q on the game-over screen) quits.
    bool paused = false;
    while (!g_quit) {
        int ch = getch();

        // v toggles the strip and is swallowed here (demoted to "no key")
        // so the engine still gets its one tick this frame and the key is
        // never seen as a game input.
        if (ch == 'v' || ch == 'V') {
            vz.enabled = !vz.enabled;
            ch = ERR;
        }

        if (ch == 'Q' || (g.game_over && (ch == 'q' || ch == 27)))
            break;
        if (ch == 'r' || ch == 'R') {
            tb_init(&g, seed ? seed : (uint32_t)time(NULL));
            tb_set_lock_delay(&g, LOCK_DELAY_FRAMES);
            if (start_level > 0)
                tb_set_start_level(&g, start_level);
            paused = false;
            viz_prev_lines = 0;      // the counters restarted with the game
            viz_prev_garb  = 0;
        } else if (!g.game_over && (ch == 'q' || ch == 27)) {
            paused = !paused;
        } else if (!g.game_over && !paused) {
            tb_tick(&g, key_to_input(ch));
        }

        // read-only deltas from the brain struct: a lines_total increase
        // is a clear, a garbage-cell increase is an injection. stepping
        // is skipped while paused so the strip freezes with the game.
        if (vz.enabled && !paused) {
            if (g.lines_total > viz_prev_lines)
                viz_accent(&vz, VIZ_ACCENT_CLEAR);
            viz_prev_lines = g.lines_total;
            int garb = count_garbage(&g);
            if (garb > viz_prev_garb)
                viz_accent(&vz, VIZ_ACCENT_GARBAGE);
            viz_prev_garb = garb;
            viz_step(&vz, now_ms() - vz.start_ms);
        }

        draw(&g, paused, &vz);
        napms(FRAME_MS);
    }

    endwin();
    return EXIT_SUCCESS;
}
