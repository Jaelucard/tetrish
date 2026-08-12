# tetriSH

A secure, from-scratch Tetris server/client pair built around two shared
libraries: `libtetrissh` (a custom TCP secure-session handshake — nonce,
certificate, RSA-PSS signature, RSA-OAEP session key wrap, AES-256-CBC
framed messages — explicitly **not** TLS) and `libhtttp` (a small HTTP-like
request/response protocol, wire version `HTTTP/1.0`, used for both the game
protocol and the ops-console admin protocol).

`libtetrissh` lives in the shared `corestack` and is also linked, unmodified,
by a second, unrelated 50.003 application (`mini-gh-tracker`) to prove it's
a genuinely reusable library and not something with tetriSH baked into it.

## Building

```bash
make libs      # libtetrissh.a, libhtttp.a
make bins      # tetrisd, tetrisu, tetrislogd, tetrisctl
make test      # all test harnesses (see "Testing" below)
bash auth/generate_keys.sh   # one-time: generates a CA + server cert/key
```

## Running

```bash
cp sample.tetrishrc .tetrishrc
mkdir -p var/log var/run
bin/tetrislogd .tetrishrc &
bin/tetrisd .tetrishrc &
bin/tetrisu .tetrishrc room1
# interactive client: a/d move, w rotate, s soft drop, SPACE hard drop, g start, q quit
```

## Testing

| Binary | Deliverable | What it proves |
|---|---|---|
| `build/tests/test_handshake` | Week 5 | All 7 handshake steps, both ends in one process via `socketpair()`, the frame layer + 64 KiB cap, and the 3 required failure paths (bad cert, bad signature, truncated key blob). Valgrind-clean (0 errors, 0 leaks) — see below. |
| `build/tests/stub_client` | Week 6/7 | Full protocol round trip against a live `tetrisd`: handshake → JOIN → START → STATE stream → MOVE → LEAVE. |
| `build/tests/fuzz_malformed` | Week 8 | 9 malformed-input cases against a live `tetrisd`; daemon never crashes, and a normal JOIN still works afterward. |
| `build/tests/test_admin` | Week 9/10 | Admin protocol role gating (401/403/404) and two concurrent ops consoles spectating the same room without interference. |

```bash
# Valgrind, the Week 5 deliverable:
valgrind --leak-check=full --show-leak-kinds=all build/tests/test_handshake
#   => 0 errors from 0 contexts, all heap blocks freed -- every X509*/EVP_PKEY* is freed.
```

`docs/tetrish_encrypted_traffic.pcap` is a real loopback capture (via
`tcpdump`) of a full handshake + JOIN/START/STATE/MOVE/LEAVE session. Open
it in Wireshark: the certificate exchange is plaintext PEM (expected — the
cert is meant to be public), and every message after the session key is
established is high-entropy ciphertext. Searching the whole capture for the
literal strings `JOIN`, `STATE`, `HTTTP`, or `player ` — all of which appear
constantly in the actual protocol — returns zero matches anywhere outside
the certificate blob.

## Error code → status code mapping

`libhtttp`'s parser returns an internal `htttp_err_t`; the daemon maps every
non-OK parse result to one HTTP status before replying. This mapping is
intentionally coarse for parse errors (almost everything is a 400) and
precise for protocol-level failures (401/403/404/409/413/429/500), so that a
client can tell "you sent garbage" apart from "you sent something
well-formed but not allowed."

| `htttp_err_t` | Meaning | Status sent |
|---|---|---|
| `HTTTP_ERR_INCOMPLETE` | Message ends mid-header/mid-body (e.g. Content-Length says more than we got) | 400 |
| `HTTTP_ERR_MALFORMED` | Bad grammar: missing CRLF, no leading `/` on the path, Content-Length says *less* than we got, garbage bytes | 400 |
| `HTTTP_ERR_BAD_VERSION` | Version field present but not exactly `HTTTP/1.0` | 400 |
| `HTTTP_ERR_TOO_LARGE` | Content-Length exceeds the parser's body limit | 400 (see also the frame-layer 413 below) |
| `HTTTP_ERR_NO_MEMORY` | Serialiser allocation failed | 500 |
| `HTTTP_ERR_BAD_HEADER` | A required header is missing/invalid, or a body is present with no (or a zero) Content-Length | 400 |
| *(frame layer, below `htttp_parse_request`)* | Declared frame length exceeds the 64 KiB cap | **413**, distinguished from every other frame failure — see `session_last_recv_was_oversized()` |
| *(dispatch layer)* | Method not in the request-line grammar at all | 400 |
| *(dispatch layer)* | `Player-Id` header missing/doesn't match this connection's assigned id | 401 |
| *(dispatch layer)* | Player-Id valid, but wrong room / not your board / role too low for an admin method | 403 |
| *(dispatch layer)* | Room or player doesn't exist | 404 |
| *(dispatch layer)* | Already in a room / room already started | 409 |
| *(dispatch layer)* | More than 50 requests/second on one connection | 429 |
| *(dispatch layer)* | Internal failure (e.g. timerfd creation) | 500 |

All ten required codes (200, 201, 400, 401, 403, 404, 409, 413, 429, 500)
are reachable and covered by `fuzz_malformed`/`stub_client`/`test_admin`.

## Admin protocol (Week 9-11)

Four methods, over the *same* encrypted `libtetrissh` session and
`libhtttp` grammar game clients use — not a separate channel:

| Method | Path | Role required | Response |
|---|---|---|---|
| `ADMIN-STATUS` | `/admin/status` | readonly | `200 {"rooms": N, "players": N, "dropped_logs": N, "your_role": "..."}` |
| `ADMIN-ROOMS` | `/admin/rooms` | readonly | `200` — JSON array, same shape as the local ctl plane's `/rooms` |
| `ADMIN-ATTACH` | `/room/<id>` | readonly | `200 {"attached": true}`, then this connection receives the same `STATE` broadcasts the room's own players get, read-only, until it disconnects. `404` if the room doesn't exist. |
| `ADMIN-KICK` | `/room/<id>/player/<id>` | **full** | `200 {"kicked": "<id>", "room": "<id>"}` and the named player's connection is closed. `404` if no such player is in that room. |

**Authentication:** an `Admin-Token` header, checked against
`auth/admin_tokens` (`<token> <role>` per line, `#` comments, one line per
credential). Unknown/missing token → `401`. Token lookup uses a
constant-time comparison (see `src/tetrisd/admin.c`) so a network observer
timing responses can't binary-search a valid token.

**Permission model (Week 11: read-only admin vs. full admin):**
- `readonly` — `ADMIN-STATUS`, `ADMIN-ROOMS`, `ADMIN-ATTACH`. None of these
  can change any game or server state, even `ADMIN-ATTACH`: it only
  subscribes the connection to a copy of the room's own broadcast, the same
  bytes the room's players already receive every tick.
- `full` — everything `readonly` can do, plus `ADMIN-KICK`, the one
  network-reachable action that mutates state (it disconnects a player).

**Enforcement happens once, before dispatch:** every `ADMIN-*` request
resolves its token to a role and checks that role against the method's
required role in a single block at the top of the admin case in
`dispatch_request()` (`src/tetrisd/main.c`), before any method-specific
handler logic runs. A request that fails this check never reaches the
handler that would act on it.

**Rejections return a useful reason, not just a bare 403.** `admin_forbidden_body()`
(`src/tetrisd/admin.c`) builds a JSON body naming both the role the caller
has and the role the method requires, e.g.:
```json
{"error": "ADMIN-KICK requires the full role", "your_role": "readonly", "required_role": "full"}
```

**Deliberately not exposed here:** anything that affects the daemon's own
availability — shutting it down, forcing a room closed outright, editing
the admin token table. Those stay on tetrisd's existing local-only
Unix-domain-socket control plane (`tetrisctl`), which is a strictly
stronger boundary than "holds the right network token": it additionally
requires a local shell on the host. `ADMIN-KICK` was judged to be the
right amount of "full admin" to expose over the network precisely because
its blast radius is bounded to one player's connection — it can never
affect the daemon's own uptime the way a network-reachable shutdown could.

**Concurrency:** `test_admin` connects two independent sessions, attaches
both to the same room, and confirms both receive a full, independent
stream of `STATE` frames (`client_foreach` fans the same wire bytes out to
every attached spectator per tick — see `handle_room_tick` in
`src/tetrisd/main.c`).

## Security assumptions (Week 11)

Everything above — the threat model, the admin permission model, the
reentrancy claims — rests on a handful of things this project assumes are
true rather than defends against. If any of these don't hold in a given
deployment, the guarantees above don't either:

- **The server's private key (`auth/server.key`) never leaves the machine
  running `tetrisd`, and is only readable by the account running it.** The
  entire handshake's authenticity guarantee (the client trusts *this*
  server) reduces to "whoever holds this key". We assume normal filesystem
  permissions enforce this; the protocol has no way to detect or recover
  from a leaked key short of rotating it and reissuing the CA-signed cert.
- **The CA's private key (`auth/ca.key`) is at least as well-protected as
  the server key, and ideally kept offline entirely after issuing the
  server cert.** Anyone holding it can mint a cert any client here will
  trust — this project has no CRL/OCSP check (see the Threat Model), so
  there's no way to invalidate a cert minted by a compromised CA key
  short of every client updating which CA it trusts.
- **`auth/admin_tokens` is only readable by the account running `tetrisd`.**
  These are bearer credentials with no expiry, no per-token scoping beyond
  the two roles, and no rotation mechanism built in. We assume an operator
  treats this file with the same care as a password file, and rotates it
  (rewrites the file, restarts `tetrisd`) if a token might have leaked.
- **The host's system clock is roughly correct.** Two things depend on
  this: certificate validity-window checks (Week 12 tests this — see
  "Certificate validation edge cases" below) and the `Date` header on every
  response. Neither the client nor the server independently verifies the
  other's clock; a sufficiently wrong local clock could cause either a
  valid cert to be rejected as expired, or an expired cert to be
  incorrectly accepted.
- **The local Unix-domain-socket control plane (`tetrisctl`) is exactly as
  trusted as shell access to the host.** It has no authentication of its
  own — deliberately, per the Admin Protocol section above — because we
  assume anyone who can open that socket already has an equivalent or
  greater level of access (a local shell), so adding a credential check
  there would protect against nothing a real attacker with that access
  couldn't already do another way.
- **`generate_keys.sh` / `generate_certs.sh` are run in a trusted
  environment**, and their output (private keys in particular) isn't
  accidentally committed to version control or shipped in a build
  artifact. This project's `.gitignore`-equivalent hygiene (deleting
  generated `auth/*.key`/`*.crt` before packaging) is a project-level
  convention, not something the protocol itself enforces.
- **One `tetrisd` process serves one CA's worth of trust.** There's no
  mechanism here for a client to trust multiple independent CAs or to pin
  a specific cert beyond "signed by the one CA I loaded" — appropriate for
  this project's single-deployment model, but worth stating explicitly
  since it's an assumption, not a proven property.

## Testing: Week 12 additions

Beyond the Week 8 `fuzz_malformed` cases, two more test binaries specifically
target the Week 12 deliverables:

| Binary | Covers |
|---|---|
| `build/tests/fuzz_week12` | 15 rounds of random-length/random-content frames (seeded via an optional CLI arg for reproducibility), 3 truncated-message cases (a frame that declares more bytes than the client actually sends before disconnecting), a 50 KB single header value, a 40-header message (over the 32-header cap), and invalid UTF-8 byte sequences in a header value. All against a live `tetrisd`, followed by a sanity JOIN to confirm the daemon is still fully healthy. |
| `build/tests/test_handshake` (TEST 5, TEST 6) | **Half-open handshakes**: the client disconnects before sending its nonce, right after sending its nonce, and after reading the cert+signature but before sending any key blob at all — three distinct points, each proven to fail cleanly server-side rather than hang (backstopped by the harness's own `alarm(45)` deadlock detector). **Certificate validity edge cases**: an expired certificate and a not-yet-valid certificate (both generated via `faketime` so their `notBefore`/`notAfter` land outside the real clock's current time), each correctly rejected by the client's `session_connect()`. |

```bash
build/tests/fuzz_week12 .tetrishrc [seed]   # against a live tetrisd
build/tests/test_handshake                   # standalone, no daemon needed
```

## Reentrancy audit (Week 9)

`src/corestack/secure_session.c` — the implementation shared verbatim with
`mini-gh-tracker` — has:
- **Zero global variables.** (`grep -n "static"` in the file matches only
  function definitions, never a static local or file-scope variable.)
- **Zero hardcoded paths.** Cert/key/CA paths are parameters to
  `session_server_init()`/`session_client_ctx_init()`, stored in a
  heap-allocated `session_ctx_t` the *caller* owns — never compiled in,
  never a global.
- All per-connection state (the AES session key, the read buffer, the last
  error string) lives in a heap-allocated `session_t` returned from
  `session_accept()`/`session_connect()`. Two callers, two independent
  `session_t`s, no shared mutable state between them.

This isn't just a code-reading claim: `mini-gh-tracker` links this exact
object file (verified `diff`-identical to tetriSH's copy) with its own
cert/key/CA, and the two daemons have been run at the same time, on
different ports, each independently completing real handshakes, without
touching each other's state in any way. That's the practical version of "a
second application can link it cleanly."

## Threat model

**In scope / defended against:**
- **Passive network eavesdropping.** Everything after the handshake is
  AES-256-CBC encrypted under a key only the two endpoints ever see in the
  clear (established via RSA-OAEP, itself only decryptable by the holder of
  the server's private key). See the pcap analysis above.
- **On-path tampering / MITM without the private key.** The client verifies
  the server's certificate against a pinned CA and verifies an RSA-PSS
  signature over a client-chosen nonce before trusting anything the server
  says. An attacker without the CA-issued cert's private key cannot
  complete a handshake as the server.
- **Malformed/malicious input crashing the daemon.** Every parse error path
  returns a typed error instead of touching memory outside what was
  validated; `fuzz_malformed` exercises 9 classes of bad input against a
  live daemon and the process survives every one, still answering normal
  requests afterward.
- **Resource exhaustion via oversized messages.** The frame layer rejects
  any declared length over 64 KiB *before* attempting to read or allocate
  for the payload (`recv_length_prefixed`'s length check happens before the
  `malloc`), so a peer can't make the server allocate an attacker-chosen
  amount of memory just by claiming a huge frame.
- **A single connection monopolizing the daemon.** The per-connection
  429 rate limit (50 req/s) bounds how much CPU/lock time one client can
  demand.
- **Identity spoofing between players.** The server assigns `Player-Id` at
  JOIN time and ties it to the connection; a request whose `Player-Id`
  header doesn't match gets 401/403 rather than being allowed to control
  someone else's board.
- **Admin-token guessing via timing.** Token lookup is constant-time-ish
  (see `admin.c`); it doesn't return early on the first match and pads the
  length comparison too.

**Explicitly out of scope for this project:**
- **Endpoint compromise.** If the server's private key or an admin token
  leaks, the protocol offers no defense — this is true of essentially any
  keyed protocol and is not specific to `libtetrissh`.
- **Denial of service via raw connection volume** (as opposed to
  oversized/malformed messages on an established connection). `tetrisd`
  doesn't currently rate-limit new TCP connections or handshake attempts
  per source IP; a flood of connection attempts could still consume file
  descriptors / CPU on RSA operations. A production deployment would want
  this in front of `tetrisd` (e.g. connection-rate limiting at the OS or a
  reverse proxy layer) rather than inside the game protocol itself.
- **Traffic analysis.** Frame sizes and timing are still observable on the
  wire even though contents are encrypted (this is inherent to the
  fixed-frame design, not a bug — see the 413 discussion above, where we
  accept a minor size oracle deliberately rather than obscure a value
  that's already visible via TCP byte counting).
- **Forward secrecy.** The session key is wrapped with the server's static
  RSA key; if that key is later compromised, a recorded session could in
  principle be decrypted. A production system would want an ephemeral
  (EC)DH step for this; it's out of scope for the handout's fixed 7-step
  handshake.
- **Certificate revocation.** There's no CRL/OCSP check; a compromised but
  not-yet-expired server cert would still verify. Out of scope given the
  handout's single self-signed CA model.
