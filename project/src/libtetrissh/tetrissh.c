#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tetrissh.h"

tetrissh_session_t *tetrissh_session_alloc(void) {
    tetrissh_session_t *sess = calloc(1, sizeof(tetrissh_session_t));
    if (!sess) {
        fprintf(stderr, "[tetrissh] tetrissh_session_alloc: out of memory\n");
        return NULL;
    }
    sess->inner = NULL;
    snprintf(sess->last_error, sizeof(sess->last_error), "not yet initialised");
    return sess;
}

void tetrissh_session_free(tetrissh_session_t *sess) {
    if (!sess) {
        return;
    }
    if (sess->inner) {
        session_close(sess->inner); // zeroes the AES key
    }
    free(sess);
}

int tetrissh_handshake_server(tetrissh_session_t *sess, int sockfd, const char *cert_path, const char *key_path) {
    if (!sess || sockfd < 0 || !cert_path || !key_path) {
        if (sess) {
            snprintf(sess->last_error, sizeof(sess->last_error), "handshake_server: invalid arguments");
        }
        return -1;
    }

    // Create a server context with session_server_init().
    session_ctx_t *ctx = session_server_init(cert_path, key_path);
    if (!ctx) {
        snprintf(sess->last_error, sizeof(sess->last_error), "handshake_server: failed to load cert/key from %s / %s", cert_path, key_path);
        return -1;
    }

    // Perform the complete server-side handshake with session_accept().
    session_t *inner = session_accept(ctx, sockfd);

    // Destroy the temporary context with session_server_shutdown().
    session_server_shutdown(ctx);

    if (!inner) {
        snprintf(sess->last_error, sizeof(sess->last_error), "handshake_server: handshake failed (see stderr for detail)");
        return -1;
    }

    // Store the secure session.
    sess->inner = inner;
    snprintf(sess->last_error, sizeof(sess->last_error), "ok");
    return 0;
}

int tetrissh_handshake_client(tetrissh_session_t *sess, int sockfd, const char *ca_path) {
    if (!sess || sockfd < 0 || !ca_path) {
        if (sess) {
            snprintf(sess->last_error, sizeof(sess->last_error), "handshake_client: invalid arguments");
        }
        return -1;
    }

    // Create a client context with session_client_ctx_init().
    session_ctx_t *ctx = session_client_ctx_init(ca_path);
    if (!ctx) {
        snprintf(sess->last_error, sizeof(sess->last_error), "handshake_client: failed to load CA from %s", ca_path);
        return -1;
    }

    // Perform the complete client-side handshake with session_connect().
    session_t *inner = session_connect(ctx, sockfd);

    // Free the temporary context with session_client_ctx_free().
    session_client_ctx_free(ctx);

    if (!inner) {
        snprintf(sess->last_error, sizeof(sess->last_error), "handshake_client: handshake failed (see stderr for detail)");
        return -1;
    }

    // Store the secure session.
    sess->inner = inner;
    snprintf(sess->last_error, sizeof(sess->last_error), "ok");
    return 0;
}

int tetrissh_send(tetrissh_session_t *sess, int sockfd, const unsigned char *plaintext, size_t plain_len) {
    // Parameter not used in this function, but still implemented for API compatibility reasons.
    (void)sockfd;

    // Reject send request if sess is NULL or the secure handshake is not completed successfully (sess->inner must exist, secure session must be established, and AES session ket must be negotiated.)
    if (!sess || !tetrissh_is_ready(sess)) {
        if (sess) {
            snprintf(sess->last_error, sizeof(sess->last_error), "tetrissh_send: session not ready");
        }
        return -1;
    }

    // Reject send request if plaintext is NULL or plaintext length is zero because sending an empty message is considered an error.
    if (!plaintext || plain_len == 0) {
        snprintf(sess->last_error, sizeof(sess->last_error), "tetrissh_send: empty plaintext");
        return -1;
    }

    // Reject send request if plaintext length exceeds maximum permitted payload size to prevent oversized frames that could exceed protocol limits, waste memory, or be used for denial-of-service attacks.
    if (plain_len > TETRISSH_MAX_FRAME_LEN) {
        snprintf(sess->last_error, sizeof(sess->last_error), "tetrissh_send: plaintext exceeds max frame length");
        return -1;
    }

    // Perform AES encryption, frame construction, and writing of the frame to the socket.
    ssize_t n = session_write(sess->inner, plaintext, plain_len);
    if (n <= 0) {
        snprintf(sess->last_error, sizeof(sess->last_error), "tetrissh_send: %s", session_strerror(sess->inner));
        return -1;
    }
    return 0;
}

unsigned char *tetrissh_recv(tetrissh_session_t *sess, int sockfd, size_t *out_len) {
    (void)sockfd;
    if (out_len) {
        *out_len = 0;
    }
    if (!sess || !tetrissh_is_ready(sess)) {
        if (sess) {
            snprintf(sess->last_error, sizeof(sess->last_error), "tetrissh_recv: session not ready");
        }
        return NULL;
    }

    // Allocate a receive buffer to hold the maximum possible plaintext frame to contain the decrypted plaintext.
    unsigned char *buf = malloc(TETRISSH_MAX_FRAME_LEN);
    if (!buf) {
        snprintf(sess->last_error, sizeof(sess->last_error), "tetrissh_recv: out of memory");
        return NULL;
    }

    // Read and decrypt one frame using session_read().
    ssize_t n = session_read(sess->inner, buf, TETRISSH_MAX_FRAME_LEN);
    if (n <= 0) {
        free(buf);
        snprintf(sess->last_error, sizeof(sess->last_error), "tetrissh_recv: %s", session_strerror(sess->inner));
        return NULL;
    }

    // Store the number of decrypted bytes
    if (out_len) {
        *out_len = (size_t)n;
    }
    return buf; // This buffer must be freed eventually.
}

void tetrissh_close(tetrissh_session_t *sess) {
    if (!sess) {
        return;
    }
    if (sess->inner) {
        session_close(sess->inner);
        sess->inner = NULL;
    }
    snprintf(sess->last_error, sizeof(sess->last_error), "session closed");
}

int tetrissh_is_ready(const tetrissh_session_t *sess) {
    if (sess && sess->inner && session_is_ready(sess->inner)) {
        return 1;
    }
    return 0;
}

const char *tetrissh_strerror(const tetrissh_session_t *sess) {
    if (!sess) {
        return "null session";
    }
    return sess->last_error;
}

int tetrissh_last_recv_was_oversized(const tetrissh_session_t *sess) {
    if (sess && sess->inner) {
        return session_last_recv_was_oversized(sess->inner);
    } else {
        return 0;
    }
}