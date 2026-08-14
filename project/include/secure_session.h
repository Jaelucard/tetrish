#ifndef CORESTACK_SECURE_SESSION_H
#define CORESTACK_SECURE_SESSION_H

#include <sys/types.h>
#include <stddef.h>

#define SESSION_NONCE_LEN 32
#define SESSION_KEY_LEN 32
#define SESSION_MAX_FRAME_LEN (64 * 1024)

typedef struct session_ctx session_ctx_t;
typedef struct session session_t;

session_ctx_t *session_server_init(const char *cert_path, const char *key_path);
void session_server_shutdown(session_ctx_t *ctx);

session_ctx_t *session_client_ctx_init(const char *ca_path);
void session_client_ctx_free(session_ctx_t *ctx);

session_t *session_accept(session_ctx_t *ctx, int raw_fd);
session_t *session_connect(session_ctx_t *ctx, int raw_fd);

ssize_t session_read(session_t *s, void *buf, size_t len);
ssize_t session_write(session_t *s, const void *buf, size_t len);

void session_close(session_t *s);
int session_is_ready(const session_t *s);
int session_last_recv_was_oversized(const session_t *s);
const char *session_strerror(const session_t *s);

#endif

// One contract, every corestack consumer:
// >> 50.005 side : tetrisd (server) and tetrisu (client)
// >> 50.003 side : mini-gh-tracker (server-only)

// Protocol, all seven steps:
// 1. Client connects, sends a fresh nonce.
// 2. Server sends its X.509 certificate.
// 3. Client checks that certificate against the bundled CA.
// 4. Server signs the client nonce with its private key (RSA-PSS).
// 5. Client verifies that signature with the public key from the cert.
// 6. Client generates a 32-byte AES-256 session key, RSA-OAEP encrypts it with the server's public key, sends it.
// 7. Every frame after that is [4-byte BE length][AES-256-CBC ciphertext], carrying one HTTTP message. Frames stop at 64 KiB.

// Crypto comes from common.c and nowhere else (OpenSSL EVP).
// >> Never OpenSSL's SSL_* (TLS) API = HTTPS

// Statically linked into every binary that needs it.