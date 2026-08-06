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
#define TB_CELL_GARBAGE 0x7E   // Battle Royale garbage. Distinct from both the
                               // 1..7 locked-piece values and from TB_CELL_WALL,
                               // so a renderer can draw it differently.
// A locked cell stores (piece_type + 1), i.e. 1..7.

#define TB_NUM_PIECES       7
#define TB_NUM_ORIENTATIONS 4
#define TB_CELLS_PER_PIECE  4

#define TB_GRAVITY_INITIAL 48u   // ticks per gravity step at level 1
#define TB_GRAVITY_FLOOR    4u
#define TB_GRAVITY_DELTA    4u
#define TB_LINES_PER_LEVEL 10u

/* Lock delay, using the move-reset rule: once a piece is resting on something,
 * it gets a grace period before it locks, and moving or rotating it restarts
 * that period. Without a cap a player could hold a piece in place forever, so
 * the number of restarts is limited.
 *
 * The delay is counted in TICKS, not milliseconds, because this library has no
 * clock. The target is 500 ms, which is 10 ticks at the default
 * tick_hz of 20. The caller decides: tetrisd calls tb_set_lock_delay with
 * tick_hz / 2 so the wall-clock feel stays at half a second whatever the
 * configured tick rate. */
#define TB_LOCK_DELAY_TICKS 10u   /* 500 ms at tick_hz 20 */
#define TB_LOCK_MAX_RESETS  15u

// piece order: O, I, T, S, Z, J, L. This indexes the shape table.
typedef enum {
    TB_O, TB_I, TB_T, TB_S, TB_Z, TB_J, TB_L
} tb_piece_type;

typedef enum {
    TB_INPUT_NONE,
    TB_INPUT_LEFT,
    TB_INPUT_RIGHT,
    TB_INPUT_ROTATE_CCW,   // both directions now exist, and both try SRS kicks
    TB_INPUT_ROTATE_CW,
    TB_INPUT_SOFT_DROP,
    TB_INPUT_HARD_DROP,
    TB_INPUT_HOLD,         // appended: never sent by old replay logs
    TB_INPUT_ROTATE_180    // appended for the same reason: two orientation
                           // steps in one input, with its own small kick set
} tb_input;

// Why the game ended, distinct from the boolean game_over. Both causes leave
// the board in a legitimate state; the cause is only for display/logging.
typedef enum {
    TB_OVER_NONE,
    TB_OVER_BLOCKOUT,   // a freshly spawned piece had nowhere to go
    TB_OVER_TOPOUT      // garbage rose into the active piece with no room to lift it
} tb_over_cause;

// How the engine recognised the most recent lock. Scoring keys off this, and a
// frontend can use it to name the clear on screen. A "mini" is a t-spin into a
// shallow slot: it satisfies the 3-corner rule but got there on a kick without
// filling both corners of the T's pointing face, so it pays roughly a quarter.
typedef enum {
    TB_CLEAR_NORMAL,
    TB_CLEAR_TSPIN,
    TB_CLEAR_TSPIN_MINI
} tb_clear_kind;

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

    uint32_t lock_delay_ticks;   // grace period once the piece is resting
    uint32_t lock_timer;         // ticks spent resting since the last reset
    uint8_t  lock_resets;        // how many times the player has restarted it

    uint32_t rng_state;          // xorshift32, part of the game state
    uint8_t  bag[TB_NUM_PIECES];
    uint8_t  bag_index;

    // Points awarded per line clear, indexed by (rows - 1): single, double,
    // triple, quad. Multiplied by level. tb_init sets the guideline values;
    // exposed as game state (not a #define) so a future variant ruleset can
    // override it without an API change.
    uint16_t tetris_points[4];

    tb_over_cause over_cause;    // TB_OVER_NONE until game_over becomes true

    // Hold: stash the active piece and swap in the held one (or the next bag
    // piece if nothing is held yet). held_this_turn blocks a second hold
    // before the next lock, which stops a piece cycling between hold and
    // play forever.
    int8_t hold;                 // -1 = empty, else a tb_piece_type
    bool   held_this_turn;

    uint32_t start_level;        // level tb_set_start_level was called with

    /* Guideline scoring state. Everything from here down is APPENDED: tetrisd
     * links this engine directly and snapshots tb_game by memcpy, so the
     * fields above must keep both their order and their offsets. */

    // Line-clearing pieces immediately BEFORE this one, so the first clear of
    // a chain pays no combo and the second pays one unit. A piece that clears
    // nothing resets it to zero.
    uint32_t combo_count;
    // Consecutive "difficult" clears, meaning a quad or a t-spin that cleared
    // lines. Nonzero is what arms the back-to-back multiplier. A piece that
    // clears nothing leaves this alone; an ordinary clear zeroes it.
    uint32_t b2b_count;

    // The t-spin test needs to know how the piece reached the square it locked
    // on: only a rotation qualifies, and whether that rotation needed a kick is
    // what separates a mini from a full spin. Both reset on every spawn.
    bool last_was_rotation;
    bool last_rotation_kicked;

    tb_clear_kind last_clear_kind;   // what the most recent lock was recognised as

    // Bonus values, game state rather than #defines for the same reason
    // tetris_points is: a variant ruleset can retune them without an API change.
    uint16_t combo_points;           // per combo step, times level
    uint16_t perfect_clear_points;   // board left completely empty by a clear
    uint16_t tspin_points[4];        // t-spin bonus, indexed by rows cleared 0..3
    uint16_t tspin_mini_points[3];   // mini bonus, indexed by rows cleared 0..2
    uint8_t  soft_drop_points;       // per cell descended, NOT times level
    uint8_t  hard_drop_points;       // per cell descended, NOT times level
} tb_game;

extern const tb_position tb_positions[TB_NUM_PIECES][TB_NUM_ORIENTATIONS];

// Initialize a game in-place with the given seed and spawn the first piece.
// If seed is 0, a default nonzero seed is used.
void tb_init(tb_game *g, uint32_t seed);

/* Set the lock delay in ticks. tb_init leaves it at TB_LOCK_DELAY_TICKS, which
 * is correct for tick_hz 20; a caller running a different tick rate should pass
 * tick_hz / 2 to keep the wall-clock delay at 500 ms. Passing 0 disables lock
 * delay entirely, which restores the older behaviour of locking the instant
 * gravity cannot move the piece down. */
void tb_set_lock_delay(tb_game *g, uint32_t ticks);

/* Set the starting level. Recomputes gravity_period from the same formula
 * tb_init and level-ups use, so calling this mid-game re-derives gravity for
 * the new level rather than just overwriting the counter. Call it right after
 * tb_init, before any ticks, for a configured starting level; calling it
 * later is legal but will look like a sudden speed change. */
void tb_set_start_level(tb_game *g, uint32_t level);

bool tb_tick(tb_game *g, tb_input input);
void tb_spawn(tb_game *g, tb_piece_type type);
bool tb_block_fits(const tb_game *g, const tb_block *b);
void tb_render(const tb_game *g, int8_t out[TB_ROWS][TB_COLS]);

// The row the active piece would land on if hard-dropped right now. Pure:
// takes no lock, leaves the game unchanged. For rendering a ghost piece.
int tb_ghost_y(const tb_game *g);

// The next piece to be drawn from the bag, without drawing it. Pure: never
// refills the bag, so it returns -1 once the current bag window is spent
// (rather than rolling the rng to peek past it). Callers that only peek
// right after a spawn will never see -1, since a spawn either draws the last
// bag slot (leaving nothing to peek) or an earlier one.
int tb_next_piece(const tb_game *g);

//Battle Royale garbage. This pushes the locked stack up by `rows` and inserts
// that many garbage rows at the bottom.

// hole_pattern is a bitmask over the 10 playfield columns: bit x set means
// column x is left empty in every inserted row. A row with no hole would be
// instantly full and clear itself, and a row with every column holed would be
// empty, so a pattern that is 0 or has all ten bits set is replaced by a single
// hole in column 0. The same pattern is used for every row in one injection,
// which is the usual "clean garbage" behaviour.

// Anything shifted above the top of the playfield is lost, and if the active
// piece no longer fits after the shift the game is over.

// This stays inside the design contract at the top of this file: no time, no
// randomness, no I/O. The caller supplies the hole pattern, so two engines fed
// the same injections stay bit-identical which is what makes replay work.
void tb_inject_garbage(tb_game *g, int rows, uint16_t hole_pattern);

#endif