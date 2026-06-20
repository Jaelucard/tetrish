// tetrisu.c — playable ncurses Tetris client (dot-grid / bracket style).
//
// The engine is tick-driven, so this runs a fixed ~60 Hz loop: each frame it
// reads at most one key, turns it into a tb_input (or TB_INPUT_NONE if no key
// was pressed), advances the game by one tick, and redraws. Gravity needs no
// special handling — the ticks themselves drive it.

#include <ncurses.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>

#include "tetrisbrain.h"

#define FRAME_MS    16       // ~60 Hz; also the gravity time base
#define CELL_W       2       // each board cell is 2 terminal columns wide
#define BOARD_TOP    1
#define BOARD_LEFT   2

static volatile sig_atomic_t g_quit = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_quit = 1;              // handled in the loop; never call endwin() here
}

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

static void draw(const tb_game *g)
{
    int8_t view[TB_ROWS][TB_COLS];
    tb_render(g, view);

    erase();
    draw_frame(BOARD_TOP, BOARD_LEFT);

    for (int y = 0; y < TB_ROWS; y++) {
        for (int x = 0; x < TB_COLS; x++) {
            int sy = BOARD_TOP + 1 + y;
            int sx = BOARD_LEFT + 1 + x * CELL_W;
            if (view[y][x] < 0) {
                attron(A_DIM);
                mvprintw(sy, sx, " .");      // empty: dim dot
                attroff(A_DIM);
            } else {
                mvprintw(sy, sx, "[]");      // filled: bracket block
            }
        }
    }

    int footer = BOARD_TOP + TB_ROWS + 2;
    mvprintw(footer,     BOARD_LEFT, "Score: %u  |  Lines: %u",
             g->score, g->lines_total);
    mvprintw(footer + 1, BOARD_LEFT,
             "left/right move - up rotate - down soft - space drop - q quit");

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
    case KEY_UP:    return TB_INPUT_ROTATE;
    case KEY_DOWN:  return TB_INPUT_SOFT_DROP;
    case ' ':       return TB_INPUT_HARD_DROP;
    default:        return TB_INPUT_NONE;
    }
}

int main(void)
{
    signal(SIGINT, on_sigint);

    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    set_escdelay(0);
    curs_set(0);

    tb_game g;
    tb_init(&g, (uint32_t)time(NULL));   // a fresh seed each run

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