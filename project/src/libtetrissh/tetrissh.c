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
    if (!sess) return;
    if (sess->inner) session_close(sess->inner);
    free(sess);
}

/* ─────────────────────────── handshake ───────────────────────────────────── */

int tetrissh_handshake_server(tetrissh_session_t *sess, int sockfd, const char *cert_path, const char *key_path) {
    if (!sess || sockfd < 0 || !cert_path || !key_path) {
        if (sess) snprintf(sess->last_error, sizeof(sess->last_error), "handshake_server: invalid arguments");
        return -1;
    }

    /* corestack's context is a lightweight, short-lived holder for the
     * cert/key paths — safe to build and tear down per connection. tetrisd
     * may instead build one ctx once at startup and reuse it across
     * connections for slightly less I/O per handshake; either is fine
     * against corestack's contract. */
    session_ctx_t *ctx = session_server_init(cert_path, key_path);
    if (!ctx) {
        snprintf(sess->last_error, sizeof(sess->last_error), "handshake_server: failed to load cert/key from %s / %s", cert_path, key_path);
        return -1;
    }

    session_t *inner = session_accept(ctx, sockfd);
    session_server_shutdown(ctx);

    if (!inner) {
        snprintf(sess->last_error, sizeof(sess->last_error),
                 "handshake_server: handshake failed (see stderr for detail)");
        return -1;
    }

    sess->inner = inner;
    snprintf(sess->last_error, sizeof(sess->last_error), "ok");
    return 0;
}

int tetrissh_handshake_client(tetrissh_session_t *sess, int sockfd, const char *ca_path) {
    if (!sess || sockfd < 0 || !ca_path) {
        if (sess) snprintf(sess->last_error, sizeof(sess->last_error), "handshake_client: invalid arguments");
        return -1;
    }

    session_ctx_t *ctx = session_client_ctx_init(ca_path);
    if (!ctx) {
        snprintf(sess->last_error, sizeof(sess->last_error),
                 "handshake_client: failed to load CA from %s", ca_path);
        return -1;
    }

    session_t *inner = session_connect(ctx, sockfd);
    session_client_ctx_free(ctx);

    if (!inner) {
        snprintf(sess->last_error, sizeof(sess->last_error),
                 "handshake_client: handshake failed (see stderr for detail)");
        return -1;
    }

    sess->inner = inner;
    snprintf(sess->last_error, sizeof(sess->last_error), "ok");
    return 0;
}

/* ─────────────────────────── framed send / recv ──────────────────────────── */

int tetrissh_send(tetrissh_session_t *sess, int sockfd, const unsigned char *plaintext, size_t plain_len) {
    (void)sockfd;
    if (!sess || !tetrissh_is_ready(sess)) {
        if (sess) snprintf(sess->last_error, sizeof(sess->last_error), "tetrissh_send: session not ready");
        return -1;
    }
    if (!plaintext || plain_len == 0) {
        snprintf(sess->last_error, sizeof(sess->last_error), "tetrissh_send: empty plaintext");
        return -1;
    }
    if (plain_len > TETRISSH_MAX_FRAME_LEN) {
        snprintf(sess->last_error, sizeof(sess->last_error), "tetrissh_send: plaintext exceeds max frame length");
        return -1;
    }

    ssize_t n = session_write(sess->inner, plaintext, plain_len);
    if (n <= 0) {
        snprintf(sess->last_error, sizeof(sess->last_error), "tetrissh_send: %s", session_strerror(sess->inner));
        return -1;
    }
    return 0;
}

unsigned char *tetrissh_recv(tetrissh_session_t *sess, int sockfd, size_t *out_len) {
    (void)sockfd;
    if (out_len) *out_len = 0;

    if (!sess || !tetrissh_is_ready(sess)) {
        if (sess) snprintf(sess->last_error, sizeof(sess->last_error), "tetrissh_recv: session not ready");
        return NULL;
    }

    /* One tetrissh_recv() call == one whole decrypted frame. As long as
     * every previous call fully drained its frame (true here: callers
     * always take the full returned buffer), session_read() below starts
     * at a fresh frame boundary, so a single call with a buffer sized to
     * the max frame returns exactly that frame's plaintext. */
    unsigned char *buf = malloc(TETRISSH_MAX_FRAME_LEN);
    if (!buf) {
        snprintf(sess->last_error, sizeof(sess->last_error), "tetrissh_recv: out of memory");
        return NULL;
    }

    ssize_t n = session_read(sess->inner, buf, TETRISSH_MAX_FRAME_LEN);
    if (n <= 0) {
        free(buf);
        snprintf(sess->last_error, sizeof(sess->last_error), "tetrissh_recv: %s", session_strerror(sess->inner));
        return NULL;
    }

    if (out_len) *out_len = (size_t)n;
    return buf;   /* caller must free() */
}

/* ─────────────────────────── teardown ────────────────────────────────────── */

void tetrissh_close(tetrissh_session_t *sess) {
    if (!sess) return;
    if (sess->inner) {
        session_close(sess->inner);
        sess->inner = NULL;
    }
    snprintf(sess->last_error, sizeof(sess->last_error), "session closed");
}

/* ─────────────────────────── utility ─────────────────────────────────────── */

int tetrissh_is_ready(const tetrissh_session_t *sess) {
    return (sess && sess->inner && session_is_ready(sess->inner)) ? 1 : 0;
}

const char *tetrissh_strerror(const tetrissh_session_t *sess) {
    if (!sess) return "null session";
    return sess->last_error;
}

int tetrissh_last_recv_was_oversized(const tetrissh_session_t *sess) {
    return (sess && sess->inner) ? session_last_recv_was_oversized(sess->inner) : 0;
}
