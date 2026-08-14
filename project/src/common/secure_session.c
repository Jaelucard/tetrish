/*
 * The one secure-session implementation, shared by tetriSH (tetrisd/tetrisu)
 * and mini-gh-tracker. Protocol write-up is in secure_session.h.
 *
 * Crypto is EVP only, no SSL_* and no TLS:
 *   - RAND_bytes            : nonce and session key generation
 *   - EVP_DigestSign...      : RSA-PSS signing   (server signs nonce)
 *   - EVP_DigestVerify...    : RSA-PSS verify    (client verifies nonce sig)
 *   - EVP_PKEY_encrypt...    : RSA-OAEP encrypt  (client wraps session key)
 *   - EVP_PKEY_decrypt...    : RSA-OAEP decrypt  (server unwraps session key)
 *   - EVP_Encrypt.. / Decrypt..  : AES-256-CBC   (framed payload encryption)
 *   - X509_verify_cert       : cert chain check   (client validates server cert)
 *
 * Every message after the handshake goes out as:
 *   [ 4-byte big-endian length ][ IV (16 bytes) ][ AES-256-CBC ciphertext ]
 * The length counts the IV and ciphertext, not itself. Anything over
 * SESSION_MAX_FRAME_LEN (64 KiB of payload) is refused.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <openssl/rand.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/err.h>
#include <openssl/rsa.h>
#include "secure_session.h"

/* context / session structs */

struct session_ctx {
    int   is_server;         /* 1 = server ctx (cert + key), 0 = client ctx (CA) */
    char  cert_path[512];
    char  key_path[512];
    char  ca_path[512];
};

struct session {
    unsigned char session_key[SESSION_KEY_LEN];
    int ready;
    int sockfd;
    char last_error[512];
    int last_recv_was_oversized;  /* 1 only when the last session_read() failed
                                       * because the peer's declared frame length
                                       * blew the cap. Kept separate from a garbled
                                       * frame so the caller can answer 413 rather
                                       * than just hang up. */

    /* Stream-read buffer. session_read() decrypts a whole frame at a time and
     * then hands it out in whatever chunks the caller asks for, so it behaves
     * like a real socket to code above. */
    unsigned char *rbuf;
    size_t rbuf_len;
    size_t rbuf_off;
};

/* error helpers */

static void _ssl_err(session_t *sess, const char *context) {
    unsigned long e = ERR_get_error();
    char ssl_msg[256] = "(no OpenSSL error)";
    if (e) ERR_error_string_n(e, ssl_msg, sizeof(ssl_msg));
    snprintf(sess->last_error, sizeof(sess->last_error), "%s: %s", context, ssl_msg);
    fprintf(stderr, "[secure_session] %s\n", sess->last_error);
}

static void _err(session_t *sess, const char *msg) {
    snprintf(sess->last_error, sizeof(sess->last_error), "%s", msg);
    fprintf(stderr, "[secure_session] %s\n", msg);
}

/* raw socket helpers */

static int send_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len) {
    unsigned char *p = buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, MSG_WAITALL);
        if (n <= 0) return -1;   /* 0 means the peer closed on us */
        p += n; len -= (size_t)n;
    }
    return 0;
}

static int send_length_prefixed(int fd, const unsigned char *data, size_t len) {
    uint8_t hdr[4] = {
        (uint8_t)(len >> 24), (uint8_t)(len >> 16),
        (uint8_t)(len >> 8),  (uint8_t)(len)
    };
    if (send_all(fd, hdr, 4) != 0) return -1;
    if (send_all(fd, data, len) != 0) return -1;
    return 0;
}

static unsigned char *recv_length_prefixed(int fd, size_t *out_len, size_t max_len, int *out_oversized) {
    if (out_oversized) *out_oversized = 0;

    uint8_t hdr[4];
    if (recv_all(fd, hdr, 4) != 0) return NULL;

    size_t len = ((size_t)hdr[0] << 24) | ((size_t)hdr[1] << 16) |
                 ((size_t)hdr[2] << 8)  |  (size_t)hdr[3];
    if (len == 0) return NULL;
    if (len > max_len) {
        if (out_oversized) *out_oversized = 1;
        return NULL;
    }

    unsigned char *buf = malloc(len);
    if (!buf) return NULL;
    if (recv_all(fd, buf, len) != 0) { free(buf); return NULL; }

    *out_len = len;
    return buf;
}

/* crypto helpers */

static EVP_PKEY *load_private_key(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    EVP_PKEY *k = PEM_read_PrivateKey(f, NULL, NULL, NULL);
    fclose(f);
    return k;
}

static X509 *load_cert_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    X509 *c = PEM_read_X509(f, NULL, NULL, NULL);
    fclose(f);
    return c;
}

static unsigned char *cert_to_pem_bytes(X509 *cert, size_t *out_len) {
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) return NULL;
    if (!PEM_write_bio_X509(bio, cert)) { BIO_free(bio); return NULL; }

    BUF_MEM *bptr;
    BIO_get_mem_ptr(bio, &bptr);

    unsigned char *buf = malloc(bptr->length);
    if (!buf) { BIO_free(bio); return NULL; }

    memcpy(buf, bptr->data, bptr->length);
    *out_len = bptr->length;
    BIO_free(bio);
    return buf;
}

static X509 *pem_bytes_to_cert(const unsigned char *data, size_t len) {
    BIO *bio = BIO_new_mem_buf(data, (int)len);
    if (!bio) return NULL;
    X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    BIO_free(bio);
    return cert;
}

static int verify_cert_against_ca(X509 *cert, const char *ca_path) {
    X509 *ca = load_cert_file(ca_path);
    if (!ca) return 0;

    X509_STORE *store = X509_STORE_new();
    X509_STORE_CTX *ctx = X509_STORE_CTX_new();
    X509_STORE_add_cert(store, ca);
    X509_STORE_CTX_init(ctx, store, cert, NULL);

    int ok = X509_verify_cert(ctx);

    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store);
    X509_free(ca);
    return (ok == 1) ? 1 : 0;
}

static int rsa_pss_sign(EVP_PKEY *pkey, const unsigned char *msg, size_t msg_len,
                         unsigned char **sig, size_t *sig_len) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return -1;

    EVP_PKEY_CTX *pctx = NULL;
    if (EVP_DigestSignInit(ctx, &pctx, EVP_sha256(), NULL, pkey) != 1) goto fail;
    if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1) goto fail;
    if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) != 1) goto fail;
    if (EVP_DigestSignUpdate(ctx, msg, msg_len) != 1) goto fail;

    size_t needed = 0;
    if (EVP_DigestSignFinal(ctx, NULL, &needed) != 1) goto fail;
    *sig = malloc(needed);
    if (!*sig) goto fail;
    if (EVP_DigestSignFinal(ctx, *sig, &needed) != 1) { free(*sig); *sig = NULL; goto fail; }

    *sig_len = needed;
    EVP_MD_CTX_free(ctx);
    return 0;
fail:
    EVP_MD_CTX_free(ctx);
    return -1;
}

static int rsa_pss_verify(EVP_PKEY *pkey, const unsigned char *msg, size_t msg_len,
                           const unsigned char *sig, size_t sig_len) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return -1;

    EVP_PKEY_CTX *pctx = NULL;
    int rc = -1;
    if (EVP_DigestVerifyInit(ctx, &pctx, EVP_sha256(), NULL, pkey) != 1) goto done;
    if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1) goto done;
    if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) != 1) goto done;
    if (EVP_DigestVerifyUpdate(ctx, msg, msg_len) != 1) goto done;
    rc = (EVP_DigestVerifyFinal(ctx, sig, sig_len) == 1) ? 1 : 0;
done:
    EVP_MD_CTX_free(ctx);
    return rc;
}

static int rsa_oaep_encrypt(X509 *cert, const unsigned char *plain, size_t plain_len,
                             unsigned char **ct, size_t *ct_len) {
    EVP_PKEY *pubkey = X509_get_pubkey(cert);
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pubkey, NULL);
    *ct = NULL;
    if (!ctx) { EVP_PKEY_free(pubkey); return -1; }

    if (EVP_PKEY_encrypt_init(ctx) != 1) goto fail;
    if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) != 1) goto fail;
    if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) != 1) goto fail;

    size_t needed = 0;
    if (EVP_PKEY_encrypt(ctx, NULL, &needed, plain, plain_len) != 1) goto fail;
    *ct = malloc(needed);
    if (!*ct) goto fail;
    if (EVP_PKEY_encrypt(ctx, *ct, &needed, plain, plain_len) != 1) { free(*ct); *ct = NULL; goto fail; }

    *ct_len = needed;
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pubkey);
    return 0;
fail:
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pubkey);
    return -1;
}

static int rsa_oaep_decrypt(EVP_PKEY *pkey, const unsigned char *ct, size_t ct_len,
                             unsigned char **plain, size_t *plain_len) {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, NULL);
    *plain = NULL;
    if (!ctx) return -1;

    if (EVP_PKEY_decrypt_init(ctx) != 1) goto fail;
    if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) != 1) goto fail;
    if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) != 1) goto fail;

    size_t needed = 0;
    if (EVP_PKEY_decrypt(ctx, NULL, &needed, ct, ct_len) != 1) goto fail;
    *plain = malloc(needed);
    if (!*plain) goto fail;
    if (EVP_PKEY_decrypt(ctx, *plain, &needed, ct, ct_len) != 1) { free(*plain); *plain = NULL; goto fail; }

    *plain_len = needed;
    EVP_PKEY_CTX_free(ctx);
    return 0;
fail:
    EVP_PKEY_CTX_free(ctx);
    return -1;
}

static int aes256_cbc_encrypt(const unsigned char *key, const unsigned char *plain, size_t plain_len,
                               unsigned char **out, size_t *out_len) {
    const int IV_LEN = 16, BLOCK_LEN = 16;
    size_t buf_size = IV_LEN + plain_len + BLOCK_LEN;
    *out = malloc(buf_size);
    if (!*out) return -1;

    unsigned char *iv = *out;
    if (RAND_bytes(iv, IV_LEN) != 1) { free(*out); *out = NULL; return -1; }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(*out); *out = NULL; return -1; }

    unsigned char *ct = *out + IV_LEN;
    int len1 = 0, len2 = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) goto fail;
    if (EVP_EncryptUpdate(ctx, ct, &len1, plain, (int)plain_len) != 1) goto fail;
    if (EVP_EncryptFinal_ex(ctx, ct + len1, &len2) != 1) goto fail;

    *out_len = (size_t)(IV_LEN + len1 + len2);
    EVP_CIPHER_CTX_free(ctx);
    return 0;
fail:
    EVP_CIPHER_CTX_free(ctx);
    free(*out); *out = NULL;
    return -1;
}

static int aes256_cbc_decrypt(const unsigned char *key, const unsigned char *data, size_t data_len,
                               unsigned char **plain, size_t *plain_len) {
    const int IV_LEN = 16;
    if (data_len <= (size_t)IV_LEN) return -1;

    const unsigned char *iv = data;
    const unsigned char *ct = data + IV_LEN;
    size_t ct_len = data_len - IV_LEN;

    *plain = malloc(ct_len);
    if (!*plain) return -1;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(*plain); *plain = NULL; return -1; }

    int len1 = 0, len2 = 0;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) goto fail;
    if (EVP_DecryptUpdate(ctx, *plain, &len1, ct, (int)ct_len) != 1) goto fail;
    if (EVP_DecryptFinal_ex(ctx, *plain + len1, &len2) != 1) goto fail;

    *plain_len = (size_t)(len1 + len2);
    EVP_CIPHER_CTX_free(ctx);
    return 0;
fail:
    EVP_CIPHER_CTX_free(ctx);
    free(*plain); *plain = NULL;
    return -1;
}

/* context lifecycle */

session_ctx_t *session_server_init(const char *cert_path, const char *key_path) {
    if (!cert_path || !key_path) return NULL;

    /* Load both now and throw them away again, just to find out at startup
     * that they parse. Otherwise the first client to connect is the one who
     * discovers the cert path is wrong. */
    X509 *c = load_cert_file(cert_path);
    if (!c) {
        fprintf(stderr, "[secure_session] session_server_init: cannot load cert %s\n", cert_path);
        return NULL;
    }
    X509_free(c);
    EVP_PKEY *k = load_private_key(key_path);
    if (!k) {
        fprintf(stderr, "[secure_session] session_server_init: cannot load key %s\n", key_path);
        return NULL;
    }
    EVP_PKEY_free(k);

    session_ctx_t *ctx = calloc(1, sizeof(session_ctx_t));
    if (!ctx) return NULL;
    ctx->is_server = 1;
    snprintf(ctx->cert_path, sizeof(ctx->cert_path), "%s", cert_path);
    snprintf(ctx->key_path, sizeof(ctx->key_path), "%s", key_path);
    return ctx;
}

void session_server_shutdown(session_ctx_t *ctx) {
    free(ctx);
}

session_ctx_t *session_client_ctx_init(const char *ca_path) {
    if (!ca_path) return NULL;

    X509 *ca = load_cert_file(ca_path);
    if (!ca) {
        fprintf(stderr, "[secure_session] session_client_ctx_init: cannot load CA %s\n", ca_path);
        return NULL;
    }
    X509_free(ca);

    session_ctx_t *ctx = calloc(1, sizeof(session_ctx_t));
    if (!ctx) return NULL;
    ctx->is_server = 0;
    snprintf(ctx->ca_path, sizeof(ctx->ca_path), "%s", ca_path);
    return ctx;
}

void session_client_ctx_free(session_ctx_t *ctx) {
    free(ctx);
}

/* handshake, server side */

session_t *session_accept(session_ctx_t *ctx, int raw_fd) {
    if (!ctx || !ctx->is_server || raw_fd < 0) return NULL;

    session_t *sess = calloc(1, sizeof(session_t));
    if (!sess) return NULL;
    sess->sockfd = raw_fd;
    snprintf(sess->last_error, sizeof(sess->last_error), "not yet initialised");

    /* Step 1: receive the client nonce. */
    unsigned char nonce[SESSION_NONCE_LEN];
    if (recv_all(raw_fd, nonce, SESSION_NONCE_LEN) != 0) {
        _err(sess, "session_accept: failed to receive nonce");
        goto fail;
    }

    /* Step 2: send our certificate, length-prefixed. */
    X509 *cert = load_cert_file(ctx->cert_path);
    if (!cert) { _err(sess, "session_accept: failed to load certificate"); goto fail; }

    size_t pem_len = 0;
    unsigned char *pem = cert_to_pem_bytes(cert, &pem_len);
    X509_free(cert);
    if (!pem) { _err(sess, "session_accept: failed to serialise certificate"); goto fail; }

    if (send_length_prefixed(raw_fd, pem, pem_len) != 0) {
        free(pem);
        _err(sess, "session_accept: failed to send certificate");
        goto fail;
    }
    free(pem);

    /* Step 3: sign the nonce with RSA-PSS; send the signature. */
    EVP_PKEY *privkey = load_private_key(ctx->key_path);
    if (!privkey) { _err(sess, "session_accept: failed to load private key"); goto fail; }

    unsigned char *sig = NULL;
    size_t sig_len = 0;
    if (rsa_pss_sign(privkey, nonce, SESSION_NONCE_LEN, &sig, &sig_len) != 0) {
        EVP_PKEY_free(privkey);
        _ssl_err(sess, "session_accept: RSA-PSS sign failed");
        goto fail;
    }
    EVP_PKEY_free(privkey);

    if (send_length_prefixed(raw_fd, sig, sig_len) != 0) {
        free(sig);
        _err(sess, "session_accept: failed to send signature");
        goto fail;
    }
    free(sig);

    /* Step 4: receive the RSA-OAEP-wrapped session key; decrypt it. */
    size_t enc_key_len = 0;
    unsigned char *enc_key = recv_length_prefixed(raw_fd, &enc_key_len, 4096, NULL);
    if (!enc_key) { _err(sess, "session_accept: failed to receive encrypted session key"); goto fail; }

    privkey = load_private_key(ctx->key_path);
    if (!privkey) { free(enc_key); _err(sess, "session_accept: key reload failed"); goto fail; }

    unsigned char *session_key = NULL;
    size_t session_key_len = 0;
    int rc = rsa_oaep_decrypt(privkey, enc_key, enc_key_len, &session_key, &session_key_len);
    EVP_PKEY_free(privkey);
    free(enc_key);

    if (rc != 0 || session_key_len != SESSION_KEY_LEN) {
        free(session_key);
        _ssl_err(sess, "session_accept: RSA-OAEP decrypt failed");
        goto fail;
    }

    /* Step 5: store the session key; mark ready. */
    memcpy(sess->session_key, session_key, SESSION_KEY_LEN);
    memset(session_key, 0, session_key_len);
    free(session_key);

    sess->ready = 1;
    snprintf(sess->last_error, sizeof(sess->last_error), "ok");
    return sess;

fail:
    free(sess);
    return NULL;
}

/* handshake, client side */

session_t *session_connect(session_ctx_t *ctx, int raw_fd) {
    if (!ctx || ctx->is_server || raw_fd < 0) return NULL;

    session_t *sess = calloc(1, sizeof(session_t));
    if (!sess) return NULL;
    sess->sockfd = raw_fd;
    snprintf(sess->last_error, sizeof(sess->last_error), "not yet initialised");

    /* Step 1: generate a fresh nonce and send it. */
    unsigned char nonce[SESSION_NONCE_LEN];
    if (RAND_bytes(nonce, SESSION_NONCE_LEN) != 1) {
        _ssl_err(sess, "session_connect: RAND_bytes nonce failed");
        goto fail;
    }
    if (send_all(raw_fd, nonce, SESSION_NONCE_LEN) != 0) {
        _err(sess, "session_connect: failed to send nonce");
        goto fail;
    }

    /* Step 2: receive server certificate; parse it. */
    size_t cert_pem_len = 0;
    unsigned char *cert_pem = recv_length_prefixed(raw_fd, &cert_pem_len, 64 * 1024, NULL);
    if (!cert_pem) { _err(sess, "session_connect: failed to receive certificate"); goto fail; }

    X509 *server_cert = pem_bytes_to_cert(cert_pem, cert_pem_len);
    free(cert_pem);
    if (!server_cert) { _err(sess, "session_connect: failed to parse server certificate"); goto fail; }

    /* Step 3: verify the certificate against our CA. */
    if (!verify_cert_against_ca(server_cert, ctx->ca_path)) {
        X509_free(server_cert);
        _err(sess, "session_connect: server certificate failed CA verification");
        goto fail;
    }

    /* Step 4: receive RSA-PSS signature; verify it against the nonce. */
    size_t sig_len = 0;
    unsigned char *sig = recv_length_prefixed(raw_fd, &sig_len, 4096, NULL);
    if (!sig) { X509_free(server_cert); _err(sess, "session_connect: failed to receive signature"); goto fail; }

    EVP_PKEY *server_pubkey = X509_get_pubkey(server_cert);
    int verify_rc = rsa_pss_verify(server_pubkey, nonce, SESSION_NONCE_LEN, sig, sig_len);
    EVP_PKEY_free(server_pubkey);
    free(sig);

    if (verify_rc != 1) {
        X509_free(server_cert);
        _ssl_err(sess, "session_connect: nonce signature verification failed");
        goto fail;
    }

    /* Step 5: generate a fresh 32-byte AES session key. */
    unsigned char session_key[SESSION_KEY_LEN];
    if (RAND_bytes(session_key, SESSION_KEY_LEN) != 1) {
        X509_free(server_cert);
        _ssl_err(sess, "session_connect: RAND_bytes session key failed");
        goto fail;
    }

    /* Step 6: RSA-OAEP encrypt the session key; send it. */
    unsigned char *enc_key = NULL;
    size_t enc_key_len = 0;
    if (rsa_oaep_encrypt(server_cert, session_key, SESSION_KEY_LEN, &enc_key, &enc_key_len) != 0) {
        X509_free(server_cert);
        _ssl_err(sess, "session_connect: RSA-OAEP encrypt failed");
        goto fail;
    }
    X509_free(server_cert);

    if (send_length_prefixed(raw_fd, enc_key, enc_key_len) != 0) {
        free(enc_key);
        _err(sess, "session_connect: failed to send encrypted session key");
        goto fail;
    }
    free(enc_key);

    /* Step 7: store session key; mark ready. */
    memcpy(sess->session_key, session_key, SESSION_KEY_LEN);
    memset(session_key, 0, sizeof(session_key));

    sess->ready = 1;
    snprintf(sess->last_error, sizeof(sess->last_error), "ok");
    return sess;

fail:
    free(sess);
    return NULL;
}

/* framed I/O, one frame per call */

static int session_send_frame(session_t *sess, const unsigned char *plaintext, size_t plain_len) {
    if (plain_len > SESSION_MAX_FRAME_LEN) {
        _err(sess, "session_write: plaintext exceeds max frame length");
        return -1;
    }

    unsigned char *frame = NULL;
    size_t frame_len = 0;
    if (aes256_cbc_encrypt(sess->session_key, plaintext, plain_len, &frame, &frame_len) != 0) {
        _ssl_err(sess, "session_write: AES-256-CBC encrypt failed");
        return -1;
    }

    int rc = send_length_prefixed(sess->sockfd, frame, frame_len);
    free(frame);
    if (rc != 0) { _err(sess, "session_write: send failed"); return -1; }
    return 0;
}

static ssize_t session_recv_frame(session_t *sess) {
    sess->last_recv_was_oversized = 0;

    int oversized = 0;
    size_t frame_len = 0;
    unsigned char *frame = recv_length_prefixed(sess->sockfd, &frame_len, SESSION_MAX_FRAME_LEN + 16 + 16, &oversized);
    if (!frame) {
        if (oversized) {
            sess->last_recv_was_oversized = 1;
            _err(sess, "session_read: frame exceeds 64 KiB cap");
        } else {
            _err(sess, "session_read: failed to receive frame");
        }
        return -1;
    }

    unsigned char *plain = NULL;
    size_t plain_len = 0;
    if (aes256_cbc_decrypt(sess->session_key, frame, frame_len, &plain, &plain_len) != 0) {
        free(frame);
        _ssl_err(sess, "session_read: AES-256-CBC decrypt failed");
        return -1;
    }
    free(frame);

    free(sess->rbuf);
    sess->rbuf = plain;
    sess->rbuf_len = plain_len;
    sess->rbuf_off = 0;
    return (ssize_t)plain_len;
}

/* public stream I/O */

ssize_t session_read(session_t *s, void *buf, size_t len) {
    if (!s || !session_is_ready(s) || !buf || len == 0) {
        if (s) _err(s, "session_read: invalid arguments or session not ready");
        return -1;
    }

    if (s->rbuf_off >= s->rbuf_len) {
        ssize_t rc = session_recv_frame(s);
        if (rc < 0) return -1;
        if (rc == 0) return 0; /* empty frame, treat as end of frame; caller retries */
    }

    size_t avail = s->rbuf_len - s->rbuf_off;
    size_t take = (len < avail) ? len : avail;
    memcpy(buf, s->rbuf + s->rbuf_off, take);
    s->rbuf_off += take;
    return (ssize_t)take;
}

ssize_t session_write(session_t *s, const void *buf, size_t len) {
    if (!s || !session_is_ready(s) || !buf) {
        if (s) _err(s, "session_write: invalid arguments or session not ready");
        return -1;
    }
    if (len == 0) return 0;

    /* One HTTTP message is one frame. No splitting across frames, so anything
     * past the limit gets refused right here and the caller answers 413. Same
     * policy as libtetrissh's own send/recv. */
    if (session_send_frame(s, buf, len) != 0) return -1;
    return (ssize_t)len;
}

/* teardown / utility */

void session_close(session_t *s) {
    if (!s) return;
    memset(s->session_key, 0, sizeof(s->session_key));
    free(s->rbuf);
    s->rbuf = NULL;
    s->rbuf_len = s->rbuf_off = 0;
    s->ready = 0;
    /* fd is not ours; whoever opened it closes it. */
    free(s);
}

int session_is_ready(const session_t *s) {
    return (s && s->ready) ? 1 : 0;
}

int session_last_recv_was_oversized(const session_t *s) {
    return (s && s->last_recv_was_oversized) ? 1 : 0;
}

const char *session_strerror(const session_t *s) {
    return s ? s->last_error : "null session";
}
