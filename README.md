Tetris Battle Royale

By Nit, Yee sin and Jarrod

## Architecture of owned components

This section covers the application & integration track: the game logic
library, the two ncurses clients, and their tests. The server track (daemon,
log service, admin CLI, session/protocol libraries) is documented separately
by its own owners.

```
project/src/libtetrisbrain/   pure game logic (no I/O, no clock, no globals)
project/src/tetrisu/          player client: networked (default) or --local
project/src/tetrish-view/     ops console: admin repl + replay viewer
project/tests/                unit tests + tests/smoke.sh, tests/integration.sh
```

All owned code lives under `project/`; `project/src/libtetrisbrain/
tetrisbrain.h` is the brain's public header (it sits next to its `.c` rather
than under `include/`, since that's the layout the daemon side already
expects it at).

Build from `project/`: `make` builds every library and binary in the shared
tree (this track's and the server track's); `make test` builds and runs
every test suite, including the sanitized owned suite (`make test-san` runs
just that part, with no dependency on the session/protocol libraries). This
is POSIX C — ncurses, `AF_UNIX`, `signalfd`, POSIX message queues — so build
and run it in the devcontainer/WSL, not natively on Windows.

### Demo: running it end to end

1. `make` (builds everything; see "known limitations" under Ops console
   usage if `libhtttp` doesn't build on your toolchain — `tests/smoke.sh`
   and `make test-san` still work without it).
2. `bash auth/generate_keys.sh` once, to generate the PKI `tetrisd`/
   `tetrisu` need for the session handshake.
3. Copy `sample.tetrishrc` to `.tetrishrc` and adjust paths/ports if needed,
   then start the two daemons: `bin/tetrislogd .tetrishrc` and
   `bin/tetrisd .tetrishrc` (each in its own terminal).
4. Play: `bin/tetrisu .tetrishrc [room]` (networked) or
   `bin/tetrisu --local [seed] [start-level]` (offline, no daemons needed).
5. Administer: `bin/tetrish-view` (no arguments) from the same directory as
   your `.tetrishrc` opens the admin console — try `status`, `rooms`,
   `players`, then `attach <room>` while a game from step 4 is running.
6. Replay a session afterward: `bin/tetrish-view --verify var/log/
   tetrisd.log <room> <player>` (headless pass/fail) or the same command
   without `--verify` for the interactive viewer — also reachable as the
   console's own `replay` command from step 5.

## Brain design choices

`libtetrisbrain` is a pure, deterministic Tetris engine: given a seed and a
sequence of inputs, `tb_tick` always produces the same board, tick for tick,
on any machine. That's what makes both server authority (many clients
trusting one simulation) and replay (`tetrish-view`, reconstructing a session
from a log) possible. The full rationale for each design choice — board
representation, rotation system, scoring, lock delay, ghost derivation, the
next-piece preview boundary — is in `project/DECISIONS.md`, including a
memory-safety bug the determinism test itself caught and the fix for it.

Public API: `tb_init`, `tb_tick`, `tb_spawn`, `tb_block_fits`, `tb_render`,
`tb_inject_garbage` (Battle Royale garbage rows), `tb_set_lock_delay`,
`tb_set_start_level`, `tb_ghost_y`, `tb_next_piece`. Inputs include movement,
both rotation directions (full SRS kick tables), soft/hard drop, and hold.

### Determinism contract

`tb_tick` may not do I/O, read the wall clock, touch a global, or call
`rand()` — its only source of randomness is the xorshift32 state carried
inside `tb_game` itself, seeded once by `tb_init`. Same seed plus the same
input sequence must produce a `memcmp`-identical `tb_game` on any run,
which is what `tests/test_brain.c`'s 20k-tick determinism test checks on
every change, and it's the reason both tetrisd (one simulation, many
trusting clients) and `tetrish-view`'s replay (reconstructing a session
from a log) are trustworthy at all.

This means the wire format for `tb_game`/`tb_input` can only ever grow:
new `tb_input` values are appended after `TB_INPUT_HARD_DROP`, new
`tb_game` fields are appended at the struct's end, existing enum
values/field order never change, and `tb_init`'s signature never changes.
tetrisd links this engine directly and replay logs depend on tick-for-tick
behavior, so reordering anything here would silently break every
previously recorded session.

## Client rendering

**tetrisu** has two paths, both in `project/src/tetrisu/`:

- **Networked (default)**, `tetrisu <rc-file> [room]`: the server is
  authoritative. `main.c` renders whatever board the server broadcasts and
  sends keystrokes as HTTTP requests; it holds no game state and links
  `libtetrisbrain` only for the `--local` path below. The wire protocol
  itself (handshake, JOIN/START/LEAVE, request/response framing, STATE
  parsing) is factored into `net.c`/`net.h`, a small adapter (`net_
  connect_and_handshake`, `net_send_action`, `net_poll_state`) so protocol
  churn stays out of the render code. `--spectate` is parsed but not yet
  usable — it needs a server-side join mode that doesn't exist yet (see
  `project/REQUIRED_FILES.md`).
- **Local**, `tetrisu --local [seed] [start-level]`: a full offline game
  against an in-process `tb_game` (`tetrisu.c`/`local.h`), no sockets. This
  is also the only path with ghost-piece and next-piece-preview rendering
  today, since the server's STATE frames don't carry the piece/bag data
  those need (also tracked in `REQUIRED_FILES.md`).

Both paths draw a bracket/dot ASCII board (`[]` filled, dim `.` empty) at 2
terminal columns per cell; the local path adds dim `[]` for the ghost piece
and small NEXT/HOLD boxes.

### Controls

Left/Right move, Up rotates clockwise, `z` rotates counter-clockwise, Down
soft-drops, Space hard-drops, `q` quits. `c` holds — only on `--local`
today, since tetrisd has no wire action for it yet (see "known
limitations" below).

## Ops console usage

**tetrish-view** (`project/src/tetrish-view/`) has two modes, split on
whether a log file is given:

- **Admin console**, `tetrish-view` (no arguments): a repl against tetrisd's
  control plane (`ctl_path` in `.tetrishrc`, the same plain-HTTTP-over-
  `AF_UNIX` socket `tetrisctl` speaks). Prompt, read a command, dispatch,
  repeat; `help`/`?` lists commands, `quit`/`exit`/ctrl-d leaves.
  Commands: `status` (tetrisd's live counters), `rooms` (id/players/
  started/ticks, one aligned row per room), `players` (player/fd/room),
  `kick <player>` (looks the player's room up via `/players`, then sends
  the kick request tetrisd would need — there's no server endpoint yet, so
  today this reports tetrisd's real 404, see `REQUIRED_FILES.md`),
  `attach <room>` (a live plain-text board view that streams STATE frames
  until any key detaches — implemented as an ordinary late-join participant
  over the game plane, since tetrisd has no spectate mode; see
  `DECISIONS.md`'s attach-model entry for the caveat), and
  `replay <logfile> [room] [player]` (the same interactive ncurses viewer
  described below, reachable from inside the console). A real terminal
  gets raw-mode line editing with backspace, ctrl-u, and up/down command
  history (32 entries); piped/non-interactive input (scripts, tests) falls
  back to plain line-at-a-time reading. The wire code lives in `control.c`
  (`control_get`, mirroring `tetrisctl`'s own connect/request/read
  pattern) for the control-plane commands, and in `attach.c` (reusing
  `src/tetrisu/net.c`'s adapter) for the game-plane `attach`; JSON field
  extraction is `jsonish.c` (a scanner scoped to tetrisd's exact
  `/rooms`/`/players` shapes, not a general parser); line editing is
  `edit.c` (a pure byte-in/state-out state machine, no termios); the pure
  parsing/dispatch-lookup logic lives in `dispatch.c`. `jsonish.c`/`edit.c`/
  `dispatch.c` are kept free of any libhtttp dependency specifically so
  they're unit-testable without a socket or a tty (`tests/test_view_repl.c`,
  `tests/test_view_json.c`, `tests/test_view_edit.c`); `tests/test_control.c`
  tests `control_get` end to end (including the rooms/players/kick paths)
  against a mock control socket (`tests/mocks/mock_control.c`).
- **Replay viewer**, `tetrish-view [--verify] <log-file> [room] [player]`:
  parses a tetrislogd log and reconstructs the session by re-driving
  `tb_tick`, the same entry point tetrisd uses, cross-checking the
  reconstruction against every recorded board snapshot as it goes.
  Interactive mode (space to pause, left/right to step, +/- for speed)
  needs a terminal; `--verify` runs the same reconstruction headless and
  prints `REPLAY VERIFIED` or `REPLAY UNVERIFIED`, which is what
  `tests/smoke.sh` checks when a log is available.

See `project/DECISIONS.md` for the repl/attach/replay design decisions.

### Known limitations

All of these are teammate-side gaps recorded in `project/REQUIRED_FILES.md`,
not owned-code bugs; owned code is built against each boundary rather than
blocked on it:

- **No `kick` endpoint on tetrisd** — the console's `kick <player>` looks
  the player's room up correctly and sends the request tetrisd would need,
  but gets back a real 404 today.
- **No spectate join mode** — `tetrisu --spectate` is stubbed to report
  this and exit; `attach <room>` occupies a real seat (and can force a
  START) rather than watching from the side, since the only way to receive
  STATE broadcasts is an ordinary JOIN.
- **No admin-auth on the control plane** — the console authenticates as
  whoever has filesystem access to `ctl_path`, nothing more; there's no
  credential/role concept in `libtetrissh` to build a real one against.
- **No networked ghost/preview/hold** — STATE frames carry only the board
  and score, not the active piece or bag, so ghost-piece and next-piece
  rendering (and the `c` hold key) only work on `tetrisu --local`.
- **Replayed score can differ from what was originally seen** — `--verify`
  compares locked-cell hex only, so a session recorded before the scoring
  formula changed still verifies, but its displayed score won't match.
- **Interactive `replay` needs a real terminal** — it refuses cleanly
  (rather than hang or crash trying to open ncurses) if stdout isn't a
  tty; use `--verify` instead for scripts. `attach`'s plain-text view has
  no such requirement.
