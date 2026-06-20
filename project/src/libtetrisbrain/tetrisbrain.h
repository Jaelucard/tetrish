/*
 * Design contract:
 *   1. No wall-clock time. The caller advances the game by calling tb_tick().
 *      Gravity is counted in ticks, never in real seconds. Two games started
 *      with the same seed and fed the same input sequence stay bit-identical.
 *   2. No hidden globals and no rand(). All randomness comes from a seeded
 *      xorshift32 PRNG stored inside tb_game, feeding a 7-bag randomizer.
 *   3. No I/O. tb_render() composites the locked stack plus the active piece
 *      into a plain array for whatever frontend wants to draw it.
 *
 * tb_game is a flat value type (no internal pointers), so it can be memcpy'd
 * as a snapshot, sent over the wire, or compared with memcmp.
 *
 * Collision uses a sentinel-bordered board: the playfield is wrapped in a
 * permanent ring of wall cells, so checking whether a piece fits is a single
 * occupancy read with no bounds-checking branch.
 */

#ifndef TETRISBRAIN_H
#define TETRISBRAIN_H

#include <stdint.h>
#include <stdbool.h>


// Visible playfield.
#define TB_ROWS 20
#define TB_COLS 10

/* Internal board carries a 1-cell sentinel border on every side: indices
 * [1..TB_ROWS][1..TB_COLS] are play cells; everything else is wall. */
#define TB_BH (TB_ROWS + 2)
#define TB_BW (TB_COLS + 2)

// Cell encoding. 0 = empty so the collision test "if (cells[y][x])" works
 
#define TB_CELL_EMPTY 0
#define TB_CELL_WALL  0x7F
// A locked cell stores (piece_type + 1), i.e. 1..7.

#define TB_NUM_PIECES       7
#define TB_NUM_ORIENTATIONS 4
#define TB_CELLS_PER_PIECE  4

#define TB_GRAVITY_INITIAL 48u   // ticks per gravity step at level 1
#define TB_GRAVITY_FLOOR    4u
#define TB_GRAVITY_DELTA    4u
#define TB_LINES_PER_LEVEL 10u

// piece order: O, I, T, S, Z, J, L. This indexes the shape table.
typedef enum {
    TB_O, TB_I, TB_T, TB_S, TB_Z, TB_J, TB_L
} tb_piece_type;

typedef enum {
    TB_INPUT_NONE,
    TB_INPUT_LEFT,
    TB_INPUT_RIGHT,
    TB_INPUT_ROTATE,     // counter-clockwise; no wall kicks yet
    TB_INPUT_SOFT_DROP,
    TB_INPUT_HARD_DROP
} tb_input;

// cell offsets use an {x, y} convention: x is column, y is row.
typedef struct { int8_t x, y; } tb_point;
typedef struct { tb_point pos[TB_CELLS_PER_PIECE]; } tb_position;

typedef struct {
    tb_piece_type type;
    tb_point      origin;        // top-left of the piece box, play coords
    uint8_t       orientation;   // 0..3
} tb_block;

typedef struct {
    // The active piece is NOT written here until it locks
    int8_t cells[TB_BH][TB_BW];

    tb_block active;
    bool     game_over;

    uint32_t score;
    uint32_t level;              // starts at 1
    uint32_t lines_total;
    uint8_t  lines_since_level;

    uint32_t gravity_period;     // ticks between gravity steps
    uint32_t ticks_since_grav;
    uint64_t tick_count;

    uint32_t rng_state;          // xorshift32 — part of the game state
    uint8_t  bag[TB_NUM_PIECES];
    uint8_t  bag_index;
} tb_game;

extern const tb_position tb_positions[TB_NUM_PIECES][TB_NUM_ORIENTATIONS];

/* Initialize a game in-place with the given seed and spawn the first piece.
 * If seed is 0, a default nonzero seed is used. */
void tb_init(tb_game *g, uint32_t seed);
bool tb_tick(tb_game *g, tb_input input);
void tb_spawn(tb_game *g, tb_piece_type type);
bool tb_block_fits(const tb_game *g, const tb_block *b);
void tb_render(const tb_game *g, int8_t out[TB_ROWS][TB_COLS]);


#endif 