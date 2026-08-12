#ifndef CORESTACK_SECURE_SESSION_H
#define CORESTACK_SECURE_SESSION_H

/*
 * corestack/secure_session.h  —  libtetrissh: the shared secure-session
 * library described in the tetriSH handout ("Libraries > libtetrissh").
 *
 * This header is the ONE contract used by every corestack consumer:
 *   - 50.005 side : tetrisd (server) and tetrisu (client)
 *   - 50.003 side : mini-gh-tracker (server-only; browsers/harness speak
 *                   this same handshake instead of TLS)
 *
 * Protocol (fixed by the handout, do not deviate):
 *   1. Client connects, sends a fresh nonce.
 *   2. Server sends its X.509 certificate.
 *   3. Client verifies the certificate against the bundled CA.
 *   4. Server signs the client nonce with its private key (RSA-PSS).
 *   5. Client verifies the signature using the public key from the cert.
 *   6. Client generates a 32-byte AES-256 session key, RSA-OAEP encrypts
 *      it with the server's public key, sends it.
 *   7. Every subsequent frame is [4-byte BE length][AES-256-CBC ciphertext],
 *      carrying one HTTTP message. Frame size limit: 64 KiB.
 *
 * Cryptographic primitives come from common.c only (OpenSSL EVP). This
 * library never uses OpenSSL's SSL_* (TLS) API -- that would be HTTPS, which
 * the handout explicitly forbids ("No HTTPS ... over TCP").
 *
 * Statically linked into every binary that needs it (see the handout's
 * `lib/libtetrissh.a`).
 */

#include <sys/types.h>   /* ssize_t */
#include <stddef.h>

#define SESSION_NONCE_LEN        32
#define SESSION_KEY_LEN          32              /* AES-256 */
#define SESSION_MAX_FRAME_LEN    (64 * 1024)      /* 64 KiB, per handout */

/* Opaque types. Consumers must not inspect fields directly. */
typedef struct session_ctx session_ctx_t;   /* long-lived, side-wide context (cert/key or CA)   */
typedef struct session     session_t;       /* per-connection state, produced by a handshake    */

/* ─────────────────────────── context lifecycle ──────────────────────────── */

/* Server-side context: loads (paths to) the server certificate/private key.
 * Call once at startup, share across all accepted connections. */
session_ctx_t *session_server_init(const char *cert_path, const char *key_path);
void            session_server_shutdown(session_ctx_t *ctx);

/* Client-side context: records the CA path used to verify the server cert.
 * Call once at startup, share across all outgoing connections. */
session_ctx_t *session_client_ctx_init(const char *ca_path);
void            session_client_ctx_free(session_ctx_t *ctx);

/* ─────────────────────────── handshake ───────────────────────────────────── */

/* Runs the SERVER side of the handshake on an already-accept()-ed raw
 * socket fd, using the cert/key loaded into `ctx`.
 * Returns a ready session_t* on success (caller owns it), NULL on failure. */
session_t *session_accept(session_ctx_t *ctx, int raw_fd);

/* Runs the CLIENT side of the handshake on an already-connect()-ed raw
 * socket fd, verifying the server certificate against the CA in `ctx`.
 * Returns a ready session_t* on success (caller owns it), NULL on failure. */
session_t *session_connect(session_ctx_t *ctx, int raw_fd);

/* ─────────────────────────── encrypted I/O ───────────────────────────────── */

/* Byte-stream semantics over the framed AES messages, i.e. the same
 * contract as read(2)/write(2): returns bytes transferred, <=0 on
 * error/EOF. Internally, session_read() pulls and decrypts one whole
 * [len][ciphertext] frame at a time and serves it out of an internal
 * buffer, so callers (e.g. libhtttp) may request arbitrarily small
 * or large chunks, exactly like reading off a TCP socket. */
ssize_t session_read(session_t *s, void *buf, size_t len);
ssize_t session_write(session_t *s, const void *buf, size_t len);

void session_close(session_t *s);

/* ─────────────────────────── utility ─────────────────────────────────────── */

int session_is_ready(const session_t *s);

/* True iff the most recent session_read() on `s` failed specifically
 * because the peer's declared frame length exceeded SESSION_MAX_FRAME_LEN
 * (as opposed to a garbled/undecryptable frame or a plain disconnect).
 * Callers that want to answer oversized frames with an explicit 413
 * response (rather than just dropping the connection) check this before
 * disconnecting. See htttp's error-code-to-status-code mapping doc. */
int session_last_recv_was_oversized(const session_t *s);

/* Human-readable string describing the last error on this session.
 * Valid until the next call on `s`. */
const char *session_strerror(const session_t *s);

#endif
