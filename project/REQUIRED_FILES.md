# Required files / blockers outside the owned tree

Per CLAUDE.md section 2: items this track is blocked on, in a teammate-owned
path or environment. Owned-side work stops at a thin adapter or is otherwise
noted, and continues on the next unblocked item.

- [ ] `src/libhtttp/htttp.c` build fix — needed for a full `make`/`make test`
      pass on the current toolchain (WSL Ubuntu, gcc 15.2) — expected
      interface: `#include <ctype.h>` added near the top of the file (`trim()`
      calls `isspace()` without it; gcc 15's default standard treats implicit
      function declarations as a hard error, not a warning). This blocks
      linking any binary that pulls in `libhtttp.a` (`tetrisd`, `tetrisctl`,
      `tetrisu`), and blocks `test_htttp`/`fuzz_htttp`/`stub_client`. Owned
      code was verified independently instead: `libtetrisbrain` compiles and
      its test suite (`test_garbage`, `test_brain`) runs clean under
      `-fsanitize=address,undefined` built directly against sources; owned
      `.c` files were checked with a standalone `-c` compile. Not touched —
      `src/libhtttp/` is a read-only path.

- [ ] `auth/generate_keys.sh` line endings — needed for `tests/integration.sh`
      stage 2 (and any manual setup) to run on this checkout — expected fix:
      convert the file to LF. It currently has CRLF line endings, so its
      `set -euo pipefail` shebang line is read as `pipefail\r`, which bash
      rejects as an invalid option name (`invalid option name`), and it
      fails again immediately after on an unbound `$BASH_SOURCE[0]` for the
      same reason. Confirmed with `file auth/generate_keys.sh` ->
      "... with CRLF line terminators". Verified independently instead by
      stripping the `\r` into a throwaway temp copy (never writing into
      `auth/`) and running that; the script itself works once the line
      endings are fixed. Not touched — `auth/` is a read-only path.

- [ ] tetrisd wire action for `TB_INPUT_HOLD` — needed for hold to work over
      the network, not just `tetrisu --local` — expected interface: a MOVE-
      family action (e.g. `HOLD`) that the room tick maps to
      `TB_INPUT_HOLD`, analogous to how `ROTATE CW`/`CCW` map today.

- [ ] tetrisd start-level room option — needed for a configurable starting
      level over the network, not just `tetrisu --local` — expected
      interface: a JOIN/START parameter that the room applies via
      `tb_set_start_level` right after `tb_init`, before the first tick.

- [ ] week 8 MVP checklist handout — needed to walk the "17 baseline
      checklist items" CLAUDE.md's week 8 tasks refer to and confirm each
      owned item individually — expected artifact: the course handout text
      (not currently anywhere in this repo). Verified what's possible
      without it instead: a clean-tree build+run of every owned test
      (`make clean && sh tests/smoke.sh`) passes tier 1 (owned-only)
      end to end, including the 20k-tick determinism gate. Ask the user for
      the handout to close this out properly.

- [ ] libtetrissh admin-auth capability — needed for an authenticated ops
      console per CLAUDE.md's week 9 gate — expected interface: some
      credential/role concept for the control plane. Current substitute:
      tetrisd's control socket (`ctl_path`) has no session or credential
      layer at all, plain HTTTP over a local `AF_UNIX` socket gated only by
      filesystem permissions (see `tetrisctl/main.c`, the existing reference
      client). `tetrish-view`'s repl (week 9) is built against that reality
      rather than blocking on this.

- [ ] STATE frame fields for the active piece and the next piece — needed for
      ghost/preview rendering on the networked path (they currently only work
      on `tetrisu --local`) — expected interface: an extra header or body
      line per player block giving active piece type/origin/orientation and
      the next piece type, alongside the existing `player ... score ...`
      line and cell grid (`src/tetrisu/net.c`'s `parse_state`).

- [ ] tetrisd spectate join mode — needed for `tetrisu --spectate` (currently
      stubbed to print this requirement and exit 2, see `src/tetrisu/main.c`)
      and for `tetrish-view attach <room>` (week 11, `src/tetrish-view/attach.c`)
      to watch a room without occupying a seat or being able to force a
      START — expected interface: a JOIN variant that receives STATE
      broadcasts without being dealt a board or counted as a room seat.
      Implemented `attach` as an ordinary late-join participant instead (see
      DECISIONS.md's attach-model entry for the caveat this implies); it can
      switch to a real spectate JOIN with no repl-facing behavior change
      once this lands.

- [ ] tetrisd control-plane JSON escaping — needed for `/rooms` and
      `/players` to stay well-formed when a room or player name contains
      `"` or `\` (room ids come straight from the client's JOIN path;
      `jb_room`/`jb_player` in `src/tetrisd/main.c` format them into JSON
      with no escaping) — expected interface: escape `"` and `\` in every
      string field `jb_append` emits. Owned side hardened against it
      meanwhile: `src/tetrish-view/jsonish.c` only accepts a field match at
      a key position and falls back to raw brace counting when a stray
      quote breaks string scanning, so a hostile name can no longer spoof
      another field's value or make the object unparseable (covered by
      `tests/test_view_json.c`'s decoy-key test).

- [ ] tetrisd control-plane kick endpoint — needed for `tetrish-view`'s
      `kick <player>` command (week 10) to actually remove a player, not just
      report the server's refusal — expected interface: `GET
      /room/<id>/player/<p>/kick`, analogous to the existing `/room/<id>`
      path-parsing helper in `handle_ctl` (`src/tetrisd/main.c`), closing that
      player's fd and dropping their room seat. Verified the current
      boundary behaviour instead: `cmd_kick` (`src/tetrish-view/repl.c`)
      looks the player's room up via `/players`, sends the request it would
      send, and today gets back tetrisd's real 404 `{"error": "unknown
      control path"}` (confirmed live against a running `tetrisd`, and
      covered by `tests/test_control.c`'s kick-path mock).

- [ ] distinct STATE cell marker for garbage rows — needed for tetrisu's
      visualizer GARBAGE accent to be exact rather than inferred — expected
      interface: `render_board` (`src/tetrisd/main.c`) emitting a dedicated
      character (e.g. '8', matching the log format's `board_hex` choice) for
      `TB_CELL_GARBAGE` cells instead of the current `'0' + (126 % 10)` =
      '6', which aliases garbage with L-piece cells on the wire. Until then
      the client infers injections from a filled-cell jump > 4 in one STATE
      frame without a line clear (a lock adds at most 4 cells net, a garbage
      row adds 9).
