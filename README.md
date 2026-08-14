# tetriSH

A terminal Battle-Royale Tetris system in C: a launcher shell, a concurrent
game daemon, a separate logger daemon, an admin CLI, a game client, and a
dual-mode session viewer, over an authenticated and confidential session
protocol of our own implementation.

Built for the SUTD 50.005 x 50.003 CoreStack track.

2026 SUTD 50.005 CSE C3C8 Corestack: Tetris Battle Royale

Created by following SUTD students:

* 1009016 - Nithilan Srinivasan Poonthamarai
* 1009320 - Wong Yee Jin
* 1008976 - Jarrod Low


---

## Contents

- [Build and run](#build-and-run)
- [Architecture](#architecture)
- [The shell and process management](#the-shell-and-process-management)
- [Concurrency and locking](#concurrency-and-locking)
- [IPC choices](#ipc-choices)
- [Security](#security)
- [The HTTTP protocol](#the-htttp-protocol)
- [The game engine](#the-game-engine)
- [The clients](#the-clients)
- [Logging and the replay format](#logging-and-the-replay-format)
- [Configuration](#configuration)
- [Testing](#testing)
- [Known limitations](#known-limitations)

The three lanes map onto these sections: **Systems** owns the shell, the daemon
architecture, concurrency and IPC; **Networking and Security** owns the session
handshake, the threat model and the HTTTP protocol; **Application and
Integration** owns the game engine, the room lifecycle, the clients and the
replay format.

---

## Build and run

### Requirements

Linux, GCC, OpenSSL headers, ncurses headers. The daemon uses `epoll`,
`signalfd`, `timerfd` and POSIX message queues, all of which are Linux-only, so
macOS and Windows need a container. A `Dockerfile` is included:

```sh
docker build -t tetrish-build .
docker run -d --name tetrish -v "$PWD":/mnt tetrish-build sleep infinity
docker exec tetrish sh -c 'cp -r /mnt /work'      # copy INTO the container
docker exec -it tetrish bash
cd /work
```

> Copy the tree to a container-local path rather than working in the bind
> mount. AF_UNIX sockets do not behave correctly on a bind-mounted filesystem,
> and the control plane and logger both use them.

### Build from a clean checkout

```sh
bash auth/generate_keys.sh      # CA + server certificate + key, once
cp sample.tetrishrc .tetrishrc  # local config; .tetrishrc is gitignored
make                            # 3 static libraries + 6 binaries
make test                       # unit suites, fuzzer, and test harnesses
```

`make` alone is enough to produce every binary. The two steps around it exist
because the certificates are generated rather than committed (a private key in
a repository is a mistake regardless of the repository) and because
`.tetrishrc` holds machine-local paths.

### Run the demo

`tetrish` is the entry point. Put the launch commands in your `.tetrishrc` and
the whole system comes up from one program:

```
# in .tetrishrc, alongside the configuration directives
dspawn bin/tetrislogd .tetrishrc
dspawn bin/tetrisd    .tetrishrc
```

```sh
./bin/tetrish                   # runs .tetrishrc, then gives you a prompt
tetrish> sys                    # both daemons, with pids
tetrish> bin/tetrisu .tetrishrc demo
```

The shell executes `.tetrishrc` at startup the way a shell sources its rc file.
The same file is read twice for two different purposes: the daemons consume the
configuration directives, and the shell executes the lines that are commands,
skipping the directives it knows the daemons own. (`rc_is_directive()` in
`src/common/rc.c` is the list that separates the two.)

Other things to try:

```sh
./bin/tetrisctl .tetrishrc status          # JSON: uptime, rooms, players, drops
./bin/tetrisctl .tetrishrc rooms
./bin/tetrisctl .tetrishrc dropped-logs    # drops split by cause
./bin/tetrisctl .tetrishrc shutdown        # graceful, four-phase

./bin/tetrish-view                         # ops console (REPL over the control plane)
./bin/tetrish-view var/log/tetrisd.log     # replay a recorded session
./bin/tetrish-view --verify <log>          # headless replay, for scripts and tests

./bin/tetrisu --local                      # full offline game, no server needed
```

Client keys: arrows move, `↑` or `x` rotate CW, `z` rotate CCW, `a` rotate 180,
`↓` soft drop, space hard drop, `c` hold, `q` quit.

There is deliberately no pause and no retry on the networked path: a room ticks
for every player at once, so neither has a meaning the server could honour for
one seat. `--local` has both.

Inside the ops console, `attach <room>` streams a room's `STATE` frames live;
`rooms`, `players` and `kick` speak the same control plane `tetrisctl` uses.

---

## Architecture

Six processes, of which three are long-lived.

| Process | Role | Lifetime |
|---|---|---|
| `tetrish` | Launcher shell. REPL, ten builtins, job control. | Interactive |
| `tetrisd` | Game daemon. One epoll loop, all game state. | Long-lived |
| `tetrislogd` | Logger daemon. Owns the log file. | Long-lived |
| `tetrisctl` | Admin CLI. One request, then exits. | Momentary |
| `tetrisu` | Game client. Networked, or `--local` offline. | Per session |
| `tetrish-view` | Ops console, live attach, and replay viewer (50.003). | Per session |

Three static libraries: `libtetrissh` (secure session), `libhtttp` (protocol),
`libtetrisbrain` (game rules, pure logic, no I/O).

### The daemon is one thread, and everything is a file descriptor

`tetrisd` runs a single `epoll_wait` loop. Every event source it cares about is
a descriptor, so one loop covers all of them:

| Descriptor | Source |
|---|---|
| `listen_fd` | TCP, new clients |
| one per client | TCP, encrypted HTTTP frames |
| one per started room | `timerfd`, the room's tick |
| `sig_fd` | `signalfd`: SIGTERM, SIGHUP, SIGUSR1 |
| `ctl_fd` | AF_UNIX stream, the control plane |
| `mq` | POSIX message queue, Battle-Royale garbage |

The dispatch is four fixed comparisons, an O(1) lookup in an fd-indexed client
table, then a scan for the room owning that timerfd. **Because every handler is
a branch of the same `if/else` on one thread, no two handlers can interleave.**
That is why most of the concurrency answers in this project are short: for a
question of the form "what synchronises X against Y", the answer is usually
"nothing needs to - they are the same thread".

Signals arrive as readable descriptors rather than as handlers interrupting
game state, which removes the entire class of async-signal-safety bugs from the
game path. Room ticks are `timerfd`s in the same set, so there is no ticker
thread per room.

The one thing that is *not* on this thread is log shipping - see below.

### Why single-threaded dispatch

Game state is shared and mutable. A thread-per-client design needs a lock
around every room access and pays for it on every input; a process-per-room
design needs IPC for every cross-room operation, which is most of Battle
Royale. At the scale this project targets, one epoll loop is simply enough:
measured at 500 concurrent clients across 100 rooms, the tick rate held at
19.9-20.0 Hz with no client below 16 Hz. Redis's `ae` loop and ioquake3's
`SV_Frame` are the same shape for the same reason - game logic is cheap and I/O
is the bottleneck.

### Admission is queued, not inline

`accept()` does as little as possible: take the connection, apply the per-IP
limit, park the descriptor. The RSA handshake then runs from the epoll loop,
at most `handshake_budget` per pass.

This was changed under measurement. With the handshake inline in `accept()`, a
500-client burst froze the loop for about four seconds and 69 clients timed out
waiting for the certificate. Queued, with a budget of 32, the same test admits
500/500. The counter-intuitive result is that even a *small* budget beats the
inline path, because a cheap `accept()` lets the kernel's backlog drain instead
of overflowing.

One consequence worth stating: once admission is driven by readiness, the epoll
batch size stops being a tuning knob and becomes a correctness bound. With 500
pending sockets and 100 room timerfds competing for 32 event slots, handshakes
queued behind ticks until clients gave up. `EPOLL_BATCH` is now sized to the
descriptor set (`PENDING_HS_MAX + ROOM_HARD_MAX + 16`).

---

## The shell and process management

`tetrish` is the entry point and the PA1 half of the system: a REPL over
`fork()` + `execvp()`, ten builtins, and a job table.

| Builtin | Purpose |
|---|---|
| `cd`, `env`, `setenv`, `unsetenv` | working directory and environment |
| `help`, `usage`, `exit` | self-documentation and teardown |
| `dspawn CMD [ARGS...]` | run a program as a tracked background job |
| `sys` | list every tracked job and whether it is still running |
| `dcheck PID` | poll one job without blocking |

Anything that is not a builtin is run as an external program. Background jobs go
into a linked list, and `sys`/`dcheck` reap them with `waitpid(WNOHANG)` so the
shell never blocks on a child it is only reporting on.

**`.tetrishrc` is executed at startup**, the way a shell sources its rc file.
The same file is also the daemons' configuration, so `run_rc_file()` skips any
line whose first word is a known configuration key (`rc_is_directive()`) and
runs the rest as commands. That is what lets one file both configure the system
and bring it up. An `exit` inside the rc file is deliberately ignored, since it
would close the shell before the user ever saw a prompt.

### Daemon lifecycle

Both daemons use the standard double fork: `fork`, `setsid` to leave the
controlling terminal and become a session leader, `fork` again so the process
can never reacquire a terminal, then `chdir("/")` and reopen the standard
descriptors onto `/dev/null`. `daemonize no` in `.tetrishrc` skips it, which is
what the test harnesses use so a failure is visible on the terminal.

`umask(022)`, not `umask(0)`. A world-writable pid file means any local user can
overwrite it, and `kill $(cat ...pid)` then signals a process of their choosing.

### Shutdown is four phases, in order

`SIGTERM` (or `tetrisctl shutdown`) arrives on the signalfd as a normal loop
event, so shutdown begins from a known point in the loop rather than from a
handler interrupting arbitrary code.

1. **Stop accepting.** Remove the listener from the epoll set and close it.
   Parked connections that never finished a handshake are closed here too -
   they own a descriptor and nothing else, so phase 2 would not find them.
2. **Drain clients.** Disconnecting a client also tears down its room, that
   room's timerfd, and its session.
3. **Stop the logshipper and join it.** The join is what guarantees records
   already in the ring actually reach `tetrislogd` before the socket closes.
4. **Unlink.** The control socket, the pid file and the message queue are
   removed - using the names actually bound at startup, not whatever the
   configuration says by then, since a `SIGHUP` reload may have replaced it.

`SIGUSR1` dumps room and player state to the log without restarting, which is
the one-shot diagnostic for a server you do not want to interrupt.

---

## Concurrency and locking

**Two kinds of mutex exist.** One per room, protecting that room's per-seat
arrays (`players[]`, `games[]`, `pending[]`, `specs[]`), and one inside the log
ring buffer.

**Global acquisition order: room lock, then ring lock. Never the reverse.**
Both *are* held together - the tick handler, the garbage handler and `START`
all call `slog`/`rlog` while holding a room lock - so the order is real and is
respected at every site. Nothing that holds the ring lock ever touches a room.

**But the ordering is not what the safety argument rests on.** `ring_push()`
takes the ring mutex with `pthread_mutex_trylock` and gives up immediately if
it is busy. A thread holding a room lock can therefore never *wait* on the ring
lock, so there is no edge to close a cycle with, whatever order the two are
acquired in. Deadlock is structurally impossible.

**The rule that is load-bearing, and the one to check when adding code: a room
lock is never held across a blocking system call.** The tick handler is the
worked example - it advances every seat, renders the boards into one buffer,
and unlocks; only then does it `mq_send` the garbage and `tetrissh_send` the
STATE frames. That is also why `room_pick_garbage_target()` refuses to take a
second room lock: holding two room locks at once is the one shape that could
deadlock, so the injection site skips dead seats instead.

### The second thread

Exactly one other thread exists: the logshipper. It touches nothing but the
ring buffer and the socket to `tetrislogd`.

```
game path                          logshipper thread
---------                          -----------------
slog()/rlog()                      ring_pop_batch()   <- takes the lock, memcpy only
  ring_push()                      unlock
    trylock -> busy? drop, count   sendto()           <- the slow part, outside the lock
```

The producer never waits. If the lock is held or the ring is full, the record
is dropped and an atomic counter is incremented - the counter is atomic
precisely *because* we are not holding the lock at that moment. Gameplay never
blocks on logging, which is the requirement; the cost is that logging is
lossy under load, which is why the loss is counted rather than hidden.

Verified under ThreadSanitizer: 400,000 records, exact drop accounting, zero
races.

---

## IPC choices

Four channels, four mechanisms, each chosen for a property:

| Channel | Mechanism | Why |
|---|---|---|
| `tetrisd` -> `tetrislogd` | `AF_UNIX` `SOCK_DGRAM` | Connectionless. The logger can restart and `tetrisd` needs no reconnect logic at all - the next `sendto` simply lands. Non-blocking, so a full queue drops rather than stalling the game. |
| `tetrisctl` -> `tetrisd` | `AF_UNIX` `SOCK_STREAM` | Separate from the TCP listener, so the control plane survives a flooded game port. Carries HTTTP, the same wire format and the same parser as the game. |
| Battle-Royale garbage | POSIX message queue | `mqd_t` is a real descriptor on Linux, so it joins the epoll set directly. Bounded buffer with an explicit drop policy on `EAGAIN`. Cross-room transfer is genuinely message-passing, not a function call into another room's state. |
| Signals | `signalfd` | Signals become readable events in the same loop. No handler races the game state. |
| Room ticks | `timerfd` | One per active room, in the epoll set. No ticker thread. |

The control plane is local-only and its socket is created mode `0700`, by
setting the umask around `bind()` - `bind()` is what creates the socket node,
so it honours the umask like any other file creation. A `chmod()` afterwards
would leave a window where the socket exists at the wrong mode and accepts
whoever connects first.

---

## Security

### The session handshake

Every byte of HTTTP travels inside a session established at connect time:

1. Client connects and sends a fresh 32-byte nonce.
2. Server sends its X.509 certificate.
3. Client verifies the certificate against the bundled CA.
4. Server signs the client's nonce with its private key (RSA-PSS).
5. Client verifies the signature using the public key from the certificate.
6. Client generates a 32-byte AES-256 key, encrypts it under the server's
   public key (RSA-OAEP), and sends it.
7. Every frame after that is `[4-byte big-endian length][AES-256-CBC ciphertext]`,
   carrying one HTTTP message. Frames larger than 64 KiB are rejected with
   `413 Payload Too Large`.

Steps 1-5 are what make this an authentication rather than just an encryption:
the nonce is fresh per connection, so a recorded signature from an earlier
session proves nothing, and only the holder of the private key matching the
CA-signed certificate can produce a valid one.

**Why RSA-PSS and RSA-OAEP** rather than PKCS#1 v1.5: both v1.5 constructions
are vulnerable to padding-oracle attacks (Bleichenbacher). PSS and OAEP are
randomised and provably secure under standard assumptions, and are what modern
guidance specifies for new systems.

### I/O deadlines, and why socket timeouts are not enough

`SO_RCVTIMEO` and `SO_SNDTIMEO` are **per-syscall** timers. A `recv()` that
times out after copying some bytes returns the short count instead of failing,
so a reassembly loop advances and calls `recv()` again with a fresh, full
timeout. A peer that drips one byte per timeout period resets the clock forever
and never trips the limit.

Since all reads and writes run on the single epoll thread, that was a denial of
service: a peer needed only `connect()` - no certificate, no key exchange - to
hold the loop indefinitely and freeze every room on the server.

`libtetrissh` therefore stamps **one absolute deadline per logical operation** -
a whole handshake, or one framed message - and each syscall inside it gets only
the time remaining. Forward progress no longer buys the peer more time; the
budget only shrinks.

Two limits, deliberately not one:

- `idle_ms` - how long to wait for the peer's **first byte** (0 = forever).
- `budget_ms` - how long the operation gets **once the first byte arrives**.

The clock starts on the first byte. Waiting for a peer to begin speaking is not
the same as a peer speaking too slowly: a client idling between STATE
broadcasts is idle, not stalled, and bounding *that* disconnects every player
whenever the server goes quiet.

### Other protections

- **Identity is bound to the connection.** The server assigns the player id at
  handshake time. A request whose `Player-Id` header names a different player
  is rejected with 403, so a client cannot drive someone else's board by
  editing a header. (A *missing* header is also 403 today rather than 401 -
  see limitation 15.)
- **Per-IP connection limit**, enforced at `accept()` and counting parked
  handshakes as well as completed ones, so the queue cannot be filled from one
  address.
- **Per-client request rate limit** (60/second), answered with `429` and a
  `Retry-After` header rather than a disconnect: flooding is usually a buggy
  client, not an attack, and dropping the session would cost a real player
  their game.
- **Spectators hold no seat**, and the seat check is a bounds check as much as
  a permission check.
- **No path, port or credential is hard-coded**; everything comes from
  `.tetrishrc`.

### Threat model, stated honestly

Defended: passive eavesdropping on the game port; a server impersonator without
the CA-signed key; replay of an earlier session's handshake; a local
unprivileged user issuing control-plane commands; a client claiming another
player's identity; slow-drip and flooding clients.

**Not defended**, and worth naming rather than leaving implied:

- **No message integrity.** The frame carries no authentication tag - see
  limitation 1. This is the most significant one.
- **No per-message replay protection** inside a live session (limitation 2).
- **No forward secrecy.** The session key is wrapped under the server's
  long-term RSA key, so anyone who later obtains that key and has a recorded
  session can recover the key and decrypt it. An ephemeral Diffie-Hellman
  exchange is the standard fix and is what TLS moved to for this reason.
- **No certificate revocation.** The client checks the CA signature and the
  validity dates and nothing else, so a compromised certificate stays trusted
  until it expires. Revocation needs a CRL or OCSP, and a distribution channel
  for it.
- **Traffic analysis is possible.** Encryption hides the contents, not the
  shape: frame sizes and timings are visible, and a `STATE` broadcast arrives at
  a fixed rate, so an observer can infer that a game is running and roughly how
  many players are in it.
- **Compromise of the server host** ends the discussion - the private key and
  the control socket both live there, and the control plane trusts local file
  permissions rather than any credential.

---

## The HTTTP protocol

`HTTTP/1.0` literally, CRLF line endings, body after a blank line.

```
REQUEST  ::= REQUEST-LINE *(HEADER CRLF) CRLF [BODY]
REQUEST-LINE ::= METHOD SP PATH SP "HTTTP/1.0" CRLF
RESPONSE ::= STATUS-LINE *(HEADER CRLF) CRLF [BODY]
STATUS-LINE  ::= "HTTTP/1.0" SP STATUS-CODE SP REASON-PHRASE CRLF
```

| Method | Path | Body |
|---|---|---|
| `JOIN` | `/room/<id>` | - (header `X-Spectate: 1` to watch instead of play) |
| `LEAVE` | `/room/<id>` | - |
| `START` | `/room/<id>` | - |
| `MOVE` | `/room/<id>/player/<pid>` | `LEFT` / `RIGHT` |
| `ROTATE` | `/room/<id>/player/<pid>` | `CW` / `CCW` / `180` |
| `DROP` | `/room/<id>/player/<pid>` | `SOFT` / `HARD` |
| `HOLD` | `/room/<id>/player/<pid>` | none - the method is the whole instruction |
| `STATE` | `/room/<id>` | server-originated broadcast |

`HOLD` is the one input that carries no body, so `map_input()` tests the method
before it tests the body; requiring one would reject every well-formed `HOLD`.
It was also appended **last** in `htttp_method_t` on purpose, so every existing
value keeps the number it already had and a peer that never sends `HOLD` is
unaffected by the addition.

180-degree rotation is a third *body* rather than a fourth method, because it is
a rotation and the engine already had `TB_INPUT_ROTATE_180`. `CCW` is tested
before `CW` for the obvious reason.

`STATE` is the only server-originated message, so a client must read unprompted
frames while interleaving its own request/response cycles. The disambiguation
rule is one comparison: a frame beginning `HTTTP/` is a response to something
we sent; anything else is a pushed `STATE`.

### The STATE payload

One header line, then the grid, one row per line:

```
player p9 score 0 level 1 lines 0 over 0 next 6 hold -1 held 0
.....3....
....333...
   ...
.....g....
....ggg...
```

`.` is empty, `1`-`7` identify the piece type occupying a cell (matching the
client's colour pairs), `8` is garbage, and `g` marks the **ghost** - where the
active piece would land if hard-dropped now.

`next` is the piece that will spawn, `hold` is the stashed piece (`-1` when
empty) and `held` records whether hold has already been used for this piece.

**The ghost is computed server-side, and it has to be.** `tb_render` merges the
active piece into the grid, so by the time a board reaches the wire nothing
distinguishes falling cells from locked ones, and a client that owns no game
state could not reconstruct the projection. Sending the landing position as part
of the board is what lets a purely-rendering client draw it. A ghost cell only
ever replaces an *empty* cell, so a real block is never hidden behind the
projection of the piece about to land on it.

Status codes reachable today: 200, 201, 400, 403, 404, 409, 429, 500. `401` and
`413` are defined with reason phrases but have no live call site - see
limitations 14 and 15.
Headers: `Content-Length` on every message with a body, `Content-Type`
(`application/tetris-command` on client requests, `application/tetris-state` on
broadcasts), `Player-Id` on authenticated requests, `Date` in RFC 1123 on every
response, `Retry-After` on 429.

**Parser design: one-shot.** `htttp_parse_request(buf, len, &msg)`
takes a complete buffer and fills a message whose fields point *into* that
buffer - no allocation, no copying, and the caller owns the lifetime. This is
the right trade here because the session layer below already delivers whole
frames: length-prefixed framing means the protocol layer never sees a partial
message, so the incremental-feed machinery a streaming parser needs would be
complexity with no user. The control plane, which reads from a stream socket
and *can* see partial messages, handles that by looping until the parser stops
saying "incomplete" - the one place the distinction matters.

### Request dispatch

`dispatch_request()` is the single entry point for every parsed message. In
order: rate limit, then path parse, then method dispatch, then the seat and
identity checks inside each handler. The rate check runs *before* path parsing
so a flood costs the server as little work as possible. Every request is logged
with the status it was answered with, one line each, whatever happened to it.

---

## The game engine

`libtetrisbrain` is pure logic: no sockets, no files, no clock, no globals. It
is given a board and an input and produces a new board. That is what makes it
testable without a server, and what lets `tetrisd` and `tetrish-view` share one
implementation of the rules so live play and replay cannot disagree.

| Area | Choice |
|---|---|
| Rotation | **SRS**, with the standard wall-kick tables (a separate table for `I`) |
| Randomiser | **7-bag** fed by an `xorshift32` PRNG stored inside `tb_game` |
| Lock delay | **move reset**, with a cap on the number of resets |
| Gravity | 48 ticks per step at level 1, shortening with level |
| Scoring | per-clear table multiplied by level, plus t-spins, back-to-back, combo and perfect clear |
| Extras | hold (once per piece), ghost projection, configurable start level |

**The PRNG lives inside the game struct, not in a global.** That is the property
replay depends on: same seed plus same inputs produces a byte-identical game, so
the replay engine can reconstruct a session from a seed and a list of ticks.

**Lock delay is capped deliberately.** Move reset gives the player a grace
period that any move or rotation restarts - without a cap, spinning a piece in
place stalls the game forever. The cap bounds it, and after that the piece locks
regardless.

**Hold blocks a second use before the next lock** (`held_this_turn`), which
stops a piece cycling between hold and active to dodge gravity indefinitely.

**Garbage arrives from outside.** `tb_inject_garbage()` lifts the stack and
inserts rows with a hole pattern chosen by the caller. It is the one input to a
board that does not come from that board's own player, which is why replay
cannot derive it and every garbage event is written to the log.

### Room lifecycle and the tick

A room is created by the first `JOIN` and destroyed when its last participant
leaves; `room_join`, `room_leave` and `room_spectate` are the only ways in and
out. Spectators occupy a separate `specs[]` array with no seat, so a watcher can
never be indexed as a player.

`START` resolves the room's tick rate once, stores it on the room, arms a
`timerfd` at that period, and seeds every seat. Everything downstream reads the
rate from the room rather than from the live configuration, so a `SIGHUP`
between `START` and a late join cannot leave one player's lock delay
disagreeing with the ticker they are joining.

Each tick, in this order:

1. Take the room lock.
2. Apply each seat's pending input, then `tb_tick` for gravity and locking.
3. Note lines cleared; a clear of two or more becomes a garbage event.
4. Render every board into one buffer, and write the replay records.
5. **Unlock.**
6. Only now: `mq_send` the garbage, and `tetrissh_send` the `STATE` frames to
   every player *and* spectator through one send loop with one failure policy.

Steps 5 and 6 are the whole discipline: copy and render under the lock, do the
I/O outside it.

---

## The clients

### `tetrisu`

One `select()` watches three descriptors, and its timeout is the frame pacer:
stdin for keys, the TCP socket for frames, and a signalfd for `SIGINT`. The fd
set and the `timeval` are rebuilt every pass, because `select()` is destructive
on both. `EINTR` is treated as a frame in which nothing happened rather than an
error. `SIGINT` arriving on a signalfd rather than through a handler is the same
reasoning `tetrisd` uses: no handler runs while the renderer is mid-draw.

The client is **not** authoritative. It sends `MOVE`/`ROTATE`/`DROP` and renders
whatever `STATE` comes back; it never advances its own board and assumes the
server agreed. The wire layer lives in `net.c` so protocol changes stay out of
the render path.

`--local` is the exception, and deliberately behind a flag: a full offline game
against an in-process `tb_game`, no sockets, for playing and for demonstrating
the engine independently of the server.

### `tetrish-view`

Three modes over one binary:

- **Ops console** - a REPL over the control plane, with its own line editor and
  history. `rooms`, `players`, `kick` and friends, speaking the same HTTTP the
  daemon serves to `tetrisctl`.
- **`attach <room>`** - joins the game plane as a participant and streams that
  room's `STATE` frames live, so an operator can watch a game in progress.
- **Replay** - reconstructs a recorded session from the log alone, with
  `--verify` running it headless so a test script can assert on the result.

---

## Logging and the replay format

`tetrislogd` owns the log file; `tetrisd` only generates records. That split is
deliberate: a logger bug (full disk, slow write) cannot take the game down, and
it forces a genuine producer-consumer IPC channel rather than a function call.

Every record is tagged with a level (`info`, `warn`, `error`) and timestamped by
the logger as it is written. Connection events, session establishment, HTTTP
requests and responses, room state changes and admin actions are all logged.

Two record types share the file with the prose logging, prefixed so a reader
can pick them out:

```
E <mono_ns> <tick> <room> <player> <action> [params...]
S <mono_ns> <tick> <room> <player> <w> <h> <cells_hex> <seed>
```

The **tick number** is what makes replay exact rather than approximate.
`libtetrisbrain` advances gravity per tick and has no clock, so a reader with
only wall-clock timestamps would have to guess how many ticks elapsed between
two events. With the tick recorded, replay is mechanical: seed the engine, then
for tick 0..N apply whatever input that tick carried and call `tb_tick` once.

`tetrish-view` calls `tb_tick` - the same entry point `tetrisd` calls - so
replay cannot drift from live play, because there is only one implementation of
the rules. (Quake 3 uses the same discipline for demo playback.)

**Snapshots are for verification, not seeking.** A snapshot carries the board
but not the PRNG state or the position in the 7-piece bag, so jumping to one
would make every subsequent piece differ from the real game. Seeking therefore
replays from tick 0, which costs microseconds. Snapshots are instead compared
against the reconstruction as it goes; a mismatch means a record was lost, and
it is counted, reported on screen, and resynced so the viewer keeps showing
something truthful rather than silently diverging.

**Each snapshot re-states the seat's seed.** The `SEED` record is written once,
at `START` - which is the busiest moment the logging path ever sees, since a
hundred rooms starting together emit hundreds of records into a datagram queue
that holds about ten. Measured at 500 players, 25 of 500 sessions were
unreplayable because their one `SEED` record had been dropped. Re-stating the
seed in every snapshot means any surviving snapshot can seed a replay; the
snapshot schedule is also staggered per room so the periodic writes trickle
instead of bursting. Both together: 500/500 sessions replayable, and total log
drops fell from 1491 to 703.

---

## Configuration

Everything lives in `.tetrishrc`; `sample.tetrishrc` is the committed template.
Paths are relative to the project root and nothing is hard-coded.

Required: `listen_port`, `cert_path`, `key_path`, `ca_path`, `log_path`,
`log_ipc`, `ctl_path`. The daemons refuse to start without them.

Tunable: `bind`, `ctl_perm`, `pid_path`, `garbage_mq`, `max_rooms`,
`max_players_per_room`, `tick_hz`, `snapshot_interval`, `max_clients`,
`max_conns_per_ip`, `handshake_budget`, `tcp_backlog`, `tcp_keepalive`,
`daemonize`, `log_level`, `client_timeout`.

`SIGHUP` reloads the file. The new configuration is parsed into a temporary and
copied over the live one only on success, so a typo in the file cannot destroy
the running configuration. Listeners keep their original bindings; a running
room keeps the tick rate it was started with, because lock delay is measured in
ticks and a room whose seats disagreed with its own ticker would give one
player pieces that float three times as long as everyone else's.

---

## Testing

```sh
make test                      # engine, ring, garbage, parser (54), fuzzer (20k),
                               # control plane, and the ops-console suites
sh tests/gate8.sh              # the baseline gate; --valgrind runs it under valgrind
sh tests/smoke.sh              # record a session end to end, then replay it
sh tests/integration.sh        # full stub-client session over the real wire
sh tests/replay.sh             # rebuild a session from the log alone
sh tests/week10.sh             # Battle Royale, per-IP limit, fd cleanup
sh tests/capture-demo.sh       # tcpdump proof that post-handshake traffic is opaque
sh tests/stress500.sh          # 500 concurrent real-wire clients
```

Results on the current tree: **gate8 17/17**; week10 13/13; smoke and
integration both passing; every `make test` suite passing, including the parser
at 54 checks and the fuzzer surviving 20,000 mutations with no crash and no body
pointer escaping its buffer; and 500/500 clients completing the full protocol at
19.9-20.0 Hz with no descriptor leak. Under `--valgrind` the gate reports 0
errors and 0 leaks (23,604 allocations, 23,604 frees).

**One test currently fails, and it is a real gap rather than a flaky harness.**
`tests/replay.sh` is 6/7: the case that strips every `SEED` line from the log and
requires the session to reconstruct anyway. `tetrisd` still writes the seed into
every snapshot record, but the replay reader only reads `SEED` records and
refuses a log without one, so the durability the log format provides is not yet
used by the reader. Ordinary replay is unaffected.

`tests/capture-demo.sh` is the one worth running in front of someone: 120 board
rows crossed the socket and not one is readable in the capture, while the
certificate is plainly visible - which is correct, because a certificate is
public by definition.

---

## Known limitations

Things that are true and that we would rather state than have found.

**Security**

1. **Frames are not authenticated.** The wire format is
   `[length][IV][ciphertext]` with no MAC, so AES-CBC here is malleable: an
   attacker who can modify traffic can flip bits in a block and corrupt the
   plaintext in a predictable way. Confidentiality holds; integrity does not.
   The fix is an HMAC over the ciphertext, verified before decrypting.
2. **No per-message replay protection inside a live session.** The handshake
   nonce stops a *session* being replayed, but a frame captured earlier in the
   same session can be re-sent. A monotonic counter header rejected on
   duplicate would close it.
3. **Cryptographic primitives are called directly through OpenSSL's EVP API**
   rather than through PA2's `common.c` wrappers, which we did not have in this
   repository. We use OpenSSL only, no TLS and no `SSL_*`, and implement no
   primitives of our own - but this is a deviation from the handout's letter
   and we would rather name it.

**Parsed but not enforced**

4. `log_level` is read from the configuration and stamped on every record, but
   never used to filter one. Every level is written.
5. `client_timeout` is read and unused, so an idle authenticated client holds
   its slot indefinitely.

**Scale and capacity**

6. `tetrisctl rooms` and `players` **truncate** at 4096 bytes - roughly 66 rooms
   or 85 players. (This was a stack buffer *overflow* until recently: `vsnprintf`
   returns the length it would have written, so the offset ran past the end of
   the buffer and the next call underflowed its size computation. It now
   truncates cleanly. Returning the full list would need a streamed body.)
7. **Logging is lossy under load, by design.** The game path never blocks on
   logging, so records are dropped when the ring is contended or the datagram
   queue is full. Both causes are counted separately and exposed through
   `tetrisctl dropped-logs`. At 500 players roughly 700 records were dropped,
   essentially all at the socket rather than the ring.
8. **The drop counter lives in `tetrisd`, not `tetrislogd`.** With a datagram
   channel the receiver cannot observe a drop - a datagram that never arrives
   leaves no trace - so the count is kept where it is actually observable.
9. **Battle-Royale garbage is dropped when the message queue is full**, counted
   and logged. A dropped garbage row costs one player an easier board; blocking
   the tick would freeze every room.
10. `net.unix.max_dgram_qlen` is 10 in our container and 512 on a typical host,
    so log loss under burst is considerably worse in our test environment than
    it would be in deployment.

**Behaviour**

11. Writes in the tick handler are blocking. The per-operation deadline bounds
    them, and a client that misses it is disconnected, but a genuinely
    congested link still costs the loop that deadline once.
12. `.tetrishrc` is read by two different parsers, so each announces what it
    does not recognise: the daemons print `unknown directive 'dspawn'` for the
    shell commands in the file. Harmless, and the alternative - teaching the
    config parser about shell builtins - is worse coupling than the noise.
13. Room ids are restricted to letters, digits, dashes and underscores, checked
    at the edge because the id later appears inside JSON and log lines.
14. **An oversized frame closes the connection rather than answering `413`.**
    The 64 KiB limit is enforced in `recv_length_prefixed` on the declared
    length, before anything is read into memory or decrypted - so at the point
    of rejection there is no decrypted HTTTP message to reply to, and
    manufacturing a reply would mean trusting an unauthenticated length field.
    Dropping is the safer behaviour, but it is not what the handout describes,
    and a `413` for messages that exceed the limit after decryption would be
    the closer reading.
15. **`tetrislogd` does not emit a periodic drop summary.** The dropped-record
    count is maintained and readable through `tetrisctl dropped-logs`, but
    nothing prints a recurring `dropped N records in last 30s` line, which is
    what the handout asks for.

---

## Repository layout

```
project/
├── bin/            compiled binaries (gitignored)
├── lib/            libtetrissh.a  libhtttp.a  libtetrisbrain.a
├── include/        public headers
├── auth/           generate_keys.sh (certificates are generated, not committed)
├── src/
│   ├── tetrish/        launcher shell
│   ├── tetrisd/        game daemon: main.c, rooms.c, clients.c, ring.c
│   ├── tetrislogd/     logger daemon
│   ├── tetrisctl/      admin CLI
│   ├── tetrisu/        game client
│   ├── tetrish-view/   spectator + replay viewer (50.003)
│   ├── common/         rc.c (config), daemon.c (double fork), stateview.c
│   ├── libtetrissh/    secure session
│   ├── libhtttp/       protocol
│   └── libtetrisbrain/ game rules, pure logic
├── tests/          unit suites, fuzzer, integration and load harnesses
├── var/            log/ and run/ (created at runtime)
├── sample.tetrishrc
├── Dockerfile
└── Makefile
```

`src/tetrisu/net.c` holds the wire adapter - `JOIN`/`START`/`STATE`/`LEAVE` -
and is linked by `tetrish-view`'s `attach` as well as by the client itself, so
a player's view and an operator's view of a room are produced by one piece of
code and cannot drift apart.

`src/common/stateview.c` is an earlier extraction of the same idea, kept in the
tree but no longer linked by anything after the client refactor consolidated on
`net.c`.
