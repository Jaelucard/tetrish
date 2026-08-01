/**
 * tetrissh.h  —  libtetrissh: Secure Session Library for tetriSH
 *
 * Implements the PA2-style authenticated, confidential session handshake
 * between tetrisu (client) and tetrisd (server).
 *
 * Session protocol (required by spec):
 *   1. Client connects, sends a fresh nonce (32 random bytes).
 *   2. Server sends its X.509 certificate (PEM, length-prefixed).
 *   3. Client verifies the certificate against bundled CA (cacsertificate.crt).
 *   4. Server signs the client nonce with its private key (RSA-PSS).
 *   5. Client verifies the signature using the cert's public key.
 *   6. Client generates a 32-byte AES-256 session key, RSA-OAEP encrypts
 *      it with the server's public key, sends it.
 *   7. All subsequent frames are [4-byte BE length][AES ciphertext].
 *      Frames are limited to 64 KiB; larger payloads are rejected.
 *
 * Crypto primitives come exclusively from common.c / common.h (PA2 rules).
 * No OpenSSL SSL_* API or TLS is used here.
 *
 * Memory ownership:
 *   Every function that allocates heap memory documents the caller's
 *   responsibility to free it (via free(), EVP_PKEY_free(), X509_free()).
 *
 * Error convention:
 *   int-returning functions return 0 on success, -1 on failure.
 *   Pointer-returning functions return NULL on failure.
 *   All failures are printed to stderr before returning.
 */

#ifndef TETRISSH_H
#define TETRISSH_H

#include <stddef.h>
#include <stdint.h>

/* ───────────────────────── constants ─────────────────────────────────────── */

/** Random nonce length (bytes) sent by the client at the start of the handshake. */
#define TETRISSH_NONCE_LEN 32

/** AES-256 session key length (bytes). Matches SESSION_KEY_LEN in common.h. */
#define TETRISSH_SESSION_KEY_LEN 32

/** Maximum encrypted frame payload (bytes). Frames exceeding this are rejected. */
#define TETRISSH_MAX_FRAME_LEN (64 * 1024)

/** Wire: length-prefix field width (bytes) for each encrypted frame. */
#define TETRISSH_FRAME_HDR_LEN 4

/* ───────────────────────── session state ─────────────────────────────────── */

/**
 * tetrissh_session_t
 *
 * Opaque structure holding all per-connection session state after a
 * successful handshake. Heap-allocated by tetrissh_session_alloc();
 * must be freed with tetrissh_session_free().
 *
 * Neither tetrisd nor tetrisu should inspect the fields directly; use
 * the API functions below.
 */
typedef struct tetrissh_session tetrissh_session_t;

/* ───────────────────── lifecycle ─────────────────────────────────────────── */

/**
 * tetrissh_session_alloc
 *
 * Allocates and zero-initialises a new session object.
 * Must be freed with tetrissh_session_free() when the connection closes.
 *
 * Returns: pointer on success, NULL on allocation failure.
 */
tetrissh_session_t *tetrissh_session_alloc(void);

/**
 * tetrissh_session_free
 *
 * Frees all resources owned by the session (including the AES key,
 * any EVP_PKEY or X509 pointers still held). Safe to call with NULL.
 */
void tetrissh_session_free(tetrissh_session_t *sess);

/* ───────────────────── handshake ─────────────────────────────────────────── */

/**
 * tetrissh_handshake_server
 *
 * Runs the server side of the handshake on an already-accepted TCP socket.
 * On success the session is ready to send/receive encrypted frames.
 *
 * @param sess        Session object (from tetrissh_session_alloc).
 * @param sockfd      Connected TCP socket file descriptor.
 * @param cert_path   Path to the server's PEM certificate (server.crt).
 * @param key_path    Path to the server's PEM private key (server.key).
 *
 * Handshake steps performed here:
 *   - Receive client nonce.
 *   - Send PEM certificate (length-prefixed).
 *   - Sign nonce with RSA-PSS private key; send signature.
 *   - Receive RSA-OAEP-encrypted session key; decrypt with private key.
 *   - Store session key in sess for subsequent send/recv calls.
 *
 * Returns: 0 on success, -1 on any handshake failure.
 */
int tetrissh_handshake_server(tetrissh_session_t *sess, int sockfd, const char *cert_path, const char *key_path);

/**
 * tetrissh_handshake_client
 *
 * Runs the client side of the handshake on a connected TCP socket.
 * On success the session is ready to send/receive encrypted frames.
 *
 * @param sess        Session object (from tetrissh_session_alloc).
 * @param sockfd      Connected TCP socket file descriptor.
 * @param ca_path     Path to the CA certificate (cacsertificate.crt).
 *
 * Handshake steps performed here:
 *   - Generate and send random nonce.
 *   - Receive server certificate; verify against CA.
 *   - Receive RSA-PSS signature over the nonce; verify against cert pubkey.
 *   - Generate 32-byte AES session key; RSA-OAEP encrypt with server pubkey;
 *     send encrypted key.
 *   - Store session key in sess for subsequent send/recv calls.
 *
 * Returns: 0 on success, -1 on any handshake failure.
 */
int tetrissh_handshake_client(tetrissh_session_t *sess, int sockfd, const char *ca_path);

/* ───────────────────── framed send / recv ────────────────────────────────── */

/**
 * tetrissh_send
 *
 * Encrypts `plaintext` with the session AES key and sends it over the
 * socket as a framed message:
 *
 *   [ 4-byte BE payload length ][ AES ciphertext (IV + ciphertext + HMAC) ]
 *
 * Rejects payloads larger than TETRISSH_MAX_FRAME_LEN before encryption.
 *
 * @param sess        Established session.
 * @param sockfd      Connected socket.
 * @param plaintext   Data to send.
 * @param plain_len   Length of plaintext.
 *
 * Returns: 0 on success, -1 on encryption or send failure.
 */
int tetrissh_send(tetrissh_session_t  *sess, int sockfd, const unsigned char *plaintext, size_t plain_len);

/**
 * tetrissh_recv
 *
 * Reads one framed message from the socket, decrypts it, and returns the
 * plaintext. The caller must free() the returned buffer.
 *
 * Rejects frames whose declared length exceeds TETRISSH_MAX_FRAME_LEN.
 *
 * @param sess        Established session.
 * @param sockfd      Connected socket.
 * @param out_len     Set to the plaintext length on success.
 *
 * Returns: malloc-ed plaintext buffer on success, NULL on failure.
 *          Caller must free().
 */
unsigned char *tetrissh_recv(tetrissh_session_t *sess, int sockfd, size_t *out_len);

/* ───────────────────── teardown ──────────────────────────────────────────── */

/**
 * tetrissh_close
 *
 * Signals the end of the secure session.  Does NOT close the underlying
 * socket (the caller owns that), but zeroes the session key and marks
 * the session as closed so subsequent send/recv calls fail immediately.
 *
 * @param sess  Established (or partially established) session.
 */
void tetrissh_close(tetrissh_session_t *sess);

/* ───────────────────── utility ───────────────────────────────────────────── */

/**
 * tetrissh_is_ready
 *
 * Returns 1 if the handshake has completed successfully and the session
 * key is in place, 0 otherwise.
 */
int tetrissh_is_ready(const tetrissh_session_t *sess);

/**
 * tetrissh_strerror
 *
 * Returns a human-readable string describing the last error that occurred
 * in `sess`.  The returned pointer is valid until the next call on `sess`.
 */
const char *tetrissh_strerror(const tetrissh_session_t *sess);

#endif