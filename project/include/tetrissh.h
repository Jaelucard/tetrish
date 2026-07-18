#ifndef TETRISSH_H
#define TETRISSH_H

#include <stddef.h>
#include <stdint.h>
#include "secure_session.h"

#define TETRISSH_NONCE_LEN SESSION_NONCE_LEN             // Random nonce length (bytes) sent by the client at the start of the handshake.
#define TETRISSH_SESSION_KEY_LEN  SESSION_KEY_LEN        //AES-256 session key length (bytes).
#define TETRISSH_MAX_FRAME_LEN SESSION_MAX_FRAME_LEN     // Maximum encrypted frame payload (bytes). Frames exceeding this are rejected.
#define TETRISSH_FRAME_HDR_LEN 4                         // Wire: length-prefix field width (bytes) for each encrypted frame.

typedef struct tetrissh_session {
    session_t *inner;
    char last_error[512];
} tetrissh_session_t;

tetrissh_session_t *tetrissh_session_alloc(void);
void tetrissh_session_free(tetrissh_session_t *sess);

int tetrissh_handshake_server(tetrissh_session_t *sess, int sockfd, const char *cert_path, const char *key_path);
int tetrissh_handshake_client(tetrissh_session_t *sess, int sockfd, const char *ca_path);

int tetrissh_send(tetrissh_session_t *sess, int sockfd, const unsigned char *plaintext, size_t plain_len);
unsigned char *tetrissh_recv(tetrissh_session_t *sess, int sockfd, size_t *out_len);

void tetrissh_close(tetrissh_session_t *sess);

int tetrissh_is_ready(const tetrissh_session_t *sess);
const char *tetrissh_strerror(const tetrissh_session_t *sess);

#endif