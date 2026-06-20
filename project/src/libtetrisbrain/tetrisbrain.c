#include "tetrisbrain.h"
#include <string.h>
#include <assert.h>

#define BASE_SCORE_PER_ROW 10

// shape table (used tetrio for reference)

const tb_position tb_positions[TB_NUM_PIECES][TB_NUM_ORIENTATIONS] = {
    [TB_O] = {
        {{{1,1},{2,1},{1,2},{2,2}}}, {{{1,1},{2,1},{1,2},{2,2}}},
        {{{1,1},{2,1},{1,2},{2,2}}}, {{{1,1},{2,1},{1,2},{2,2}}},
    },
    [TB_I] = {
        {{{0,1},{1,1},{2,1},{3,1}}}, {{{2,0},{2,1},{2,2},{2,3}}},
        {{{0,1},{1,1},{2,1},{3,1}}}, {{{2,0},{2,1},{2,2},{2,3}}},
    },
    [TB_T] = {
        {{{2,0},{1,1},{2,1},{3,1}}}, {{{2,0},{2,1},{2,2},{3,1}}},
        {{{2,2},{1,1},{2,1},{3,1}}}, {{{2,0},{2,1},{2,2},{1,1}}},
    },
    [TB_S] = {
        {{{2,0},{1,1},{2,1},{1,2}}}, {{{1,1},{2,1},{2,2},{3,2}}},
        {{{2,0},{1,1},{2,1},{1,2}}}, {{{1,1},{2,1},{2,2},{3,2}}},
    },
    [TB_Z] = {
        {{{1,0},{1,1},{2,1},{2,2}}}, {{{1,1},{2,1},{0,2},{1,2}}},
        {{{1,0},{1,1},{2,1},{2,2}}}, {{{1,1},{2,1},{0,2},{1,2}}},
    },
    [TB_J] = {
        {{{1,0},{1,1},{1,2},{2,2}}}, {{{1,1},{2,1},{3,1},{1,2}}},
        {{{1,0},{2,0},{2,1},{2,2}}}, {{{2,0},{0,1},{1,1},{2,1}}},
    },
    [TB_L] = {
        {{{2,0},{2,1},{2,2},{1,2}}}, {{{1,0},{1,1},{2,1},{3,1}}},
        {{{1,0},{2,0},{1,1},{1,2}}}, {{{0,1},{1,1},{2,1},{2,2}}},
    },
};

// spawn origin in play coords; centers the 4-wide shape box on the board.
static const tb_point SPAWN_ORIGIN = {3, 0};

// lets go gambling (with block randomisation)

static uint32_t tb_rand(tb_game *g)
{
    uint32_t x = g->rng_state;        // xorshift32; never zero (see tb_init)
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return g->rng_state = x;
}

static void refill_bag(tb_game *g)
{
    for (uint8_t i = 0; i < TB_NUM_PIECES; i++) g->bag[i] = i;
    for (uint8_t i = TB_NUM_PIECES - 1; i > 0; i--) {   // Fisher-Yates
        uint8_t j = (uint8_t)(tb_rand(g) % (uint32_t)(i + 1));
        uint8_t t = g->bag[i]; g->bag[i] = g->bag[j]; g->bag[j] = t;
    }
    g->bag_index = 0;
}

static tb_piece_type next_from_bag(tb_game *g)
{
    if (g->bag_index >= TB_NUM_PIECES) refill_bag(g);
    return (tb_piece_type)g->bag[g->bag_index++];
}

// board setup and collision detection

static void reset_board(tb_game *g)
{
    memset(g->cells, TB_CELL_EMPTY, sizeof(g->cells));
    for (int x = 0; x < TB_BW; x++) {                 // top + bottom walls
        g->cells[0][x]         = TB_CELL_WALL;
        g->cells[TB_BH - 1][x] = TB_CELL_WALL;
    }
    for (int y = 0; y < TB_BH; y++) {                 // left + right walls
        g->cells[y][0]         = TB_CELL_WALL;
        g->cells[y][TB_BW - 1] = TB_CELL_WALL;
    }
}

bool tb_block_fits(const tb_game *g, const tb_block *b)
{
    const tb_position *p = &tb_positions[b->type][b->orientation % TB_NUM_ORIENTATIONS];
    for (int i = 0; i < TB_CELLS_PER_PIECE; i++) {
        /* +1 folds in the border offset; any out-of-play coordinate lands on
         * a wall cell, so no explicit bounds check is required. */
        int bx = b->origin.x + p->pos[i].x + 1;
        int by = b->origin.y + p->pos[i].y + 1;
        if (g->cells[by][bx] != TB_CELL_EMPTY)
            return false;
    }
    return true;
}

// move, rotate, and drop functions

typedef enum { MV_LEFT, MV_RIGHT, MV_DOWN, MV_DROP, MV_ROTATE } tb_action;

static bool move_block(tb_game *g, tb_action a)
{
    tb_block nb = g->active;
    switch (a) {
    case MV_LEFT:   nb.origin.x--; break;
    case MV_RIGHT:  nb.origin.x++; break;
    case MV_DOWN:   nb.origin.y++; break;
    case MV_DROP:
        do { nb.origin.y++; } while (tb_block_fits(g, &nb));
        nb.origin.y--;                              // back off one row
        g->active = nb;                             // a drop always commits
        return true;
    case MV_ROTATE:
        nb.orientation = (uint8_t)((nb.orientation + TB_NUM_ORIENTATIONS - 1)
                                   % TB_NUM_ORIENTATIONS);  // counter-clockwise
        break;
    }
    if (!tb_block_fits(g, &nb)) return false;       // add wall kicks/srs shit here eventually
    g->active = nb;
    return true;
}

// freeze + line clear functions

static void freeze_block(tb_game *g)
{
    const tb_position *p = &tb_positions[g->active.type][g->active.orientation];
    for (int i = 0; i < TB_CELLS_PER_PIECE; i++) {
        int bx = g->active.origin.x + p->pos[i].x + 1;
        int by = g->active.origin.y + p->pos[i].y + 1;
        assert(g->cells[by][bx] == TB_CELL_EMPTY);
        g->cells[by][bx] = (int8_t)(g->active.type + 1);   // 1..7, nonzero
    }
}

static int clear_lines(tb_game *g)
{
    int cleared = 0;
    for (int y = TB_ROWS; y >= 1; y--) {            // play rows 1..TB_ROWS
        bool full = true;
        for (int x = 1; x <= TB_COLS; x++)
            if (g->cells[y][x] == TB_CELL_EMPTY) { full = false; break; }
        if (!full) continue;

        for (int yy = y; yy > 1; yy--)              // shift everything down
            memcpy(&g->cells[yy][1], &g->cells[yy - 1][1], TB_COLS * sizeof(int8_t));
        memset(&g->cells[1][1], TB_CELL_EMPTY, TB_COLS * sizeof(int8_t));

        cleared++;
        y++;                                        // re-check this row index
    }
    return cleared;
}

static void score_lines(tb_game *g, int rows)
{
    g->score += (uint32_t)g->level * (uint32_t)(rows * rows) * BASE_SCORE_PER_ROW;
    g->lines_total += (uint32_t)rows;
    g->lines_since_level = (uint8_t)(g->lines_since_level + rows);
    while (g->lines_since_level >= TB_LINES_PER_LEVEL) {
        g->lines_since_level = (uint8_t)(g->lines_since_level - TB_LINES_PER_LEVEL);
        g->level++;
        g->gravity_period = (g->gravity_period > TB_GRAVITY_FLOOR + TB_GRAVITY_DELTA)
                          ? g->gravity_period - TB_GRAVITY_DELTA : TB_GRAVITY_FLOOR;
    }
}

void tb_spawn(tb_game *g, tb_piece_type type)
{
    g->active.type        = type;
    g->active.orientation = 0;
    g->active.origin      = SPAWN_ORIGIN;
    if (!tb_block_fits(g, &g->active)) g->game_over = true;
}

static void lock_and_next(tb_game *g)
{
    freeze_block(g);
    int rows = clear_lines(g);
    if (rows) score_lines(g, rows);
    tb_spawn(g, next_from_bag(g));
    g->ticks_since_grav = 0;
}

// game cycle (idk what else to call it lol)

void tb_init(tb_game *g, uint32_t seed)
{
    memset(g, 0, sizeof(*g));                       // zero incl. padding
    reset_board(g);
    g->level          = 1;
    g->gravity_period = TB_GRAVITY_INITIAL;
    g->rng_state      = seed ? seed : 0x9E3779B9u;  // xorshift32 must be != 0
    g->bag_index      = TB_NUM_PIECES;              // force refill on first draw
    tb_spawn(g, next_from_bag(g));
}

bool tb_tick(tb_game *g, tb_input input)
{
    if (g->game_over) return false;
    g->tick_count++;

    switch (input) {
    case TB_INPUT_NONE:                                  break;
    case TB_INPUT_LEFT:   move_block(g, MV_LEFT);        break;
    case TB_INPUT_RIGHT:  move_block(g, MV_RIGHT);       break;
    case TB_INPUT_ROTATE: move_block(g, MV_ROTATE);      break;
    case TB_INPUT_SOFT_DROP:
        if (move_block(g, MV_DOWN)) g->ticks_since_grav = 0;
        else                        lock_and_next(g);
        return !g->game_over;
    case TB_INPUT_HARD_DROP:
        move_block(g, MV_DROP);
        lock_and_next(g);
        return !g->game_over;
    }

    // tick-driven gravity: step down once every gravity_period ticks.
    if (++g->ticks_since_grav >= g->gravity_period) {
        g->ticks_since_grav = 0;
        if (!move_block(g, MV_DOWN)) lock_and_next(g);
    }
    return !g->game_over;
}

void tb_render(const tb_game *g, int8_t out[TB_ROWS][TB_COLS])
{
    for (int y = 0; y < TB_ROWS; y++)
        for (int x = 0; x < TB_COLS; x++) {
            int8_t c = g->cells[y + 1][x + 1];      // strip the border offset
            out[y][x] = (c == TB_CELL_EMPTY) ? -1 : (int8_t)(c - 1);
        }
    if (g->game_over) return;

    const tb_position *p = &tb_positions[g->active.type][g->active.orientation];
    for (int i = 0; i < TB_CELLS_PER_PIECE; i++) {
        int x = g->active.origin.x + p->pos[i].x;   // play coords
        int y = g->active.origin.y + p->pos[i].y;
        if (x >= 0 && x < TB_COLS && y >= 0 && y < TB_ROWS)
            out[y][x] = (int8_t)g->active.type;
    }
}