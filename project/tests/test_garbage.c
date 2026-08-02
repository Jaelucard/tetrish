// Unit tests for tb_inject_garbage(), the Battle Royale garbage injector.
//
// libtetrisbrain had no tests at all before this file. These are pure-logic
// tests: no sockets, no daemon, no timing. That is possible precisely because
// the engine's design contract forbids I/O and hidden randomness, so a board
// can be set up by hand and the result checked exactly.

#include <stdio.h>
#include <string.h>
#include "tetrisbrain.h"
#include "garbage.h"         // garbage_rows_for, the attack table

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

int main(void){
    printf("test_garbage: tb_inject_garbage\n");

    // --- 1. Basic shape: one row, one hole -------------------------------
    {
        tb_game g;
        tb_init(&g, 12345);
        tb_inject_garbage(&g, 1, 1u << 3);      // hole in column index 3

        check("1 row injected leaves 9 of 10 cells filled on the bottom row",
              row_filled(&g, TB_ROWS) == TB_COLS - 1);
        check("the hole is exactly where we asked for it",
              g.cells[TB_ROWS][3 + 1] == TB_CELL_EMPTY);
        check("garbage cells use TB_CELL_GARBAGE, not a piece value",
              g.cells[TB_ROWS][1] == TB_CELL_GARBAGE);
        check("the row above the garbage is untouched",
              row_filled(&g, TB_ROWS - 1) == 0);
    }

    // --- 2. The stack really moves up ------------------------------------
    {
        tb_game g;
        tb_init(&g, 999);
        // Put a single marker cell on the bottom row, then inject 3 rows.
        // The marker must end up 3 rows higher.
        g.cells[TB_ROWS][2] = 5;
        tb_inject_garbage(&g, 3, 1u << 0);

        check("a marker on the bottom row moves up by exactly 3",
              g.cells[TB_ROWS - 3][2] == 5);
        check("its old position is now garbage",
              g.cells[TB_ROWS][2] == TB_CELL_GARBAGE);
        check("all 3 requested rows were written",
              row_filled(&g, TB_ROWS)     == TB_COLS - 1 &&
              row_filled(&g, TB_ROWS - 1) == TB_COLS - 1 &&
              row_filled(&g, TB_ROWS - 2) == TB_COLS - 1);
    }

    // --- 3. Degenerate hole patterns are corrected -----------------------
    // A row with no hole would clear itself instantly; a fully-holed row is
    // not garbage at all. Both must fall back to a single hole.
    {
        tb_game g;
        tb_init(&g, 7);
        tb_inject_garbage(&g, 1, 0);
        check("hole_pattern 0 falls back to one hole (row not full)",
              row_filled(&g, TB_ROWS) == TB_COLS - 1);

        tb_game h;
        tb_init(&h, 7);
        tb_inject_garbage(&h, 1, 0xFFFF);
        check("all-ones hole_pattern falls back to one hole (row not empty)",
              row_filled(&h, TB_ROWS) == TB_COLS - 1);
    }

    // --- 4. Guards --------------------------------------------------------
    {
        tb_game g;
        tb_init(&g, 42);
        tb_inject_garbage(&g, 0, 1);
        check("injecting 0 rows is a no-op",
              row_filled(&g, TB_ROWS) == 0);

        tb_inject_garbage(&g, -5, 1);
        check("a negative row count is a no-op",
              row_filled(&g, TB_ROWS) == 0);

        tb_game d;
        tb_init(&d, 42);
        d.game_over = true;
        tb_inject_garbage(&d, 4, 1);
        check("a finished game is never modified",
              row_filled(&d, TB_ROWS) == 0);
    }

    // --- 5. Burying a player ends the game --------------------------------
    // Injecting a full playfield's worth of garbage leaves the active piece
    // nowhere to sit, so the game must end rather than silently corrupt.
    {
        tb_game g;
        tb_init(&g, 3);
        tb_inject_garbage(&g, TB_ROWS, 1u << 0);
        check("filling the board with garbage ends the game",
              g.game_over == true);
    }

    // --- 6. Determinism: same seed + same injections => identical state ----
    // This is the property that makes replay possible, and it only holds
    // because the caller supplies the hole pattern instead of the engine
    // rolling its own.
    {
        tb_game a, b;
        tb_init(&a, 20260802);
        tb_init(&b, 20260802);
        for (int i = 0; i < 30; i++){
            tb_tick(&a, TB_INPUT_NONE);
            tb_tick(&b, TB_INPUT_NONE);
        }
        tb_inject_garbage(&a, 2, 1u << 5);
        tb_inject_garbage(&b, 2, 1u << 5);
        check("two engines fed identical input stay bit-identical",
              memcmp(&a, &b, sizeof(tb_game)) == 0);
    }

    // --- 7. The attack table ---------------------------------------------
    printf("\ntest_garbage: garbage_rows_for (the attack table)\n");
    {
        check("a single sends nothing",        garbage_rows_for(1) == 0);
        check("a double sends 1 row",          garbage_rows_for(2) == 1);
        check("a triple sends 2 rows",         garbage_rows_for(3) == 2);
        check("a tetris sends 4 rows",         garbage_rows_for(4) == 4);
        check("clearing nothing sends nothing", garbage_rows_for(0) == 0);
    }

    // --- 8. Line-clear detection ------------------------------------------
    // handle_room_tick cannot ask tb_tick how many lines were cleared, because
    // it only returns a bool. It infers the count from the change in
    // lines_total across the tick. This proves that inference is sound, by
    // building a board where exactly two rows complete at once.
    //
    // No automated client in this repo plays well enough to clear two lines
    // (with no input at all, a game tops out after ~3900 ticks having cleared
    // none), so this hand-built board is the only way to exercise the path
    // short of a human at a real client.
    printf("\ntest_garbage: line-clear detection via lines_total delta\n");
    {
        tb_game g;
        tb_init(&g, 4242);

        // Fill the bottom two rows except columns 5 and 6, which is exactly
        // the 2x2 footprint an O piece drops into.
        for (int y = TB_ROWS - 1; y <= TB_ROWS; y++)
            for (int x = 1; x <= TB_COLS; x++)
                if (x != 5 && x != 6) g.cells[y][x] = 1;

        // Place an O piece above that gap. Its cells sit at offsets (1,1),
        // (2,1), (1,2), (2,2) from the origin, and board x = origin.x + off + 1,
        // so origin.x = 3 puts it in columns 5 and 6.
        g.active.type        = TB_O;
        g.active.orientation = 0;
        g.active.origin.x    = 3;
        g.active.origin.y    = 0;

        check("the two rows are not yet full",
              row_filled(&g, TB_ROWS) == TB_COLS - 2);

        uint32_t before = g.lines_total;
        tb_tick(&g, TB_INPUT_HARD_DROP);        // lock it into the gap
        uint32_t after  = g.lines_total;

        check("hard drop into the gap clears exactly 2 rows",
              (int)(after - before) == 2);
        check("that delta maps to 1 garbage row sent",
              garbage_rows_for((int)(after - before)) == 1);
        check("the board is clear again afterwards",
              row_filled(&g, TB_ROWS) == 0);
    }

    // --- 9. SRS wall kicks -------------------------------------------------
    // A rotation that does not fit at the spot it lands on must be retried at
    // the offsets in the kick table. The classic case is a piece flush against
    // a wall: the plain rotation is blocked, and the kick pushes it inward.
    printf("\ntest_garbage: SRS wall kicks\n");
    {
        tb_game g;
        tb_init(&g, 11);
        // A vertical I piece in the leftmost column. Orientation 1 puts its
        // cells at origin.x + 3, so origin.x = -2 sits it in play column 1,
        // flush against the wall.
        g.active.type        = TB_I;
        g.active.orientation = 1;
        g.active.origin.x    = -2;
        g.active.origin.y    = 4;
        check("the vertical I starts in a legal position",
              tb_block_fits(&g, &g.active));

        // Rotating it to horizontal would put cells at play columns -1 and 0,
        // which are wall, so the plain rotation is blocked. Test 2 of the
        // I-piece table for this transition is (+2, 0), which shifts it clear.
        tb_block before = g.active;
        tb_block plain  = g.active;
        plain.orientation = 2;
        check("the plain rotation really is blocked",
              !tb_block_fits(&g, &plain));

        tb_tick(&g, TB_INPUT_ROTATE_CW);
        check("the kick rescues the rotation",
              g.active.orientation != before.orientation);
        check("and it shifted the piece clear of the wall",
              g.active.origin.x == before.origin.x + 2);
    }
    {
        // Both directions must exist and must be opposites.
        tb_game g;
        tb_init(&g, 12);
        g.active.origin.y = 4;                       // clear of the floor
        uint8_t start = g.active.orientation;
        tb_tick(&g, TB_INPUT_ROTATE_CW);
        uint8_t cw = g.active.orientation;
        tb_tick(&g, TB_INPUT_ROTATE_CCW);
        check("CW then CCW returns to the starting orientation",
              g.active.orientation == start);
        check("CW actually changed the orientation",
              g.active.type == TB_O ? 1 : cw != start);
    }

    // --- 10. Lock delay ----------------------------------------------------
    printf("\ntest_garbage: lock delay (move reset)\n");
    {
        // Drop a piece to the floor with a hard drop, which deliberately
        // bypasses lock delay, then confirm a NEW piece resting on the floor
        // does not lock on contact.
        tb_game g;
        tb_init(&g, 21);
        tb_set_lock_delay(&g, 10);

        // Fall until the piece is resting, without locking it.
        int guard = 0;
        while (guard++ < 5000) {
            tb_block probe = g.active;
            probe.origin.y++;
            if (!tb_block_fits(&g, &probe)) break;   // grounded
            tb_tick(&g, TB_INPUT_NONE);
        }
        uint64_t landed_piece_ticks = g.tick_count;
        uint32_t stack_before = 0;
        for (int x = 1; x <= TB_COLS; x++)
            if (g.cells[TB_ROWS][x] != TB_CELL_EMPTY) stack_before++;

        check("a resting piece has not locked yet", stack_before == 0);

        // Now sit still for the full delay. It must lock.
        for (int i = 0; i < 12; i++) tb_tick(&g, TB_INPUT_NONE);
        uint32_t stack_after = 0;
        for (int y = 1; y <= TB_ROWS; y++)
            for (int x = 1; x <= TB_COLS; x++)
                if (g.cells[y][x] != TB_CELL_EMPTY) stack_after++;
        check("it locks once the delay elapses", stack_after > 0);
        check("the delay actually took ticks", g.tick_count > landed_piece_ticks);
    }
    {
        // The reset cap. Moving forever must not hold a piece up forever.
        tb_game g;
        tb_init(&g, 22);
        tb_set_lock_delay(&g, 10);
        int guard = 0;
        while (guard++ < 5000) {
            tb_block probe = g.active;
            probe.origin.y++;
            if (!tb_block_fits(&g, &probe)) break;
            tb_tick(&g, TB_INPUT_NONE);
        }
        // Alternate left and right far longer than 15 resets would allow.
        for (int i = 0; i < 400; i++)
            tb_tick(&g, (i % 2) ? TB_INPUT_LEFT : TB_INPUT_RIGHT);
        uint32_t filled = 0;
        for (int y = 1; y <= TB_ROWS; y++)
            for (int x = 1; x <= TB_COLS; x++)
                if (g.cells[y][x] != TB_CELL_EMPTY) filled++;
        check("endless shuffling cannot stall the game forever", filled > 0);
    }
    {
        // Disabling it restores the old lock-on-contact behaviour, which the
        // determinism of earlier recordings depends on.
        tb_game g;
        tb_init(&g, 23);
        tb_set_lock_delay(&g, 0);
        int guard = 0;
        while (guard++ < 5000 && g.tick_count < 4000) {
            tb_tick(&g, TB_INPUT_NONE);
            uint32_t filled = 0;
            for (int x = 1; x <= TB_COLS; x++)
                if (g.cells[TB_ROWS][x] != TB_CELL_EMPTY) filled++;
            if (filled) break;
        }
        uint32_t filled = 0;
        for (int x = 1; x <= TB_COLS; x++)
            if (g.cells[TB_ROWS][x] != TB_CELL_EMPTY) filled++;
        check("lock_delay 0 locks on contact, as before", filled > 0);
    }

    printf("\ntest_garbage: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
