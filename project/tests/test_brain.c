// Unit tests for the week-5/6 brain features layered on top of the existing
// SRS/lock-delay/garbage engine: the tetris_points scoring table, row-18 and
// row-20 clears, game-over cause, the 7-bag property, 20k-tick determinism,
// tb_ghost_y, tb_next_piece, hold, tb_set_start_level, and the guideline
// scoring layer (combo, back-to-back, t-spins, perfect clear, drop rewards).
//
// Same approach as test_garbage.c: pure-logic tests built by poking g.cells
// and g.active directly, since the engine forbids I/O and hidden randomness
// and so is fully controllable by hand.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tetrisbrain.h"

static int failures = 0;

static void check(const char *what, int cond){
    printf("  %-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) failures++;
}

// What a hard drop is about to pay for the distance it travels. Every
// hard-drop test nets this out before checking the clear award, since the two
// land in g.score together and only their sum is observable.
static uint32_t drop_reward(const tb_game *g){
    return (uint32_t)(tb_ghost_y(g) - g->active.origin.y) * g->hard_drop_points;
}

// Fill one play row, leaving `ngap` named columns empty. Board coords, 1-based,
// matching the cells[] indexing every other setup here uses.
static void fill_row_except(tb_game *g, int y, const int *gap, int ngap){
    for (int x = 1; x <= TB_COLS; x++){
        int skip = 0;
        for (int i = 0; i < ngap; i++) if (gap[i] == x) skip = 1;
        if (!skip) g->cells[y][x] = 1;
    }
}

// Count occupied cells in one visible play row (1-based, 1 = top).
static int row_filled(const tb_game *g, int y){
    int n = 0;
    for (int x = 1; x <= TB_COLS; x++)
        if (g->cells[y][x] != TB_CELL_EMPTY) n++;
    return n;
}

// Empty the board back to just its sentinel border, leaving every other
// field (rng, bag, score, hold, ...) untouched. Lets a test hard-drop
// repeatedly without the stack ever blocking the next piece.
static void clear_stack(tb_game *g){
    memset(g->cells, TB_CELL_EMPTY, sizeof(g->cells));
    for (int x = 0; x < TB_BW; x++) {
        g->cells[0][x]         = TB_CELL_WALL;
        g->cells[TB_BH - 1][x] = TB_CELL_WALL;
    }
    for (int y = 0; y < TB_BH; y++) {
        g->cells[y][0]         = TB_CELL_WALL;
        g->cells[y][TB_BW - 1] = TB_CELL_WALL;
    }
}

static uint32_t test_lcg(uint32_t *state){
    *state = (*state) * 1103515245u + 12345u;
    return *state;
}

int main(void){
    printf("test_brain: tetris_points scoring\n");

    // --- 1. Single: only the bottom row completes --------------------------
    {
        tb_game g;
        tb_init(&g, 101);
        g.level = 1;
        for (int x = 1; x <= TB_COLS; x++)
            if (x != 5 && x != 6) g.cells[TB_ROWS][x] = 1;
        // row TB_ROWS - 1 stays empty on purpose.

        g.active.type = TB_O; g.active.orientation = 0; g.active.origin.x = 3;
        uint32_t before = g.score, fall = drop_reward(&g);
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("single clear at level 1 scores tetris_points[0]",
              g.score - before == (uint32_t)g.tetris_points[0] * 1u + fall);
        check("the guideline single base value is 100", g.tetris_points[0] == 100);
    }

    // --- 2. Double, at level 3 ----------------------------------------------
    {
        tb_game g;
        tb_init(&g, 102);
        g.level = 3;
        for (int y = TB_ROWS - 1; y <= TB_ROWS; y++)
            for (int x = 1; x <= TB_COLS; x++)
                if (x != 5 && x != 6) g.cells[y][x] = 1;
        // A marker well above the action. Without it the two prefilled rows are
        // the entire stack, so clearing them would also earn a perfect clear
        // and this case would be measuring two bonuses at once.
        g.cells[10][1] = 1;

        g.active.type = TB_O; g.active.orientation = 0; g.active.origin.x = 3;
        uint32_t before = g.score, fall = drop_reward(&g);
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("double clear at level 3 scores tetris_points[1] * level",
              g.score - before == (uint32_t)g.tetris_points[1] * 3u + fall);
        check("the guideline double base value is 300", g.tetris_points[1] == 300);
    }

    // --- 3. Triple, at level 1 -----------------------------------------------
    // Vertical I fills a single column across 4 rows. Prefill the bottom 3 of
    // those rows minus that column; leave the 4th (topmost) empty so only 3
    // rows complete.
    {
        tb_game g;
        tb_init(&g, 103);
        g.level = 1;
        for (int y = TB_ROWS - 2; y <= TB_ROWS; y++)
            for (int x = 1; x <= TB_COLS; x++)
                if (x != 5) g.cells[y][x] = 1;

        g.active.type = TB_I; g.active.orientation = 1; g.active.origin.x = 2;
        uint32_t before = g.score, fall = drop_reward(&g);
        uint32_t lines_before = g.lines_total;
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("the vertical I actually cleared exactly 3 rows",
              g.lines_total - lines_before == 3);
        check("triple clear at level 1 scores tetris_points[2]",
              g.score - before == (uint32_t)g.tetris_points[2] * 1u + fall);
        check("the guideline triple base value is 500", g.tetris_points[2] == 500);
    }

    // --- 4. Quad, at level 5 --------------------------------------------------
    {
        tb_game g;
        tb_init(&g, 104);
        g.level = 5;
        for (int y = TB_ROWS - 3; y <= TB_ROWS; y++)
            for (int x = 1; x <= TB_COLS; x++)
                if (x != 5) g.cells[y][x] = 1;
        g.cells[10][1] = 1;               // no perfect clear, as in case 2

        g.active.type = TB_I; g.active.orientation = 1; g.active.origin.x = 2;
        uint32_t before = g.score, fall = drop_reward(&g);
        uint32_t lines_before = g.lines_total;
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("the vertical I actually cleared exactly 4 rows",
              g.lines_total - lines_before == 4);
        check("quad clear at level 5 scores tetris_points[3] * level",
              g.score - before == (uint32_t)g.tetris_points[3] * 5u + fall);
        check("the guideline quad base value is 800", g.tetris_points[3] == 800);
        check("a lone quad arms back-to-back but is not itself multiplied",
              g.b2b_count == 1);
    }

    // --- 5. Row 20 (the bottom row) clears and shifts everything above -------
    printf("\ntest_brain: row-specific clears\n");
    {
        tb_game g;
        tb_init(&g, 201);
        for (int x = 1; x <= TB_COLS; x++)
            if (x != 5) g.cells[TB_ROWS][x] = 1;
        g.cells[TB_ROWS - 1][1] = 3;          // marker, well clear of column 5

        g.active.type = TB_I; g.active.orientation = 1; g.active.origin.x = 2;
        uint32_t lines_before = g.lines_total;
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("row 20 clears exactly once",
              g.lines_total - lines_before == 1);
        check("the marker above it shifted down into row 20",
              g.cells[TB_ROWS][1] == 3);
    }

    // --- 6. Row 18, with an unrelated garbage floor underneath ---------------
    // tb_inject_garbage gives a floor that is deliberately never full (a
    // permanent hole at column 1), so it cannot clear itself and only serves
    // to block the piece from falling past row 18.
    {
        tb_game g;
        tb_init(&g, 202);
        tb_inject_garbage(&g, 2, 1u << 0);    // rows 19-20, hole at column 1
        for (int x = 1; x <= TB_COLS; x++)
            if (x != 5) g.cells[18][x] = 1;
        g.cells[17][2] = 3;                   // marker, above the target row

        g.active.type = TB_I; g.active.orientation = 1; g.active.origin.x = 2;
        uint32_t lines_before = g.lines_total;
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("row 18 clears exactly once, floor rows untouched",
              g.lines_total - lines_before == 1);
        check("the marker above row 18 shifted down into it",
              g.cells[18][2] == 3);
        check("the garbage floor below is still there (rows below a clear don't move)",
              row_filled(&g, 20) == TB_COLS - 1 && row_filled(&g, 19) == TB_COLS - 1);
    }

    // --- 7. Game-over cause ---------------------------------------------------
    printf("\ntest_brain: game-over cause\n");
    {
        tb_game g;
        tb_init(&g, 301);
        // Occupy the O piece's spawn box (array rows 2-3, columns 5-6; see the
        // spawn-origin math in section 5) so the next spawn cannot fit.
        for (int y = 2; y <= 3; y++)
            for (int x = 5; x <= 6; x++)
                g.cells[y][x] = 1;
        tb_spawn(&g, TB_O);
        check("a blocked spawn box sets game_over", g.game_over);
        check("the cause is TB_OVER_BLOCKOUT", g.over_cause == TB_OVER_BLOCKOUT);
    }
    {
        tb_game g;
        tb_init(&g, 302);
        tb_inject_garbage(&g, TB_ROWS, 1u << 0);   // bury the whole board
        check("burying the board ends the game", g.game_over);
        check("the cause is TB_OVER_TOPOUT", g.over_cause == TB_OVER_TOPOUT);
    }

    // --- 8. 7-bag property: every 7 consecutive spawns contain each piece --
    printf("\ntest_brain: 7-bag property\n");
    {
        tb_game g;
        tb_init(&g, 401);
        const int total = 91;                 // 13 full bags
        int8_t history[91];
        for (int i = 0; i < total; i++) {
            history[i] = (int8_t)g.active.type;
            clear_stack(&g);
            tb_tick(&g, TB_INPUT_HARD_DROP);
        }
        int all_ok = 1;
        for (int b = 0; b < total / TB_NUM_PIECES; b++) {
            int seen[TB_NUM_PIECES] = {0};
            for (int k = 0; k < TB_NUM_PIECES; k++) {
                int8_t p = history[b * TB_NUM_PIECES + k];
                if (p < 0 || p >= TB_NUM_PIECES || seen[p]) { all_ok = 0; break; }
                seen[p] = 1;
            }
            if (!all_ok) break;
        }
        check("every aligned 7-window has each piece exactly once", all_ok);
    }

    // --- 9. 20k-tick determinism ---------------------------------------------
    // The canonical determinism gate: two engines, same seed, fed the same
    // pseudo-random inputs (drawn from a small LCG that has nothing to do
    // with the game's own xorshift32), must stay memcmp-identical the whole
    // way. An idle/near-idle game tops out well under 20000 ticks, so both
    // engines are re-seeded together and the run continues.
    printf("\ntest_brain: 20k-tick determinism\n");
    {
        uint64_t total_ticks = 0;
        int mismatch = 0;
        uint32_t lcg = 0xC0FFEEu;
        uint32_t seed = test_lcg(&lcg);
        tb_game a, b;
        tb_init(&a, seed);
        tb_init(&b, seed);
        while (total_ticks < 20000) {
            tb_input in = (tb_input)(test_lcg(&lcg) % 8u);
            tb_tick(&a, in);
            tb_tick(&b, in);
            if (memcmp(&a, &b, sizeof(tb_game)) != 0) { mismatch = 1; break; }
            total_ticks++;
            if (a.game_over) {
                seed = test_lcg(&lcg);
                tb_init(&a, seed);
                tb_init(&b, seed);
            }
        }
        check("20000 ticks of matched random input stay bit-identical",
              !mismatch);
        check("the full 20000-tick budget was actually reached",
              total_ticks >= 20000);
    }

    // --- 10. tb_ghost_y ---------------------------------------------------
    printf("\ntest_brain: tb_ghost_y\n");
    {
        tb_game g;
        tb_init(&g, 501);
        tb_game before = g;
        int ghost = tb_ghost_y(&g);
        check("tb_ghost_y does not mutate the game",
              memcmp(&g, &before, sizeof(tb_game)) == 0);

        tb_block probe = g.active;
        probe.origin.y = (int8_t)ghost;
        check("the ghost row itself is a legal fit", tb_block_fits(&g, &probe));
        probe.origin.y = (int8_t)(ghost + 1);
        check("one row past the ghost is not a legal fit", !tb_block_fits(&g, &probe));
    }

    // --- 11. tb_next_piece --------------------------------------------------
    printf("\ntest_brain: tb_next_piece preview\n");
    {
        tb_game g;
        tb_init(&g, 601);
        int checked = 0, mismatches = 0;
        for (int i = 0; i < 120; i++) {
            int peek = tb_next_piece(&g);
            clear_stack(&g);
            tb_tick(&g, TB_INPUT_HARD_DROP);
            if (peek >= 0) {
                checked++;
                if (peek != (int)g.active.type) mismatches++;
            }
        }
        check("tb_next_piece always predicts the next spawn when not -1",
              checked > 0 && mismatches == 0);
    }

    // --- 12. Hold -------------------------------------------------------------
    printf("\ntest_brain: hold\n");
    {
        tb_game g;
        tb_init(&g, 701);
        int8_t first_active = (int8_t)g.active.type;
        int peek = tb_next_piece(&g);
        tb_tick(&g, TB_INPUT_HOLD);
        check("hold stashes the piece that was active", g.hold == first_active);
        check("with nothing held, hold draws the next bag piece",
              peek < 0 || (int)g.active.type == peek);
        check("held_this_turn is now set", g.held_this_turn);

        tb_piece_type after_first = g.active.type;
        tb_tick(&g, TB_INPUT_HOLD);
        check("a second hold before locking is ignored",
              g.active.type == after_first && g.hold == first_active);
    }
    {
        tb_game g;
        tb_init(&g, 702);
        tb_tick(&g, TB_INPUT_HOLD);           // stash piece A, draw piece B
        int8_t stashed_a = g.hold;
        tb_tick(&g, TB_INPUT_HARD_DROP);      // lock piece B, spawn piece C
        check("held_this_turn clears once the piece locks", !g.held_this_turn);
        tb_tick(&g, TB_INPUT_HOLD);           // swap: get piece A back
        check("hold is usable again next turn and returns the earlier piece",
              g.active.type == (tb_piece_type)stashed_a);
    }
    {
        // A refused hold (held_this_turn already set) must still run gravity
        // and lock delay on that tick. If it doesn't, spamming hold every
        // tick freezes the piece mid-air forever -- the exact stall the
        // held_this_turn flag exists to prevent. A lock is observable as
        // held_this_turn clearing (lock_and_next resets it), so within a
        // generous tick budget (fall from spawn at level-1 gravity is under
        // 1000 ticks, plus the lock delay) at least one lock must happen.
        tb_game g;
        tb_init(&g, 703);
        tb_tick(&g, TB_INPUT_HOLD);           // legitimate: sets held_this_turn
        int locked = 0;
        for (int i = 0; i < 5000 && !g.game_over; i++) {
            tb_tick(&g, TB_INPUT_HOLD);       // refused every single tick
            if (!g.held_this_turn) { locked = 1; break; }
        }
        check("spamming a refused hold cannot stall gravity and lock delay",
              locked);
    }

    // --- 13. tb_set_start_level -----------------------------------------------
    printf("\ntest_brain: tb_set_start_level\n");
    {
        tb_game g;
        tb_init(&g, 801);
        tb_set_start_level(&g, 5);
        check("start level sets level", g.level == 5);
        check("start level records itself", g.start_level == 5);

        // Independently reconstruct the level-1..5 gravity progression from
        // the same public constants tb_set_start_level is documented to use,
        // rather than re-deriving its closed-form formula.
        uint32_t expected = TB_GRAVITY_INITIAL;
        for (uint32_t lv = 1; lv < 5; lv++)
            expected = (expected > TB_GRAVITY_FLOOR + TB_GRAVITY_DELTA)
                     ? expected - TB_GRAVITY_DELTA : TB_GRAVITY_FLOOR;
        check("gravity period matches the natural level-up progression",
              g.gravity_period == expected);
    }

    // --- 14. Drop rewards -----------------------------------------------------
    // Both are flat: the spec deliberately exempts them from the level
    // multiplier, so a level-9 game is checked to pay exactly what level 1 does.
    printf("\ntest_brain: drop rewards\n");
    {
        tb_game g;
        tb_init(&g, 901);
        g.level = 9;
        uint32_t before = g.score;
        int y_before = g.active.origin.y;
        tb_tick(&g, TB_INPUT_SOFT_DROP);
        check("a soft drop that moves pays soft_drop_points, flat",
              g.active.origin.y == y_before + 1 &&
              g.score - before == (uint32_t)g.soft_drop_points);
        check("the default soft drop reward is 1 per cell", g.soft_drop_points == 1);
    }
    {
        tb_game g;
        tb_init(&g, 902);
        g.level = 9;
        uint32_t before = g.score;
        uint32_t fallen = (uint32_t)(tb_ghost_y(&g) - g.active.origin.y);
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("a hard drop pays hard_drop_points per cell, flat",
              g.score - before == fallen * (uint32_t)g.hard_drop_points);
        check("the default hard drop reward is 2 per cell", g.hard_drop_points == 2);
        check("the drop distance was actually nonzero", fallen > 0);
    }
    {
        // A soft drop into the floor moves nothing, so it must pay nothing --
        // otherwise holding the key down on a resting piece prints points.
        tb_game g;
        tb_init(&g, 903);
        tb_tick(&g, TB_INPUT_HARD_DROP);        // land and lock a piece
        clear_stack(&g);
        g.active.origin.y = (int8_t)tb_ghost_y(&g);   // park it on the floor
        uint32_t before = g.score;
        tb_tick(&g, TB_INPUT_SOFT_DROP);
        check("a soft drop that cannot move pays nothing", g.score == before);
    }

    // --- 15. Combo ------------------------------------------------------------
    // Three single clears in a row. The combo counts the pieces that cleared
    // BEFORE the current one, so the awards go 100, 100+50, 100+100.
    printf("\ntest_brain: combo\n");
    {
        tb_game g;
        tb_init(&g, 1001);
        g.level = 1;
        const int gap[2] = {5, 6};
        uint32_t expect[3] = {100u, 150u, 200u};
        int ok = 1;
        for (int i = 0; i < 3; i++){
            clear_stack(&g);
            fill_row_except(&g, TB_ROWS, gap, 2);
            g.active.type = TB_O; g.active.orientation = 0;
            g.active.origin.x = 3; g.active.origin.y = 0;
            uint32_t before = g.score, fall = drop_reward(&g);
            tb_tick(&g, TB_INPUT_HARD_DROP);
            if (g.score - before != expect[i] + fall) ok = 0;
        }
        check("three chained singles pay 100, then 150, then 200", ok);
        check("the combo counter reached three", g.combo_count == 3);

        // A piece that clears nothing ends the chain.
        clear_stack(&g);
        g.active.type = TB_O; g.active.orientation = 0;
        g.active.origin.x = 3; g.active.origin.y = 0;
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("a lock that clears nothing resets the combo", g.combo_count == 0);

        // ...and the next clear is therefore back to a bare single.
        clear_stack(&g);
        fill_row_except(&g, TB_ROWS, gap, 2);
        g.active.type = TB_O; g.active.orientation = 0;
        g.active.origin.x = 3; g.active.origin.y = 0;
        uint32_t before = g.score, fall = drop_reward(&g);
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("the clear after a reset pays no combo again",
              g.score - before == 100u + fall);
    }

    // --- 16. Back-to-back -----------------------------------------------------
    printf("\ntest_brain: back-to-back\n");
    {
        tb_game g;
        tb_init(&g, 1101);
        g.level = 1;
        const int gap[1] = {5};

        // Two quads back to back. The first arms the chain and is not
        // multiplied; the second gets 800 * 3 / 2 = 1200, plus one combo step
        // (50) since it is also the second consecutive clearing piece.
        uint32_t award[2];
        for (int i = 0; i < 2; i++){
            clear_stack(&g);
            g.cells[10][1] = 1;                 // suppress the perfect clear
            for (int y = TB_ROWS - 3; y <= TB_ROWS; y++)
                fill_row_except(&g, y, gap, 1);
            g.active.type = TB_I; g.active.orientation = 1;
            g.active.origin.x = 2; g.active.origin.y = 0;
            uint32_t before = g.score, fall = drop_reward(&g);
            tb_tick(&g, TB_INPUT_HARD_DROP);
            award[i] = g.score - before - fall;
        }
        check("the first quad pays a flat 800", award[0] == 800u);
        check("the second quad pays 1200 back-to-back plus a 50 combo step",
              award[1] == 1250u);
        check("the back-to-back chain is two long", g.b2b_count == 2);

        // A piece that clears nothing must NOT break the chain.
        clear_stack(&g);
        g.active.type = TB_O; g.active.orientation = 0;
        g.active.origin.x = 3; g.active.origin.y = 0;
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("a lock that clears nothing leaves back-to-back intact",
              g.b2b_count == 2);

        // An ordinary single does break it.
        clear_stack(&g);
        const int sgap[2] = {5, 6};
        fill_row_except(&g, TB_ROWS, sgap, 2);
        g.active.type = TB_O; g.active.orientation = 0;
        g.active.origin.x = 3; g.active.origin.y = 0;
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("an ordinary line clear breaks the back-to-back chain",
              g.b2b_count == 0);
    }

    // --- 17. T-spin recognition -----------------------------------------------
    // The engine only calls something a t-spin if the piece ARRIVED by
    // rotation, so this first case drives a real rotation through tb_tick
    // rather than poking the flags: it is the end-to-end proof that the
    // rotation bookkeeping and the corner rule agree.
    //
    // Board geometry, in cells[] coords. A T rotated CW from its vertical
    // (orientation 1) form at origin (2,17) lands nose-down in the notch at
    // column 5, centre at cells[19][5], corners at [18][4] [18][6] [20][4]
    // [20][6]. Three of those four are filled, and the plain rotation fits
    // without a kick, so it is a full spin rather than a mini.
    printf("\ntest_brain: t-spin recognition\n");
    {
        tb_game g;
        tb_init(&g, 1201);
        g.level = 1;
        g.cells[20][4] = 1;
        g.cells[20][6] = 1;
        g.cells[18][4] = 1;

        g.active.type = TB_T; g.active.orientation = 1;
        g.active.origin.x = 2; g.active.origin.y = 17;
        tb_tick(&g, TB_INPUT_ROTATE_CW);
        check("the setup rotation actually landed nose-down",
              g.active.orientation == 2 && g.active.origin.y == 17);
        check("a successful rotation records itself", g.last_was_rotation);
        check("this rotation needed no kick", !g.last_rotation_kicked);

        uint32_t before = g.score;
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("a rotation into a 3-corner pocket is a full t-spin",
              g.last_clear_kind == TB_CLEAR_TSPIN);
        check("a t-spin clearing nothing still scores tspin_points[0]",
              g.score - before == (uint32_t)g.tspin_points[0]);
    }
    {
        // Same board, but the T is walked into place instead of rotated into
        // it. Nothing about the shape changed, so this isolates the "last
        // action was a rotation" half of the rule.
        tb_game g;
        tb_init(&g, 1202);
        g.level = 1;
        g.cells[20][4] = 1;
        g.cells[20][6] = 1;
        g.cells[18][4] = 1;

        g.active.type = TB_T; g.active.orientation = 2;
        g.active.origin.x = 1; g.active.origin.y = 17;
        tb_tick(&g, TB_INPUT_RIGHT);
        check("the move put the T in the same pocket",
              g.active.origin.x == 2 && !g.last_was_rotation);

        uint32_t before = g.score;
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("the same placement reached by moving is not a t-spin",
              g.last_clear_kind == TB_CLEAR_NORMAL);
        check("and it therefore scores nothing", g.score == before);
    }

    // --- 18. T-spin and mini values -------------------------------------------
    // The remaining variants set last_was_rotation directly rather than
    // building a board that forces each exact kick. That is the same
    // poke-the-state approach the rest of this file uses, and it is what lets
    // the mini rule be tested independently of which kick offset produced it.
    // Every T here is placed already resting, so the hard drop travels zero
    // rows and cannot clear the flag on its way down.
    printf("\ntest_brain: t-spin and mini values\n");
    {
        struct {
            const char  *name;
            int          orientation;
            int          rows_prefilled[3];   // cells[] rows to fill, 0 = unused
            const int   *gaps[3];
            int          ngaps[3];
            int          extra_y, extra_x;    // one more corner block
            int          kicked;
            tb_clear_kind expect_kind;
            uint32_t     expect_award;
        } cases[] = {
            // Nose-down in the notch: both bottom corners are the pointing
            // face, so filling the row that clears always fills them -- which
            // is exactly why a mini cannot clear from this orientation.
            { "t-spin single", 2, {20, 0, 0}, {(const int[]){5}, 0, 0}, {1, 0, 0},
              18, 4, 0, TB_CLEAR_TSPIN, 100u + 800u },
            { "t-spin double", 2, {20, 19, 0},
              {(const int[]){5}, (const int[]){4, 5, 6}, 0}, {1, 3, 0},
              18, 4, 0, TB_CLEAR_TSPIN, 300u + 1200u },
            // Nose-left, so the pointing face is the two LEFT corners, split
            // across two rows. Leaving cells[18][4] empty keeps them from both
            // filling even when the bottom row completes -- the shape that
            // makes a mini single possible at all.
            { "t-spin mini single", 3, {20, 0, 0}, {(const int[]){5}, 0, 0}, {1, 0, 0},
              18, 6, 1, TB_CLEAR_TSPIN_MINI, 100u + 200u },
            { "t-spin mini double", 3, {20, 19, 0},
              {(const int[]){5}, (const int[]){4, 5}, 0}, {1, 2, 0},
              18, 6, 1, TB_CLEAR_TSPIN_MINI, 300u + 400u },
        };

        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++){
            tb_game g;
            tb_init(&g, (uint32_t)(1300 + i));
            g.level = 1;
            for (int r = 0; r < 3; r++)
                if (cases[i].rows_prefilled[r])
                    fill_row_except(&g, cases[i].rows_prefilled[r],
                                    cases[i].gaps[r], cases[i].ngaps[r]);
            g.cells[cases[i].extra_y][cases[i].extra_x] = 1;

            g.active.type = TB_T;
            g.active.orientation = (uint8_t)cases[i].orientation;
            g.active.origin.x = 2; g.active.origin.y = 17;
            g.last_was_rotation    = true;
            g.last_rotation_kicked = cases[i].kicked ? true : false;

            uint32_t before = g.score;
            check("the T is already resting, so the drop travels nothing",
                  drop_reward(&g) == 0);
            tb_tick(&g, TB_INPUT_HARD_DROP);
            check(cases[i].name, g.last_clear_kind == cases[i].expect_kind &&
                                 g.score - before == cases[i].expect_award);
        }
    }
    {
        // A t-spin triple: vertical T, nose left, filling one column across
        // three rows that all complete. The marker keeps it from also being a
        // perfect clear.
        tb_game g;
        tb_init(&g, 1310);
        g.level = 1;
        g.cells[10][1] = 1;
        const int g18[1] = {5}, g19[2] = {4, 5}, g20[1] = {5};
        fill_row_except(&g, 18, g18, 1);
        fill_row_except(&g, 19, g19, 2);
        fill_row_except(&g, 20, g20, 1);

        g.active.type = TB_T; g.active.orientation = 3;
        g.active.origin.x = 2; g.active.origin.y = 17;
        g.last_was_rotation = true;

        uint32_t before = g.score, lines_before = g.lines_total;
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("the t-spin triple cleared exactly three rows",
              g.lines_total - lines_before == 3);
        check("t-spin triple", g.last_clear_kind == TB_CLEAR_TSPIN &&
                               g.score - before == 500u + 1600u);
    }
    {
        // A mini that clears nothing, which the mini table's index 0 covers.
        tb_game g;
        tb_init(&g, 1311);
        g.level = 1;
        g.cells[18][4] = 1;
        g.cells[18][6] = 1;
        g.cells[20][4] = 1;               // one pointing-face corner only

        g.active.type = TB_T; g.active.orientation = 2;
        g.active.origin.x = 2; g.active.origin.y = 17;
        g.last_was_rotation    = true;
        g.last_rotation_kicked = true;

        uint32_t before = g.score;
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("t-spin mini clearing nothing",
              g.last_clear_kind == TB_CLEAR_TSPIN_MINI &&
              g.score - before == (uint32_t)g.tspin_mini_points[0]);
    }
    {
        // A t-spin that clears lines is "difficult", so two in a row chain
        // back-to-back exactly as two quads do.
        tb_game g;
        tb_init(&g, 1312);
        g.level = 1;
        const int gap[1] = {5};
        uint32_t award[2];
        for (int i = 0; i < 2; i++){
            clear_stack(&g);
            fill_row_except(&g, 20, gap, 1);
            g.cells[18][4] = 1;
            g.active.type = TB_T; g.active.orientation = 2;
            g.active.origin.x = 2; g.active.origin.y = 17;
            g.last_was_rotation = true;
            uint32_t before = g.score;
            tb_tick(&g, TB_INPUT_HARD_DROP);
            award[i] = g.score - before;
        }
        check("the first t-spin single pays 100 base plus 800 bonus",
              award[0] == 900u);
        // 100 * 3 / 2 = 150 on the base only, the 800 bonus is added after the
        // multiplier, and the second clear also earns one 50-point combo step.
        check("a chained t-spin single multiplies only its base value",
              award[1] == 150u + 800u + 50u);
    }

    // --- 19. Perfect clear ----------------------------------------------------
    printf("\ntest_brain: perfect clear\n");
    {
        tb_game g;
        tb_init(&g, 1401);
        g.level = 1;
        const int gap[4] = {4, 5, 6, 7};
        fill_row_except(&g, TB_ROWS, gap, 4);   // one row, one I-piece hole

        g.active.type = TB_I; g.active.orientation = 0;
        g.active.origin.x = 3; g.active.origin.y = 0;
        uint32_t before = g.score, fall = drop_reward(&g);
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("the board really is empty afterwards", row_filled(&g, TB_ROWS) == 0);
        check("emptying the board adds perfect_clear_points",
              g.score - before == 100u + (uint32_t)g.perfect_clear_points + fall);
        check("the default perfect clear award is 3500",
              g.perfect_clear_points == 3500);
    }

    // --- 20. Determinism of the new scoring state ------------------------------
    // The 20k-tick gate above compares whole structs, which already covers
    // these fields. This case is narrower and states the intent directly:
    // score and both chain counters must agree at EVERY step, not just at the
    // end, so a divergence cannot cancel itself out before the final compare.
    printf("\ntest_brain: scoring determinism\n");
    {
        uint32_t lcg = 0x5EEDu;
        uint32_t seed = test_lcg(&lcg);
        tb_game a, b;
        tb_init(&a, seed);
        tb_init(&b, seed);
        int mismatch = 0, saw_score = 0;
        for (int i = 0; i < 4000 && !mismatch; i++){
            tb_input in = (tb_input)(test_lcg(&lcg) % 9u);
            tb_tick(&a, in);
            tb_tick(&b, in);
            if (a.score != b.score || a.combo_count != b.combo_count ||
                a.b2b_count != b.b2b_count || a.last_clear_kind != b.last_clear_kind)
                mismatch = 1;
            if (a.score > 0) saw_score = 1;
            if (a.game_over){
                seed = test_lcg(&lcg);
                tb_init(&a, seed);
                tb_init(&b, seed);
            }
        }
        check("score and both chain counters agree at every step", !mismatch);
        check("the run actually scored something to compare", saw_score);
    }

    printf("\ntest_brain: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
