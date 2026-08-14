/*
 * tetrissh.h - libtetrissh: the authenticated, confidential session
 * between tetrisu (client) and tetrisd (server). PA2 rules apply.
 *
 * The handshake is fixed by the spec. All seven steps, in order:
 *   1. Client connects, sends a fresh nonce (32 random bytes).
 *   2. Server sends its X.509 certificate (PEM, length-prefixed).
 *   3. Client checks that cert against the bundled CA (cacsertificate.crt).
 *   4. Server signs the client nonce with its private key (RSA-PSS).
 *   5. Client verifies that signature with the cert's public key.
 *   6. Client generates a 32-byte AES-256 session key, RSA-OAEP encrypts
 *      it under the server's public key, sends it.
 *   7. Everything after that is [4-byte BE length][AES ciphertext].
 *      Frames cap at 64 KiB; anything bigger is rejected.
 *
 * Crypto comes from common.c / common.h and nowhere else. No SSL_*, no TLS.
 *
 * Ownership: anything handed back on the heap says who frees it and with
 * what (free(), EVP_PKEY_free(), X509_free()).
 *
 * Errors: int functions give 0 or -1, pointer functions give NULL, and
 * every failure prints to stderr on the way out.
 */

#ifndef TETRISSH_H
#define TETRISSH_H

#include <stddef.h>
#include <stdint.h>

/* constants */

/* Nonce the client sends first, in bytes. */
#define TETRISSH_NONCE_LEN 32

/* AES-256 session key, in bytes. Same as SESSION_KEY_LEN in common.h. */
#define TETRISSH_SESSION_KEY_LEN 32

/* Frame payload cap. Anything past this is rejected (413 upstream). */
#define TETRISSH_MAX_FRAME_LEN (64 * 1024)

/* Width of the length prefix in front of every encrypted frame. */
#define TETRISSH_FRAME_HDR_LEN 4

/*
 * I/O budgets, in ms.
 *
 * These bound a whole operation (one handshake, or one framed message), not
 * an individual read() or write(). That distinction is the whole point, so it
 * is worth having here and not only down in the .c:
 *
 * SO_RCVTIMEO and SO_SNDTIMEO expire per syscall. A recv() that times out
 * after copying some bytes returns the short count instead of failing, so the
 * reassembly loop just advances and calls recv() again with a fresh, full
 * timeout. Send one byte per timeout period and you never trip any single
 * call's limit; you hold the socket, and with it tetrisd's one event-loop
 * thread, for as long as you like.
 *
 * So each operation runs against one deadline and every syscall in it gets
 * only what is left of that deadline. Making progress cannot buy the peer more
 * time. The budget only shrinks.
 *
 * Clock starts on the FIRST BYTE, not on entry. Waiting for a peer to start
 * talking is not the same as a peer talking too slowly: a client sitting in
 * tetrissh_recv between STATE broadcasts is idle, not stalled, and killing it
 * for that would drop every player any time the server paused. Only the
 * pre-handshake read bounds the idle wait as well, since there a silent peer
 * has already cost us an accepted connection.
 *
 * Handshake gets the bigger budget: four round trips and two RSA ops, against
 * one round trip for a frame.
 */
#define TETRISSH_HANDSHAKE_TIMEOUT_MS 10000
#define TETRISSH_FRAME_TIMEOUT_MS      5000

/* session state */

/*
 * Opaque, one per connection, valid once the handshake succeeds.
 * tetrissh_session_alloc() makes it, tetrissh_session_free() destroys it.
 * Do not poke at the fields from tetrisd or tetrisu; use the calls below.
 */
typedef struct tetrissh_session tetrissh_session_t;

/* lifecycle */

/*
 * Zeroed session object. Free it with tetrissh_session_free() when the
 * connection goes away. NULL if malloc failed.
 */
tetrissh_session_t *tetrissh_session_alloc(void);

/*
 * Frees everything the session owns: AES key, plus any EVP_PKEY or X509
 * still held. NULL is fine.
 */
void tetrissh_session_free(tetrissh_session_t *sess);

/* handshake */

/*
 * Server side, on a socket that has already been accept()ed. Session is
 * ready for encrypted frames afterwards.
 *
 * @param sess        from tetrissh_session_alloc.
 * @param sockfd      connected TCP socket.
 * @param cert_path   server's PEM cert (server.crt).
 * @param key_path    server's PEM private key (server.key).
 *
 * What happens here, in order:
 *   - read the client nonce
 *   - send the PEM cert, length-prefixed
 *   - RSA-PSS sign the nonce with the private key, send the signature
 *   - read the RSA-OAEP wrapped session key, decrypt it with the same key
 *   - stash the session key in sess for send/recv
 *
 * 0 on success, -1 if anything at all went wrong.
 */
int tetrissh_handshake_server(tetrissh_session_t *sess, int sockfd, const char *cert_path, const char *key_path);

/*
 * Client side, on a connected TCP socket. Session is ready for encrypted
 * frames afterwards.
 *
 * @param sess        from tetrissh_session_alloc.
 * @param sockfd      connected TCP socket.
 * @param ca_path     the CA cert (cacsertificate.crt).
 *
 * What happens here, in order:
 *   - make a random nonce and send it
 *   - read the server cert, check it chains to the CA
 *   - read the RSA-PSS signature over our nonce, verify it against the
 *     pubkey inside that cert
 *   - make a 32-byte AES key, RSA-OAEP it under the server pubkey, send it
 *   - stash the session key in sess for send/recv
 *
 * 0 on success, -1 if anything at all went wrong.
 */
int tetrissh_handshake_client(tetrissh_session_t *sess, int sockfd, const char *ca_path);

/* framed send / recv */

/*
 * Encrypts `plaintext` under the session key and puts it on the wire as:
 *
 *   [ 4-byte BE payload length ][ AES ciphertext (IV + ciphertext + HMAC) ]
 *
 * Anything over TETRISSH_MAX_FRAME_LEN is refused before we bother
 * encrypting it.
 *
 * @param sess        established session.
 * @param sockfd      connected socket.
 * @param plaintext   what to send.
 * @param plain_len   how much of it.
 *
 * 0 on success, -1 if encrypt or send failed.
 */
int tetrissh_send(tetrissh_session_t  *sess, int sockfd, const unsigned char *plaintext, size_t plain_len);

/*
 * Pulls one frame off the socket, decrypts it, hands back the plaintext.
 * Caller free()s it.
 *
 * A frame whose declared length is over TETRISSH_MAX_FRAME_LEN is refused
 * before we allocate for it, so a lying length header cannot make us
 * malloc 4 GB.
 *
 * @param sess        established session.
 * @param sockfd      connected socket.
 * @param out_len     gets the plaintext length on success.
 *
 * malloc-ed buffer on success (caller frees), NULL on failure.
 */
unsigned char *tetrissh_recv(tetrissh_session_t *sess, int sockfd, size_t *out_len);

/* teardown */

/*
 * Ends the secure session. Does NOT close the socket, that is the caller's
 * fd. Zeroes the session key and flips the ready flag so later send/recv
 * calls bail out straight away.
 *
 * @param sess  session, established or half-established.
 */
void tetrissh_close(tetrissh_session_t *sess);

/* utility */

/* 1 once the handshake finished and the session key is in place, else 0. */
int tetrissh_is_ready(const tetrissh_session_t *sess);

/*
 * Text for the last thing that went wrong on `sess`. Pointer stays good
 * until the next call on that same session.
 */
const char *tetrissh_strerror(const tetrissh_session_t *sess);

#endif