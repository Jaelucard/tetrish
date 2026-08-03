#include "tetrisbrain.h"
#include <string.h>
#include <assert.h>

// classic single/double/triple/tetris point values, before the level multiplier
static const uint16_t TETRIS_POINTS_DEFAULT[4] = {40, 100, 300, 1200};

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

// advances the in-game prng one step and returns the new value. the state
// lives inside tb_game (never a global), which is what keeps two games with
// the same seed bit-identical.
static uint32_t tb_rand(tb_game *g)
{
    uint32_t x = g->rng_state;        // xorshift32; never zero (see tb_init)
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return g->rng_state = x;
}

// refills the 7-bag with one of each piece in random order. drawing from a
// shuffled bag (instead of rolling a die per piece) guarantees every piece
// appears exactly once per 7 draws -- no long droughts.
static void refill_bag(tb_game *g)
{
    for (uint8_t i = 0; i < TB_NUM_PIECES; i++) g->bag[i] = i;
    for (uint8_t i = TB_NUM_PIECES - 1; i > 0; i--) {   // Fisher-Yates
        uint8_t j = (uint8_t)(tb_rand(g) % (uint32_t)(i + 1));
        uint8_t t = g->bag[i]; g->bag[i] = g->bag[j]; g->bag[j] = t;
    }
    g->bag_index = 0;
}

// draws the next piece from the bag, refilling it first if all 7 are spent.
static tb_piece_type next_from_bag(tb_game *g)
{
    if (g->bag_index >= TB_NUM_PIECES) refill_bag(g);
    return (tb_piece_type)g->bag[g->bag_index++];
}

// board setup and collision detection

// clears the playfield and rebuilds the one-cell sentinel wall around it.
// the wall is what lets tb_block_fits treat "off the board" and "occupied"
// as the same single array read.
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

// the one collision test everything else builds on: true if every cell of
// piece `b` lands on an empty play cell. walls, locked pieces, garbage and
// out-of-array coordinates all read as "does not fit".
bool tb_block_fits(const tb_game *g, const tb_block *b)
{
    const tb_position *p = &tb_positions[b->type][b->orientation % TB_NUM_ORIENTATIONS];
    for (int i = 0; i < TB_CELLS_PER_PIECE; i++) {
        /* +1 folds in the border offset; any coordinate within one cell of
         * the play area lands on a wall cell, so most callers never need an
         * explicit bounds check. Rotation kicks are the exception: an SRS
         * kick can push origin.y two rows above row 0 while testing a
         * candidate, and indexing cells[] beyond the sentinel border is
         * undefined behaviour, not a guaranteed wall read (confirmed by
         * AddressSanitizer during 20k-tick fuzzing). Anything that lands
         * outside the array is therefore rejected explicitly, same as a
         * wall cell would be. */
        int bx = b->origin.x + p->pos[i].x + 1;
        int by = b->origin.y + p->pos[i].y + 1;
        if (by < 0 || by >= TB_BH || bx < 0 || bx >= TB_BW) return false;
        if (g->cells[by][bx] != TB_CELL_EMPTY)
            return false;
    }
    return true;
}

// SRS wall kicks
//
// A plain rotation often fails next to a wall or a stack. SRS says: do not give
// up, try the rotation again at up to four nearby offsets and take the first
// that fits. That is what lets a piece "kick" off a wall, and it is what makes
// T-spins possible.
//
// These tables are specification data from the Super Rotation System, not
// something derived here. Five tests per transition, the first always (0,0)
// which is the plain rotation.
//
// ONE CONVERSION MATTERS. The published tables use a y axis that points UP, so
// their +1 means up and -2 means down two. This engine's y points DOWN: row 1
// is the top of the playfield and move_block advances origin.y to fall. So
// every y value below is negated when it is applied. Get this wrong and the
// kicks push pieces through the floor instead of off the wall.
//
// Indexed by the orientation being rotated FROM. The O piece never kicks: all
// four of its orientations are identical, so a rotation that fits at (0,0)
// always fits, and one that does not cannot be helped by an offset.
typedef struct { int8_t dx, dy; } tb_kick;

static const tb_kick kicks_JLSTZ_cw[TB_NUM_ORIENTATIONS][5] = {
    /* 0 -> R */ {{0,0}, {-1,0}, {-1,+1}, {0,-2}, {-1,-2}},
    /* R -> 2 */ {{0,0}, {+1,0}, {+1,-1}, {0,+2}, {+1,+2}},
    /* 2 -> L */ {{0,0}, {+1,0}, {+1,+1}, {0,-2}, {+1,-2}},
    /* L -> 0 */ {{0,0}, {-1,0}, {-1,-1}, {0,+2}, {-1,+2}},
};
static const tb_kick kicks_JLSTZ_ccw[TB_NUM_ORIENTATIONS][5] = {
    /* 0 -> L */ {{0,0}, {+1,0}, {+1,+1}, {0,-2}, {+1,-2}},
    /* R -> 0 */ {{0,0}, {+1,0}, {+1,-1}, {0,+2}, {+1,+2}},
    /* 2 -> R */ {{0,0}, {-1,0}, {-1,+1}, {0,-2}, {-1,-2}},
    /* L -> 2 */ {{0,0}, {-1,0}, {-1,-1}, {0,+2}, {-1,+2}},
};
// The I piece is long enough that it needs its own, wider offsets.
static const tb_kick kicks_I_cw[TB_NUM_ORIENTATIONS][5] = {
    /* 0 -> R */ {{0,0}, {-2,0}, {+1,0}, {-2,-1}, {+1,+2}},
    /* R -> 2 */ {{0,0}, {-1,0}, {+2,0}, {-1,+2}, {+2,-1}},
    /* 2 -> L */ {{0,0}, {+2,0}, {-1,0}, {+2,+1}, {-1,-2}},
    /* L -> 0 */ {{0,0}, {+1,0}, {-2,0}, {+1,-2}, {-2,+1}},
};
static const tb_kick kicks_I_ccw[TB_NUM_ORIENTATIONS][5] = {
    /* 0 -> L */ {{0,0}, {-1,0}, {+2,0}, {-1,+2}, {+2,-1}},
    /* R -> 0 */ {{0,0}, {+2,0}, {-1,0}, {+2,+1}, {-1,-2}},
    /* 2 -> R */ {{0,0}, {+1,0}, {-2,0}, {+1,-2}, {-2,+1}},
    /* L -> 2 */ {{0,0}, {-2,0}, {+1,0}, {-2,-1}, {+1,+2}},
};

// move, rotate, and drop functions

typedef enum { MV_LEFT, MV_RIGHT, MV_DOWN, MV_DROP,
               MV_ROTATE_CW, MV_ROTATE_CCW } tb_action;

// Is the piece resting on something? Used by the lock delay. Trying the move on
// a copy rather than committing it keeps this side-effect free.
static bool is_grounded(const tb_game *g)
{
    tb_block nb = g->active;
    nb.origin.y++;                       // at most one row outside the play
    return !tb_block_fits(g, &nb);       // area, which the sentinel border covers
}

// rotates the active piece with srs wall kicks: try the plain rotation
// first, then up to four table offsets, committing the first candidate
// that fits. returns false (piece untouched) if all five are blocked.
static bool try_rotate(tb_game *g, bool clockwise)
{
    tb_block nb = g->active;
    uint8_t from = (uint8_t)(nb.orientation % TB_NUM_ORIENTATIONS);
    nb.orientation = clockwise
        ? (uint8_t)((from + 1) % TB_NUM_ORIENTATIONS)
        : (uint8_t)((from + TB_NUM_ORIENTATIONS - 1) % TB_NUM_ORIENTATIONS);

    // The O piece is rotationally symmetric in this shape table, so kicking it
    // would only shove it sideways for no visual reason.
    if (g->active.type == TB_O) {
        if (!tb_block_fits(g, &nb)) return false;
        g->active = nb;
        return true;
    }

    const tb_kick (*table)[5] = (g->active.type == TB_I)
        ? (clockwise ? kicks_I_cw : kicks_I_ccw)
        : (clockwise ? kicks_JLSTZ_cw : kicks_JLSTZ_ccw);

    for (int t = 0; t < 5; t++) {
        tb_block cand = nb;
        cand.origin.x = (int8_t)(cand.origin.x + table[from][t].dx);
        cand.origin.y = (int8_t)(cand.origin.y - table[from][t].dy);  // y is flipped
        if (tb_block_fits(g, &cand)) {
            g->active = cand;
            return true;
        }
    }
    return false;                        // all five tests blocked
}

// applies one movement action to the active piece, committing it only if
// the destination fits. returns whether the piece actually moved -- the
// lock delay's move-reset rule keys off that.
static bool move_block(tb_game *g, tb_action a)
{
    tb_block nb = g->active;
    switch (a) {
    case MV_LEFT:   nb.origin.x--; break;
    case MV_RIGHT:  nb.origin.x++; break;
    case MV_DOWN:   nb.origin.y++; break;
    case MV_DROP:
        // slide down until blocked; a drop cannot fail, it just may not move
        do { nb.origin.y++; } while (tb_block_fits(g, &nb));
        nb.origin.y--;                              // back off one row
        g->active = nb;                             // a drop always commits
        return true;
    case MV_ROTATE_CW:  return try_rotate(g, true);
    case MV_ROTATE_CCW: return try_rotate(g, false);
    }
    // plain left/right/down: commit only if the shifted position is legal
    if (!tb_block_fits(g, &nb)) return false;
    g->active = nb;
    return true;
}

// freeze + line clear functions

// writes the active piece permanently into the board cells. until this
// moment the piece exists only in g->active; after it, the cells own it.
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

// removes every full row, shifting everything above it down, and returns
// how many rows were cleared (0..4) so the caller can score them.
static int clear_lines(tb_game *g)
{
    int cleared = 0;
    for (int y = TB_ROWS; y >= 1; y--) {            // play rows 1..TB_ROWS
        // a row is full when no play cell in it is empty
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

// awards points for a clear (classic table value times level), then
// advances the level every TB_LINES_PER_LEVEL lines, speeding up gravity
// each time it does.
static void score_lines(tb_game *g, int rows)
{
    // rows is always 1..4: clear_lines re-checks one row at a time and this
    // engine's pieces cannot span more than 4 rows.
    g->score += (uint32_t)g->tetris_points[rows - 1] * g->level;
    g->lines_total += (uint32_t)rows;
    g->lines_since_level = (uint8_t)(g->lines_since_level + rows);
    // level up once per full batch of lines, shaving the gravity period
    // down toward its floor
    while (g->lines_since_level >= TB_LINES_PER_LEVEL) {
        g->lines_since_level = (uint8_t)(g->lines_since_level - TB_LINES_PER_LEVEL);
        g->level++;
        g->gravity_period = (g->gravity_period > TB_GRAVITY_FLOOR + TB_GRAVITY_DELTA)
                          ? g->gravity_period - TB_GRAVITY_DELTA : TB_GRAVITY_FLOOR;
    }
}

// battle royale garbage: shifts the locked stack up and inserts `rows`
// garbage rows at the bottom, each with holes where hole_pattern has bits
// set. ends the game (topout) if the active piece gets buried. full
// contract is documented above the declaration in tetrisbrain.h.
void tb_inject_garbage(tb_game *g, int rows, uint16_t hole_pattern)
{
    // nothing to do on a finished game; clamp silly row counts
    if (g->game_over || rows <= 0) return;
    if (rows > TB_ROWS) rows = TB_ROWS;

    // A row with no hole clears itself the instant it lands, and a row with
    // every column holed is not garbage at all, so both degenerate patterns
    // fall back to a single hole in column 0.
    uint16_t all = (uint16_t)((1u << TB_COLS) - 1u);
    if ((hole_pattern & all) == 0 || (hole_pattern & all) == all)
        hole_pattern = 1u;

    // Shift the locked stack up by `rows`. Play rows are 1..TB_ROWS with row 1
    // at the top, so "up" means copying each row to a LOWER index. We walk
    // top-down and read from y + rows, which is always a row we have not
    // written yet. Anything that would land above row 1 is pushed off the
    // playfield and lost. That is what makes garbage dangerous.
    for (int y = 1; y <= TB_ROWS - rows; y++)
        memcpy(&g->cells[y][1], &g->cells[y + rows][1],
               TB_COLS * sizeof(int8_t));

    // Write the garbage into the bottom `rows` rows.
    for (int y = TB_ROWS - rows + 1; y <= TB_ROWS; y++)
        for (int x = 1; x <= TB_COLS; x++)
            g->cells[y][x] = (hole_pattern & (1u << (x - 1)))
                           ? TB_CELL_EMPTY
                           : TB_CELL_GARBAGE;

    // The active piece is not stored in cells[] until it locks, so the shift
    // cannot have overwritten it. The stack may have risen into it though, so
    // lift it until it fits again. If there is nowhere left, the player is
    // buried and the game ends.
    //
    // The lift goes one row at a time and stops at row 0, rather than jumping
    // straight to (origin.y minus rows). That is not caution for its own sake.
    // tb_block_fits does no bounds checking, relying instead on the one-cell
    // sentinel border to catch out-of-play coordinates. An origin more than one
    // row above the playfield indexes cells[] before the start of the array,
    // which is undefined behaviour that happens to read zeros. The piece then
    // appears to fit, and a buried player never dies.
    if (!tb_block_fits(g, &g->active)) {
        bool placed = false;
        for (int lift = 1; lift <= rows && !placed; lift++) {
            int ny = g->active.origin.y - lift;
            if (ny < 0) break;                  // top of the playfield reached
            tb_block cand = g->active;
            cand.origin.y = (int8_t)ny;
            if (tb_block_fits(g, &cand)) { g->active = cand; placed = true; }
        }
        if (!placed) {
            g->game_over  = true;
            g->over_cause = TB_OVER_TOPOUT;
        }
    }
}

// Swap the active piece into hold, bringing in whatever was held (or the next
// bag piece, the first time hold is used). Returns false and does nothing if
// hold was already used this turn.
static bool do_hold(tb_game *g)
{
    if (g->held_this_turn) return false;
    tb_piece_type incoming = (g->hold < 0) ? next_from_bag(g) : (tb_piece_type)g->hold;
    g->hold           = (int8_t)g->active.type;
    g->held_this_turn = true;
    tb_spawn(g, incoming);
    return true;
}

// places a new active piece at the spawn position in its default
// orientation. if the spawn box is already occupied the game is over
// (blockout) -- the classic "stack reached the top" death.
void tb_spawn(tb_game *g, tb_piece_type type)
{
    g->active.type        = type;
    g->active.orientation = 0;
    g->active.origin      = SPAWN_ORIGIN;
    if (!tb_block_fits(g, &g->active)) {
        g->game_over  = true;
        g->over_cause = TB_OVER_BLOCKOUT;
    }
}

// the end of one piece's life: freeze it into the board, clear and score
// any full rows, spawn the next piece, and reset all the per-piece state
// (gravity phase, lock delay allowance, hold availability).
static void lock_and_next(tb_game *g)
{
    freeze_block(g);
    int rows = clear_lines(g);
    if (rows) score_lines(g, rows);
    tb_spawn(g, next_from_bag(g));
    g->ticks_since_grav = 0;
    // The new piece gets a fresh allowance. Carrying the old piece's spent
    // resets over would make the second piece in a stack lock almost instantly.
    g->lock_timer      = 0;
    g->lock_resets     = 0;
    g->held_this_turn  = false;   // hold is available again for the new piece
}

// game cycle (idk what else to call it lol)

// sets up a fresh game: zeroed state, walled board, level 1 defaults,
// seeded prng, and the first piece already spawned. zeroing the whole
// struct first (padding included) is what makes two same-seed games
// memcmp-identical.
void tb_init(tb_game *g, uint32_t seed)
{
    memset(g, 0, sizeof(*g));                       // zero incl. padding
    reset_board(g);
    g->level          = 1;
    g->gravity_period = TB_GRAVITY_INITIAL;
    g->rng_state      = seed ? seed : 0x9E3779B9u;  // xorshift32 must be != 0
    g->bag_index      = TB_NUM_PIECES;              // force refill on first draw
    g->lock_delay_ticks = TB_LOCK_DELAY_TICKS;      // 500 ms at tick_hz 20
    memcpy(g->tetris_points, TETRIS_POINTS_DEFAULT, sizeof(g->tetris_points));
    g->hold           = -1;                         // memset already zeroed held_this_turn
    g->start_level    = g->level;
    tb_spawn(g, next_from_bag(g));
}

// reconfigures the lock-delay length (in ticks; 0 disables it) and resets
// the running timer so the change applies cleanly from the next tick.
void tb_set_lock_delay(tb_game *g, uint32_t ticks)
{
    g->lock_delay_ticks = ticks;
    g->lock_timer       = 0;
    g->lock_resets      = 0;
}

// jumps the game to a chosen starting level, recomputing gravity to what
// it would be had the player leveled up into it naturally. kept separate
// from tb_init so tb_init's signature never changes (append-only api).
void tb_set_start_level(tb_game *g, uint32_t level)
{
    g->level          = level;
    g->start_level    = level;
    g->lines_since_level = 0;
    // Same formula tb_init and score_lines' level-up use, just run backwards
    // from level 1 instead of counting down one level-up at a time.
    uint32_t drop = (level > 1) ? (level - 1) * TB_GRAVITY_DELTA : 0;
    g->gravity_period = (drop < TB_GRAVITY_INITIAL - TB_GRAVITY_FLOOR)
                       ? TB_GRAVITY_INITIAL - drop : TB_GRAVITY_FLOOR;
}

// where the active piece would land if hard-dropped right now: walk a COPY
// of it down until it stops fitting. read-only on the game, so clients can
// call it every frame to draw the ghost piece.
int tb_ghost_y(const tb_game *g)
{
    tb_block probe = g->active;
    do { probe.origin.y++; } while (tb_block_fits(g, &probe));
    probe.origin.y--;                    // back off the row that failed
    return probe.origin.y;
}

// peeks at the next bag draw without taking it. -1 at the bag boundary --
// peeking further would mean rolling the rng, which would change the game.
int tb_next_piece(const tb_game *g)
{
    if (g->bag_index >= TB_NUM_PIECES) return -1;
    return g->bag[g->bag_index];
}

// advances the game by exactly one tick: apply the player's input (if
// any), then run gravity, then run the lock-delay rule. this is the ONE
// entry point that mutates a running game -- tetrisd calls it live, the
// replay viewer calls it from a log, and both must agree tick for tick.
// returns false once the game is over.
bool tb_tick(tb_game *g, tb_input input)
{
    if (g->game_over) return false;
    g->tick_count++;

    bool moved = false;                  // did the player successfully act?
    switch (input) {
    case TB_INPUT_NONE:                                          break;
    case TB_INPUT_LEFT:       moved = move_block(g, MV_LEFT);       break;
    case TB_INPUT_RIGHT:      moved = move_block(g, MV_RIGHT);      break;
    case TB_INPUT_ROTATE_CW:  moved = move_block(g, MV_ROTATE_CW);  break;
    case TB_INPUT_ROTATE_CCW: moved = move_block(g, MV_ROTATE_CCW); break;
    case TB_INPUT_SOFT_DROP:
        // A soft drop nudges the piece down but no longer locks it on contact.
        // Locking is the lock delay's job now, which is what gives the player
        // the grace period to slide it at the last moment.
        if (move_block(g, MV_DOWN)) { g->ticks_since_grav = 0; moved = true; }
        break;
    case TB_INPUT_HARD_DROP:
        // A hard drop is the one input that deliberately skips lock delay.
        // Slamming a piece down is a commitment.
        move_block(g, MV_DROP);
        lock_and_next(g);
        return !g->game_over;
    case TB_INPUT_HOLD:
        // A successful hold replaces the active piece outright, so lock
        // delay/gravity for this tick apply to the new piece next tick rather
        // than being computed against it now. A REFUSED hold (already used
        // this turn) must fall through to normal gravity and lock-delay
        // processing below -- returning early on it would let a player spam
        // hold every tick and freeze the piece mid-air forever.
        if (do_hold(g)) return !g->game_over;
        break;
    }

    // tick-driven gravity: step down once every gravity_period ticks.
    // Failing to move down no longer locks immediately; being unable to fall is
    // simply what "grounded" means, and the lock delay below decides when that
    // becomes a lock.
    if (++g->ticks_since_grav >= g->gravity_period) {
        g->ticks_since_grav = 0;
        move_block(g, MV_DOWN);
    }

    // Lock delay, move-reset rule.
    //
    // While the piece is resting on something, a timer runs. Moving or rotating
    // restarts it, so a player can shuffle a piece into place instead of losing
    // it the instant it touches down. The restart is capped, otherwise a player
    // could hold a piece up forever and the game would never progress. Once the
    // cap is reached the timer keeps running and the piece locks on schedule.
    if (g->lock_delay_ticks == 0) {
        // Lock delay disabled: behave the old way and lock on contact.
        if (is_grounded(g)) lock_and_next(g);
    } else if (is_grounded(g)) {
        if (moved && g->lock_resets < TB_LOCK_MAX_RESETS) {
            g->lock_timer = 0;
            g->lock_resets++;
        }
        if (++g->lock_timer >= g->lock_delay_ticks)
            lock_and_next(g);
    } else {
        // Airborne again, whether from a kick that lifted the piece or from the
        // stack falling away beneath it. The allowance refreshes.
        g->lock_timer  = 0;
        g->lock_resets = 0;
    }
    return !g->game_over;
}

// composites the locked stack plus the in-flight active piece into a
// plain 20x10 array for a frontend to draw: -1 = empty, else a piece
// value. the only place the active piece and the board ever appear merged.
void tb_render(const tb_game *g, int8_t out[TB_ROWS][TB_COLS])
{
    // copy the visible play area, dropping the sentinel border
    for (int y = 0; y < TB_ROWS; y++)
        for (int x = 0; x < TB_COLS; x++) {
            int8_t c = g->cells[y + 1][x + 1];      // strip the border offset
            out[y][x] = (c == TB_CELL_EMPTY) ? -1 : (int8_t)(c - 1);
        }
    // overlay the active piece unless the game already ended
    if (g->game_over) return;

    const tb_position *p = &tb_positions[g->active.type][g->active.orientation];
    for (int i = 0; i < TB_CELLS_PER_PIECE; i++) {
        int x = g->active.origin.x + p->pos[i].x;   // play coords
        int y = g->active.origin.y + p->pos[i].y;
        if (x >= 0 && x < TB_COLS && y >= 0 && y < TB_ROWS)
            out[y][x] = (int8_t)g->active.type;
    }
}