#ifndef TETRISSH_H
#define TETRISSH_H

/*
 * tetrissh.h — tetriSH-side compatibility shim over the STANDARDISED
 * libtetrissh implementation now shared with the 50.003 corestack app
 * (mini-gh-tracker): see include/corestack/secure_session.h and
 * src/corestack/secure_session.c.
 *
 * Both projects link the exact same secure_session.c (real nonce/cert/
 * RSA-PSS/RSA-OAEP/AES-256-CBC handshake, no OpenSSL SSL_* / TLS). This
 * header just re-exposes it under tetrisd/tetrisu/tetrisctl's original
 * tetrissh_* names so none of their call sites had to change.
 *
 * Old alloc-then-handshake-in-place pattern:
 *     tetrissh_session_t *sess = tetrissh_session_alloc();
 *     tetrissh_handshake_server(sess, cfd, cert_path, key_path);
 *     ...
 *     tetrissh_session_free(sess);
 *
 * is preserved here on top of corestack's atomic
 * session_server_init()+session_accept() pair: tetrissh_handshake_server()
 * builds a short-lived session_ctx_t from (cert_path,key_path), performs
 * the handshake, and stores the resulting session_t* inside the wrapper.
 */

#include <stddef.h>
#include <stdint.h>
#include "corestack/secure_session.h"

/* ───────────────────────── constants ─────────────────────────────────────── */

#define TETRISSH_NONCE_LEN        SESSION_NONCE_LEN
#define TETRISSH_SESSION_KEY_LEN  SESSION_KEY_LEN
#define TETRISSH_MAX_FRAME_LEN    SESSION_MAX_FRAME_LEN
#define TETRISSH_FRAME_HDR_LEN    4

/* ───────────────────────── session state ─────────────────────────────────── */

/**
 * tetrissh_session_t
 *
 * Thin wrapper around a corestack session_t*. Heap-allocated by
 * tetrissh_session_alloc(); must be freed with tetrissh_session_free().
 * Neither tetrisd nor tetrisu should inspect the fields directly.
 */
typedef struct tetrissh_session {
    session_t *inner;      /* NULL until a handshake succeeds */
    char       last_error[512];
} tetrissh_session_t;

/* ───────────────────── lifecycle ─────────────────────────────────────────── */

tetrissh_session_t *tetrissh_session_alloc(void);
void tetrissh_session_free(tetrissh_session_t *sess);

/* ───────────────────── handshake ─────────────────────────────────────────── */

/**
 * tetrissh_handshake_server / tetrissh_handshake_client
 *
 * Same handshake steps described in the tetriSH handout's libtetrissh
 * section, now performed by corestack/secure_session.c underneath.
 * Returns: 0 on success, -1 on any handshake failure.
 */
int tetrissh_handshake_server(tetrissh_session_t *sess, int sockfd, const char *cert_path, const char *key_path);
int tetrissh_handshake_client(tetrissh_session_t *sess, int sockfd, const char *ca_path);

/* ───────────────────── framed send / recv ────────────────────────────────── */

/**
 * tetrissh_send / tetrissh_recv
 *
 * One call == one HTTTP message == one encrypted frame
 * ([4-byte BE length][AES-256-CBC ciphertext]), via corestack's
 * session_write()/session_read(). tetrissh_recv() mallocs the returned
 * plaintext buffer; caller must free() it.
 */
int tetrissh_send(tetrissh_session_t *sess, int sockfd, const unsigned char *plaintext, size_t plain_len);
unsigned char *tetrissh_recv(tetrissh_session_t *sess, int sockfd, size_t *out_len);

/* ───────────────────── teardown ──────────────────────────────────────────── */

/**
 * tetrissh_close
 *
 * Signals the end of the secure session. Does NOT close the underlying
 * socket (the caller owns that — same convention corestack uses), but
 * zeroes the session key and marks the session closed so subsequent
 * send/recv calls fail immediately.
 */
void tetrissh_close(tetrissh_session_t *sess);

/* ───────────────────── utility ───────────────────────────────────────────── */

int tetrissh_is_ready(const tetrissh_session_t *sess);
const char *tetrissh_strerror(const tetrissh_session_t *sess);

/* True iff the last tetrissh_recv() failure was specifically an
 * oversized frame (peer declared >64 KiB), not a garbled frame or a
 * plain disconnect. tetrisd uses this to reply 413 before dropping the
 * connection, instead of dropping it silently like other failures. */
int tetrissh_last_recv_was_oversized(const tetrissh_session_t *sess);

#endif