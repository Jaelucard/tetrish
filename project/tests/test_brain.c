// Unit tests for the week-5/6 brain features layered on top of the existing
// SRS/lock-delay/garbage engine: the tetris_points scoring table, row-18 and
// row-20 clears, game-over cause, the 7-bag property, 20k-tick determinism,
// tb_ghost_y, tb_next_piece, hold, and tb_set_start_level.
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
        uint32_t before = g.score;
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("single clear at level 1 scores tetris_points[0]",
              g.score - before == (uint32_t)g.tetris_points[0] * 1u);
    }

    // --- 2. Double, at level 3 ----------------------------------------------
    {
        tb_game g;
        tb_init(&g, 102);
        g.level = 3;
        for (int y = TB_ROWS - 1; y <= TB_ROWS; y++)
            for (int x = 1; x <= TB_COLS; x++)
                if (x != 5 && x != 6) g.cells[y][x] = 1;

        g.active.type = TB_O; g.active.orientation = 0; g.active.origin.x = 3;
        uint32_t before = g.score;
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("double clear at level 3 scores tetris_points[1] * level",
              g.score - before == (uint32_t)g.tetris_points[1] * 3u);
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
        uint32_t before = g.score;
        uint32_t lines_before = g.lines_total;
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("the vertical I actually cleared exactly 3 rows",
              g.lines_total - lines_before == 3);
        check("triple clear at level 1 scores tetris_points[2]",
              g.score - before == (uint32_t)g.tetris_points[2] * 1u);
    }

    // --- 4. Tetris, at level 5 ------------------------------------------------
    {
        tb_game g;
        tb_init(&g, 104);
        g.level = 5;
        for (int y = TB_ROWS - 3; y <= TB_ROWS; y++)
            for (int x = 1; x <= TB_COLS; x++)
                if (x != 5) g.cells[y][x] = 1;

        g.active.type = TB_I; g.active.orientation = 1; g.active.origin.x = 2;
        uint32_t before = g.score;
        uint32_t lines_before = g.lines_total;
        tb_tick(&g, TB_INPUT_HARD_DROP);
        check("the vertical I actually cleared exactly 4 rows",
              g.lines_total - lines_before == 4);
        check("tetris clear at level 5 scores tetris_points[3] * level",
              g.score - before == (uint32_t)g.tetris_points[3] * 5u);
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

    printf("\ntest_brain: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
