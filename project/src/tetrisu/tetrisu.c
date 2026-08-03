// tetrisu.c: playable ncurses Tetris client (dot-grid / bracket style),
// against a purely LOCAL tb_game (no sockets). Reached via `tetrisu --local`.
//
// The engine is tick-driven, so this runs a fixed ~60 Hz loop: each frame it
// reads at most one key, turns it into a tb_input (or TB_INPUT_NONE if no key
// was pressed), advances the game by one tick, and redraws. Gravity needs no
// special handling. The ticks themselves drive it.

#include <ncurses.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tetrisbrain.h"
#include "local.h"

#define FRAME_MS    16       // ~60 Hz; also the gravity time base
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
// NEXT/HOLD side boxes, score footer, and the game-over banner. reads the
// game, never mutates it.
static void draw(const tb_game *g)
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

    int footer = BOARD_TOP + TB_ROWS + 2;
    mvprintw(footer,     BOARD_LEFT, "Score: %u  |  Lines: %u  |  Level: %u",
             g->score, g->lines_total, g->level);
    mvprintw(footer + 1, BOARD_LEFT,
             "left/right move - up rotate - down soft - space drop - c hold - q quit");

    if (g->game_over) {
        int my = BOARD_TOP + TB_ROWS / 2;
        attron(A_BOLD);
        mvprintw(my,     BOARD_LEFT + 3, " GAME OVER ");
        mvprintw(my + 1, BOARD_LEFT + 3, " press q   ");
        attroff(A_BOLD);
    }
    refresh();
}

// Map a keypress to an engine input. ERR (no key this frame) -> NONE.
static tb_input key_to_input(int ch)
{
    switch (ch) {
    case KEY_LEFT:  return TB_INPUT_LEFT;
    case KEY_RIGHT: return TB_INPUT_RIGHT;
    case KEY_UP:    return TB_INPUT_ROTATE_CW;
    case 'z':      return TB_INPUT_ROTATE_CCW;
    case KEY_DOWN:  return TB_INPUT_SOFT_DROP;
    case ' ':       return TB_INPUT_HARD_DROP;
    case 'c':       return TB_INPUT_HOLD;
    default:        return TB_INPUT_NONE;
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
    // getch so a frame never waits for input, hidden cursor
    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    set_escdelay(0);
    curs_set(0);

    // a 0 seed means "give me a different game each run"; an explicit seed
    // reproduces the exact same piece sequence (that is the determinism
    // contract, and it is how replays work)
    tb_game g;
    tb_init(&g, seed ? seed : (uint32_t)time(NULL));
    if (start_level > 0)
        tb_set_start_level(&g, start_level);

    // ~60 Hz frame loop; the engine only advances while the game is live,
    // but drawing continues so the game-over banner stays up
    while (!g_quit) {
        int ch = getch();
        if (ch == 'q')
            break;

        if (!g.game_over)
            tb_tick(&g, key_to_input(ch));

        draw(&g);
        napms(FRAME_MS);
    }

    endwin();
    return EXIT_SUCCESS;
}
