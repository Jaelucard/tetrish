# Design decisions

Open-design choices in the application-track code (`libtetrisbrain`, `tetrisu`,
`tetrish-view`), recorded as they're made. Entries are appended in the order
the corresponding feature was built; earlier entries are not rewritten once
later work builds on them.

## Board representation

The playfield is `int8_t cells[TB_BH][TB_BW]` where `TB_BH = TB_ROWS + 2` and
`TB_BW = TB_COLS + 2` (22x12 for a 20x10 visible field). The extra row/column
on every side is a permanent sentinel wall (`TB_CELL_WALL`), so a piece's
occupancy check is a single array read with no explicit bounds branch: any
coordinate that would be off the visible board lands on a wall cell instead.
Piece-to-board coordinates fold in the border with a `+1` offset
(`bx = origin.x + piece_dx + 1`), which is why that `+1` shows up throughout
`tetrisbrain.c`.

**Caveat found while adding the 20k-tick determinism test (test_brain.c):**
the sentinel border only covers exactly one cell beyond the visible field.
SRS kick candidates can propose an origin more than one cell out (an I-piece
kick can move `origin.y` by 2), and indexing `cells[]` beyond the border is
undefined behaviour, not a guaranteed wall read — AddressSanitizer caught a
real stack-buffer-overflow from this during rotation fuzzing. `tb_block_fits`
now explicitly bounds-checks `by`/`bx` against the array dimensions before
indexing, on top of the sentinel-border fast path. The border optimization
still holds for the case it was designed for (single-cell moves); it just
isn't sufficient on its own for arbitrary candidate positions.

## Rotation system

SRS (Super Rotation System): a rotation first tries the plain orientation
change, and if that's blocked, retries at up to four additional offsets from
a fixed kick table (JLSTZ and I have separate tables; O never kicks, since
all four of its orientations are identical). The kick tables in
`tetrisbrain.c` (`kicks_JLSTZ_cw/ccw`, `kicks_I_cw/ccw`) are the source of
truth — they're published SRS specification data, not derived from anything
else in this codebase. Both rotation directions (`TB_INPUT_ROTATE_CW/CCW`)
exist and both try kicks.

## Scoring

Line-clear score is `tetris_points[rows - 1] * level`, where `tetris_points`
is `{40, 100, 300, 1200}` for single/double/triple/tetris — the classic NES
values. It replaced an earlier `level * rows^2 * 10` placeholder formula.

`tetris_points` lives in `tb_game`, not a `#define`, so a future ruleset
variant could override it without an API change; `tb_init` seeds it with the
defaults above.

**Replay caveat:** `tetrish-view --verify` (`board_hex` in
`src/tetrish-view/main.c`) compares locked-cell hex only, never score, so this
change doesn't break verification of logs recorded before it. It does mean a
replayed session's on-screen score can differ from what the player originally
saw if their session predates this change — the board state is still
authoritative and matches exactly.

## Lock delay policy

Move-reset lock delay: once a piece is resting (can't move down), a
tick-counted grace period (`TB_LOCK_DELAY_TICKS`, default 10 ticks / 500 ms at
20 Hz) has to elapse before it locks. Any successful move or rotation while
resting restarts the timer, up to a cap of `TB_LOCK_MAX_RESETS` (15) restarts;
after the cap, the timer keeps running and the piece locks on schedule
regardless of further input. This stops a piece being held in place forever
while still giving a player room to slide/rotate at the last moment.
`tb_set_lock_delay(g, 0)` disables it and restores lock-on-contact, which is
what tetrisd falls back to for the tick rates where this determinism
guarantee matters least.

## Ghost derivation

`tb_ghost_y` is a pure, `const`-game helper: it walks a copy of the active
piece down until it stops fitting and returns the last fitting row. It never
mutates `tb_game`, and only reads shared state (`tb_block_fits`), so it can be
called every render frame with zero cost to game state or determinism. Ghost
rendering itself lives client-side, not in the brain, per the separation rule
in CLAUDE.md (pure derivations belong to the caller).

## Hold

`hold` (`TB_INPUT_HOLD`, `do_hold` in `tetrisbrain.c`) swaps the active
piece into a one-slot stash and brings in whatever was previously held —
or, the first time hold is ever used, the next piece from the bag rather
than leaving the slot's contents ambiguous. `hold = -1` is the sentinel for
"nothing held yet"; `held_this_turn` blocks a second hold before the piece
locks, which is what stops a player from hold-swapping back and forth
indefinitely to stall gravity. Locking clears `held_this_turn` (in
`lock_and_next`), making hold available again for the next piece — the
same reset point that already clears the lock-delay timer and its move
count, so a fresh piece never inherits any state from the one before it.

## Start level

`tb_set_start_level` (not folded into `tb_init`, per the append-only rule —
changing `tb_init`'s signature would break every existing caller) sets
`level`/`start_level` and recomputes `gravity_period` by running the same
step `score_lines`'s level-up uses, backwards from level 1, rather than
looping `tb_tick`-style level-ups `level - 1` times. Both formulas are kept
in sync deliberately (see the comment at the call site): a future change to
the level-up gravity curve has to touch both, and forgetting one would mean
a game started at a given level plays at different speed than one that
leveled up into it normally.

## Next-piece preview boundary

`tb_next_piece` peeks `bag[bag_index]` without refilling the bag. It returns
`-1` exactly when the currently active piece is the last piece of its bag
window (`bag_index == TB_NUM_PIECES`) — peeking past that would require
rolling the RNG to refill early, which would change what the *next* spawn
actually draws depending on how many times a caller chose to peek. Refusing
to peek past the boundary keeps the helper pure and behavior-neutral; the
client renders `?` for the `-1` case.

## Repl design (ops console)

tetrisd's control plane is plain HTTTP `GET` over a local `AF_UNIX` socket
(`ctl_path` in the rc file): `/status`, `/rooms`, `/players`,
`/dropped-logs`, `/shutdown`. Authorization is filesystem permissions on
that socket, not a session or credential — there is no admin-auth concept in
libtetrissh to gate against (recorded in `REQUIRED_FILES.md`). `tetrish-view`
speaks this same plane rather than inventing a second one: `control.c`
mirrors `tetrisctl`'s connect/request/read-to-EOF/parse pattern (the existing
reference client for this protocol), and the repl's commands are thin
wrappers that map a typed name to a fixed path, exactly as `tetrisctl`'s
`command_to_path` does. `kick` has no server endpoint yet, so it is
implemented to the boundary: it sends the request it would send and reports
the server's real error, rather than pretending to succeed.

`attach`/`replay` are a different protocol entirely (the game plane, over
`tetrissh`, not the control plane), so `tetrish-view` ends up speaking two
protocols for two different purposes — this is inherent to what the console
needs to do, not an accidental extra dependency.

## Line editing and history (ops console)

`edit.c`/`edit.h` is a pure byte-in, state-out line editor (`edit_feed`,
`edit_commit`) with zero termios/tty knowledge, following the same split as
`dispatch.c`: keeping the state machine free of I/O is what makes it
unit-testable without a pty (`tests/test_view_edit.c`). `repl.c` owns the
actual raw-mode tty and does the drawing (`\r\x1b[K` plus reprint) in
response to what `edit_feed` reports.

`repl_run` picks between two loops based on `isatty(STDIN_FILENO)`: a real
terminal gets raw mode, `edit.c`-backed history and single-byte `select()`
input; anything else (a pipe, a redirected file) falls back to the original
`fgets`-per-line loop. This is not a cosmetic choice — a raw-mode-only repl
would break `tests/test_view_repl.c`-style piped/scripted input and
`tests/integration.sh`, since neither drives a pty. Both loops call the same
`dispatch_or_quit` helper so the command table and quit/help handling exist
in exactly one place.

Ctrl-D-on-an-empty-line-quits is handled in `repl.c`'s interactive loop, not
inside `edit_feed`: `edit.c` treats byte `0x04` like any other unmapped
control byte (ignored) because "what ends the session" is repl policy, not
line-editing state: the pure editor has no concept of quitting.

`select()` over stdin plus a `signalfd` for SIGINT/SIGTERM mirrors
`src/tetrisu/main.c`'s established loop shape (same rationale: EINTR-free
shutdown, and the tty gets restored via an `atexit` handler on every exit
path rather than only on a clean `quit`). ISIG is left enabled in raw mode
deliberately, so ctrl-c still becomes a real (blocked, then signalfd-read)
SIGINT rather than a raw `0x03` byte the editor would have to special-case.

## Attach model (ops console)

tetrisd has no spectate join mode (`REQUIRED_FILES.md`): the only way to
receive a room's STATE broadcasts is an ordinary JOIN, which deals the
caller a real board and counts toward `max_players_per_room`. Rather than
block `attach <room>` on a server feature that doesn't exist, `attach.c`
implements it as an honest late-join participant, reusing
`src/tetrisu/net.c`'s adapter (`net_connect_and_handshake`,
`net_send_action`, `net_poll_state`) instead of a second copy of the same
JOIN/START/STATE/LEAVE protocol -- both directories are owned, and tetrisu
already proved this exact sequence works.

**Caveat:** the console genuinely occupies a seat while attached (visible
to other players as an extra board that never moves) and, if it's the one
that triggers START, forces the game to begin. This is deliberate given
what the server actually supports today, not an oversight; a real spectate
mode (received but not seated, not counted) would remove the caveat without
changing `attach`'s repl-facing behavior at all, so the fix is additive
whenever that server capability lands.

Rendering is plain text (clear-screen + reprint on each STATE frame), not
ncurses, so `attach` never switches the terminal in and out of curses mode
around the console's own raw-mode prompt (see the line-editing entry
above) -- it draws directly into whatever mode the repl already has stdout
in. `replay` (below), by contrast, does use ncurses, because its
interactive viewer predates the repl and already owned that tradeoff.

## Replay as a repl command

`replay.c`/`replay.h` (`replay_run`) is the Week 6/8 replay viewer, moved
out of `main.c` unchanged except for taking its logpath/room/player/verify
as parameters instead of `argv`. Both the original CLI form
(`tetrish-view [--verify] <log-file> ...`, now just an argument-parsing
shim in `main.c`) and the repl's `replay <logfile> [room] [player]`
command call the same function, so there is exactly one reconstruction
implementation regardless of how it's invoked -- consistent with the
determinism rule that replay must never diverge from a second code path.

The repl form always runs the interactive viewer, never `--verify`: a
headless pass/fail check is what a script wants, and a script driving the
repl through a pipe already can't use ncurses anyway. `replay_run` checks
`isatty(STDOUT_FILENO)` before calling `initscr()` and refuses cleanly
(exit code 2) rather than wedging if it's asked to open the interactive
viewer without a terminal -- a latent gap in the original CLI-only code
that now matters because `tests/integration.sh`-style scripting can reach
the repl command path.

`replay_run`'s static event buffer (`g_ev`/`g_nev`) is reset at the top of
every call, since it can now run more than once in the same process (a
repl session issuing `replay` twice) rather than exactly once per process
lifetime as it was when replay was the entire program.
