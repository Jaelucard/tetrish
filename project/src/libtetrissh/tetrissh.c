/**
 * tetrissh.c  —  libtetrissh: Secure Session Library for tetriSH
 *
 * Crypto used (all via OpenSSL EVP, no deprecated low-level APIs):
 *   - RAND_bytes           : nonce and session key generation
 *   - EVP_DigestSign*      : RSA-PSS signing   (server signs nonce)
 *   - EVP_DigestVerify*    : RSA-PSS verify    (client verifies nonce sig)
 *   - EVP_PKEY_encrypt*    : RSA-OAEP encrypt  (client wraps session key)
 *   - EVP_PKEY_decrypt*    : RSA-OAEP decrypt  (server unwraps session key)
 *   - EVP_Encrypt* /Decrypt*: AES-256-CBC        (framed payload encryption)
 *   - X509_verify_cert     : cert chain check   (client validates server cert)
 *
 * Wire format for every message after the handshake:
 *   [ 4-byte big-endian length ][ IV (16 bytes) ][ AES-256-CBC ciphertext ]
 *
 * The length field counts only the IV + ciphertext, not itself.
 *
 * Build:
 *   gcc -c tetrissh.c -I../../include $(pkg-config --cflags openssl) -o tetrissh.o
 *   ar rcs ../../lib/libtetrissh.a tetrissh.o
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* POSIX socket I/O */
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

/* OpenSSL */
#include <openssl/rand.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/err.h>
#include <openssl/rsa.h>

#include "tetrissh.h"

/* macOS has no MSG_NOSIGNAL; the equivalent SIGPIPE suppression is done
 * per-socket with SO_NOSIGPIPE inside send_all() instead. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* ─────────────────────────── session struct ─────────────────────────────── */

struct tetrissh_session {
    unsigned char session_key[TETRISSH_SESSION_KEY_LEN];
    int ready;
    char last_error[512];
    int sockfd;
};

/* ─────────────────────────── internal helpers ───────────────────────────── */

/* Store a human-readable OpenSSL error string into sess->last_error. */
static void _ssl_err(tetrissh_session_t *sess, const char *context) {
    unsigned long e = ERR_get_error();
    char ssl_msg[256] = "(no OpenSSL error)";
    if (e) {
        ERR_error_string_n(e, ssl_msg, sizeof(ssl_msg));
    }
    snprintf(sess->last_error, sizeof(sess->last_error), "%s: %s", context, ssl_msg);
    fprintf(stderr, "[tetrissh] %s\n", sess->last_error);
}

/* Store a plain string into sess->last_error. */
static void _err(tetrissh_session_t *sess, const char *msg) {
    snprintf(sess->last_error, sizeof(sess->last_error), "%s", msg);
    fprintf(stderr, "[tetrissh] %s\n", msg);
}

/*
 * send_all / recv_all
 *
 * TCP is a stream protocol — a single send() or recv() call may transfer
 * fewer bytes than requested.  These wrappers loop until exactly `len`
 * bytes have been transferred or an unrecoverable error occurs.
 *
 * Returns 0 on success, -1 on error.
 */
static int send_all(int fd, const void *buf, size_t len)
{
#ifdef __APPLE__
    /* Darwin's replacement for MSG_NOSIGNAL: a write to a dead peer returns
     * EPIPE instead of raising SIGPIPE. Setting it per call is idempotent
     * and costs one cheap syscall next to the encryption work per frame. */
    int nosigpipe = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof nosigpipe);
#endif
    const unsigned char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len) {
    unsigned char *p = buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, MSG_WAITALL);

        /* 0 = peer closed cleanly */
        if (n <= 0) {
            return -1;
        }
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

/*
 * send_length_prefixed / recv_length_prefixed
 *
 * Every variable-length blob on the wire is preceded by a 4-byte
 * big-endian length field.  These helpers encode/decode that field
 * and transfer the blob in one logical operation.
 *
 * recv_length_prefixed allocates a buffer for the caller; the caller
 * must free() it.  Returns NULL on any error.
 */
static int send_length_prefixed(int fd, const unsigned char *data, size_t len) {
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(len >> 24);
    hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >>  8);
    hdr[3] = (uint8_t)(len);
    if (send_all(fd, hdr, 4) != 0) {
        return -1;
    }
    if (send_all(fd, data, len) != 0) {
        return -1;
    }
    return 0;
}

static unsigned char *recv_length_prefixed(int fd, size_t *out_len, size_t  max_len) {
    uint8_t hdr[4];
    if (recv_all(fd, hdr, 4) != 0) {
        return NULL;
    }

    size_t len = ((size_t)hdr[0] << 24) | ((size_t)hdr[1] << 16) | ((size_t)hdr[2] <<  8) | (size_t)hdr[3];

    if (len == 0 || len > max_len) {
        return NULL;
    }

    unsigned char *buf = malloc(len);
    if (!buf) {
        return NULL;
    }

    if (recv_all(fd, buf, len) != 0) { 
        free(buf); 
        return NULL;
    }

    *out_len = len;
    return buf;
}

/* ─────────────────────────── crypto helpers ─────────────────────────────── */

/*
 * load_private_key
 * Reads a PEM private key from disk.
 * Caller must EVP_PKEY_free() the returned pointer.
 */
static EVP_PKEY *load_private_key(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    EVP_PKEY *k = PEM_read_PrivateKey(f, NULL, NULL, NULL);
    fclose(f);
    return k;
}

/*
 * load_cert_file
 * Reads a PEM X.509 certificate from disk.
 * Caller must X509_free() the returned pointer.
 */
static X509 *load_cert_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    X509 *c = PEM_read_X509(f, NULL, NULL, NULL);
    fclose(f);
    return c;
}

/*
 * cert_to_pem_bytes
 * Serialises an X509* into a heap-allocated PEM byte buffer.
 * Caller must free() the returned pointer.
 */
static unsigned char *cert_to_pem_bytes(X509 *cert, size_t *out_len) {
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) {
        return NULL;
    }
    if (!PEM_write_bio_X509(bio, cert)) { 
        BIO_free(bio); 
        return NULL;
    }

    BUF_MEM *bptr;
    BIO_get_mem_ptr(bio, &bptr);

    unsigned char *buf = malloc(bptr->length);
    if (!buf) { 
        BIO_free(bio); 
        return NULL;
    }

    memcpy(buf, bptr->data, bptr->length);
    *out_len = bptr->length;
    BIO_free(bio);
    return buf;
}

/*
 * pem_bytes_to_cert
 * Parses a PEM byte buffer (not NUL-terminated) into an X509*.
 * Caller must X509_free() the returned pointer.
 */
static X509 *pem_bytes_to_cert(const unsigned char *data, size_t len) {
    BIO *bio = BIO_new_mem_buf(data, (int)len);
    if (!bio) {
        return NULL;
    }
    X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    BIO_free(bio);
    return cert;
}

/*
 * verify_cert_against_ca
 * Returns 1 if `cert` chains up to the CA in `ca_path`, 0 otherwise.
 */
static int verify_cert_against_ca(X509 *cert, const char *ca_path) {
    X509 *ca = load_cert_file(ca_path);
    if (!ca) {
        return 0;
    }

    X509_STORE *store = X509_STORE_new();
    X509_STORE_CTX *ctx = X509_STORE_CTX_new();
    X509_STORE_add_cert(store, ca);
    X509_STORE_CTX_init(ctx, store, cert, NULL);

    int ok = X509_verify_cert(ctx);   /* 1 = verified, <=0 = failed */

    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store);
    X509_free(ca);
    return (ok == 1) ? 1 : 0;
}

/*
 * rsa_pss_sign
 *
 * Signs `msg_len` bytes at `msg` with `pkey` using RSA-PSS + SHA-256.
 * Writes the signature into a freshly malloc-ed buffer.
 * Caller must free() *sig.
 * Returns 0 on success, -1 on failure.
 */
static int rsa_pss_sign(EVP_PKEY *pkey, const unsigned char *msg, size_t msg_len, unsigned char **sig, size_t *sig_len) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return -1;
    }

    EVP_PKEY_CTX *pctx = NULL;
    if (EVP_DigestSignInit(ctx, &pctx, EVP_sha256(), NULL, pkey) != 1) {
        goto fail;
    }
    if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1) {
        goto fail;
    }
    if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) != 1) {
        goto fail;
    }
    if (EVP_DigestSignUpdate(ctx, msg, msg_len) != 1) {
        goto fail;
    }

    /* First call: determine required buffer size. */
    size_t needed = 0;
    if (EVP_DigestSignFinal(ctx, NULL, &needed) != 1) {
        goto fail;
    }

    *sig = malloc(needed);
    if (!*sig) {
        goto fail;
    }

    /* Second call: actually sign. */
    if (EVP_DigestSignFinal(ctx, *sig, &needed) != 1) {
        free(*sig); *sig = NULL; 
        goto fail;
    }

    *sig_len = needed;
    EVP_MD_CTX_free(ctx);
    return 0;

fail:
    EVP_MD_CTX_free(ctx);
    return -1;
}

/*
 * rsa_pss_verify
 *
 * Verifies an RSA-PSS + SHA-256 signature.
 * Returns 1 if valid, 0 if invalid, -1 on error.
 */
static int rsa_pss_verify(EVP_PKEY *pkey, const unsigned char *msg, size_t msg_len, const unsigned char *sig, size_t sig_len) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return -1;
    }

    EVP_PKEY_CTX *pctx = NULL;
    int rc = -1;

    if (EVP_DigestVerifyInit(ctx, &pctx, EVP_sha256(), NULL, pkey) != 1) {
        goto done;
    }
    if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1) {
        goto done;
    }
    if (EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) != 1) {
        goto done;
    }
    if (EVP_DigestVerifyUpdate(ctx, msg, msg_len) != 1) {
        goto done;
    }

    rc = (EVP_DigestVerifyFinal(ctx, sig, sig_len) == 1) ? 1 : 0;

done:
    EVP_MD_CTX_free(ctx);
    return rc;
}

/*
 * rsa_oaep_encrypt
 *
 * RSA-OAEP (SHA-256) encrypts `plain` with the public key inside `cert`.
 * Writes ciphertext into a freshly malloc-ed buffer.
 * Caller must free() *ct.
 * Returns 0 on success, -1 on failure.
 */
static int rsa_oaep_encrypt(X509 *cert, const unsigned char *plain, size_t plain_len, unsigned char **ct, size_t *ct_len) {
    EVP_PKEY *pubkey = X509_get_pubkey(cert);
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pubkey, NULL);
    *ct = NULL;

    if (!ctx) { 
        EVP_PKEY_free(pubkey); 
        return -1; 
    }
    if (EVP_PKEY_encrypt_init(ctx) != 1) {
        goto fail;
    }
    if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) != 1) {
        goto fail;
    }
    if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) != 1) {
        goto fail;
    }

    /* Size query */
    size_t needed = 0;
    if (EVP_PKEY_encrypt(ctx, NULL, &needed, plain, plain_len) != 1) {
        goto fail;
    }

    *ct = malloc(needed);
    if (!*ct) {
        goto fail;
    }

    if (EVP_PKEY_encrypt(ctx, *ct, &needed, plain, plain_len) != 1) {
        free(*ct); *ct = NULL; 
        goto fail;
    }

    *ct_len = needed;
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pubkey);
    return 0;

fail:
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pubkey);
    return -1;
}

/*
 * rsa_oaep_decrypt
 *
 * RSA-OAEP (SHA-256) decrypts `ct` with `pkey`.
 * Writes plaintext into a freshly malloc-ed buffer.
 * Caller must free() *plain.
 * Returns 0 on success, -1 on failure.
 */
static int rsa_oaep_decrypt(EVP_PKEY *pkey, const unsigned char *ct, size_t ct_len, unsigned char **plain, size_t *plain_len) {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, NULL);
    *plain = NULL;

    if (!ctx) {
        return -1;
    }
    if (EVP_PKEY_decrypt_init(ctx) != 1) {
        goto fail;
    }
    if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) != 1) {
        goto fail;
    }
    if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) != 1) {
        goto fail;
    }

    size_t needed = 0;
    if (EVP_PKEY_decrypt(ctx, NULL, &needed, ct, ct_len) != 1) {
        goto fail;
    }

    *plain = malloc(needed);
    if (!*plain) {
        goto fail;
    }

    if (EVP_PKEY_decrypt(ctx, *plain, &needed, ct, ct_len) != 1) {
        free(*plain); *plain = NULL; 
        goto fail;
    }

    *plain_len = needed;
    EVP_PKEY_CTX_free(ctx);
    return 0;

fail:
    EVP_PKEY_CTX_free(ctx);
    return -1;
}

/*
 * aes256_cbc_encrypt
 *
 * AES-256-CBC encrypts `plain`.  Generates a fresh random 16-byte IV.
 * Output layout: [ IV (16 bytes) ][ ciphertext ]
 * The caller must free() *out.
 * Returns 0 on success, -1 on failure.
 */
static int aes256_cbc_encrypt(const unsigned char *key,
                                const unsigned char *plain,    size_t plain_len,
                                unsigned char      **out,      size_t *out_len)
{
    /* AES block size is always 16 bytes. */
    const int IV_LEN    = 16;
    const int BLOCK_LEN = 16;

    /* Worst-case ciphertext: plain_len + one full padding block. */
    size_t buf_size = IV_LEN + plain_len + BLOCK_LEN;
    *out = malloc(buf_size);
    if (!*out) return -1;

    /* Generate a fresh random IV. */
    unsigned char *iv = *out;
    if (RAND_bytes(iv, IV_LEN) != 1) { free(*out); *out = NULL; return -1; }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { free(*out); *out = NULL; return -1; }

    unsigned char *ct   = *out + IV_LEN;
    int            len1 = 0, len2 = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) goto fail;
    if (EVP_EncryptUpdate(ctx, ct, &len1, plain, (int)plain_len)  != 1) goto fail;
    if (EVP_EncryptFinal_ex(ctx, ct + len1, &len2)                != 1) goto fail;

    *out_len = (size_t)(IV_LEN + len1 + len2);
    EVP_CIPHER_CTX_free(ctx);
    return 0;

fail:
    EVP_CIPHER_CTX_free(ctx);
    free(*out); *out = NULL;
    return -1;
}

/*
 * aes256_cbc_decrypt
 *
 * AES-256-CBC decrypts `data` (layout: [ IV (16) ][ ciphertext ]).
 * The caller must free() *plain.
 * Returns 0 on success, -1 on failure (bad padding counts as failure).
 */
static int aes256_cbc_decrypt(const unsigned char *key, const unsigned char *data, size_t data_len, unsigned char **plain, size_t *plain_len) {
    const int IV_LEN = 16;
    if (data_len <= (size_t)IV_LEN) {
        return -1;
    }

    const unsigned char *iv = data;
    const unsigned char *ct = data + IV_LEN;
    size_t ct_len = data_len - IV_LEN;

    *plain = malloc(ct_len);   /* plaintext is always <= ciphertext length */
    if (!*plain) {
        return -1;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { 
        free(*plain); 
        *plain = NULL; 
        return -1; 
    }

    int len1 = 0, len2 = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) {
        goto fail;
    }
    if (EVP_DecryptUpdate(ctx, *plain, &len1, ct, (int)ct_len) != 1) {
        goto fail;
    }
    if (EVP_DecryptFinal_ex(ctx, *plain + len1, &len2) != 1) {
        goto fail;
    }

    *plain_len = (size_t)(len1 + len2);
    EVP_CIPHER_CTX_free(ctx);
    return 0;

fail:
    EVP_CIPHER_CTX_free(ctx);
    free(*plain); *plain = NULL;
    return -1;
}

/* ─────────────────────────── lifecycle ─────────────────────────────────── */

tetrissh_session_t *tetrissh_session_alloc(void) {
    tetrissh_session_t *sess = calloc(1, sizeof(tetrissh_session_t));
    if (!sess) {
        fprintf(stderr, "[tetrissh] tetrissh_session_alloc: out of memory\n");
        return NULL;
    }
    sess->sockfd = -1;
    snprintf(sess->last_error, sizeof(sess->last_error), "not yet initialised");
    return sess;
}

void tetrissh_session_free(tetrissh_session_t *sess) {
    if (!sess) {
        return;
    }
    memset(sess->session_key, 0, sizeof(sess->session_key));
    free(sess);
}

/* ─────────────────────────── handshake — server ─────────────────────────── */

int tetrissh_handshake_server(tetrissh_session_t *sess, int sockfd, const char *cert_path, const char *key_path) {
    if (!sess || sockfd < 0 || !cert_path || !key_path) {
        if (sess) {
            _err(sess, "handshake_server: invalid arguments");
        }
        return -1;
    }

    sess->sockfd = sockfd;

    /* ── Step 1: receive the client nonce (TETRISSH_NONCE_LEN raw bytes) ── */
    unsigned char nonce[TETRISSH_NONCE_LEN];
    if (recv_all(sockfd, nonce, TETRISSH_NONCE_LEN) != 0) {
        _err(sess, "handshake_server: failed to receive nonce");
        return -1;
    }

    /* ── Step 2: load our certificate and send it length-prefixed ────────── */
    X509 *cert = load_cert_file(cert_path);
    if (!cert) {
        _err(sess, "handshake_server: failed to load certificate");
        return -1;
    }

    size_t pem_len = 0;
    unsigned char *pem = cert_to_pem_bytes(cert, &pem_len);
    X509_free(cert);

    if (!pem) {
        _err(sess, "handshake_server: failed to serialise certificate");
        return -1;
    }

    if (send_length_prefixed(sockfd, pem, pem_len) != 0) {
        free(pem);
        _err(sess, "handshake_server: failed to send certificate");
        return -1;
    }
    free(pem);

    /* ── Step 3: sign the nonce with RSA-PSS; send signature ─────────────── */
    EVP_PKEY *privkey = load_private_key(key_path);
    if (!privkey) {
        _err(sess, "handshake_server: failed to load private key");
        return -1;
    }

    unsigned char *sig = NULL;
    size_t sig_len = 0;
    if (rsa_pss_sign(privkey, nonce, TETRISSH_NONCE_LEN, &sig, &sig_len) != 0) {
        EVP_PKEY_free(privkey);
        _ssl_err(sess, "handshake_server: RSA-PSS sign failed");
        return -1;
    }
    EVP_PKEY_free(privkey);

    if (send_length_prefixed(sockfd, sig, sig_len) != 0) {
        free(sig);
        _err(sess, "handshake_server: failed to send signature");
        return -1;
    }
    free(sig);

    /* ── Step 4: receive RSA-OAEP-encrypted session key; decrypt it ──────── */
    size_t         enc_key_len = 0;
    unsigned char *enc_key     = recv_length_prefixed(sockfd, &enc_key_len, 4096);
    if (!enc_key) {
        _err(sess, "handshake_server: failed to receive encrypted session key");
        return -1;
    }

    /* Reload the private key to decrypt. */
    privkey = load_private_key(key_path);
    if (!privkey) { 
        free(enc_key); 
        _err(sess, "handshake_server: key reload failed"); 
        return -1;
    }

    unsigned char *session_key = NULL;
    size_t session_key_len = 0;
    int rc = rsa_oaep_decrypt(privkey, enc_key, enc_key_len, &session_key, &session_key_len);
    EVP_PKEY_free(privkey);
    free(enc_key);

    if (rc != 0 || session_key_len != TETRISSH_SESSION_KEY_LEN) {
        free(session_key);
        _ssl_err(sess, "handshake_server: RSA-OAEP decrypt failed");
        return -1;
    }

    /* ── Step 5: store the session key; mark session ready ───────────────── */
    memcpy(sess->session_key, session_key, TETRISSH_SESSION_KEY_LEN);
    memset(session_key, 0, session_key_len);
    free(session_key);

    sess->ready = 1;
    snprintf(sess->last_error, sizeof(sess->last_error), "ok");
    return 0;
}

/* ─────────────────────────── handshake — client ─────────────────────────── */

int tetrissh_handshake_client(tetrissh_session_t *sess, int sockfd, const char *ca_path) {
    if (!sess || sockfd < 0 || !ca_path) {
        if (sess) {
            _err(sess, "handshake_client: invalid arguments");
        }
        return -1;
    }

    sess->sockfd = sockfd;

    /* ── Step 1: generate a fresh nonce and send it ───────────────────────── */
    unsigned char nonce[TETRISSH_NONCE_LEN];
    if (RAND_bytes(nonce, TETRISSH_NONCE_LEN) != 1) {
        _ssl_err(sess, "handshake_client: RAND_bytes nonce failed");
        return -1;
    }

    if (send_all(sockfd, nonce, TETRISSH_NONCE_LEN) != 0) {
        _err(sess, "handshake_client: failed to send nonce");
        return -1;
    }

    /* ── Step 2: receive server certificate; parse it ────────────────────── */
    size_t cert_pem_len = 0;
    unsigned char *cert_pem = recv_length_prefixed(sockfd, &cert_pem_len, 64 * 1024);
    if (!cert_pem) {
        _err(sess, "handshake_client: failed to receive certificate");
        return -1;
    }

    X509 *server_cert = pem_bytes_to_cert(cert_pem, cert_pem_len);
    free(cert_pem);

    if (!server_cert) {
        _err(sess, "handshake_client: failed to parse server certificate");
        return -1;
    }

    /* ── Step 3: verify the certificate against our CA ───────────────────── */
    if (!verify_cert_against_ca(server_cert, ca_path)) {
        X509_free(server_cert);
        _err(sess, "handshake_client: server certificate failed CA verification");
        return -1;
    }

    /* ── Step 4: receive RSA-PSS signature; verify it against the nonce ──── */
    size_t sig_len = 0;
    unsigned char *sig = recv_length_prefixed(sockfd, &sig_len, 4096);
    if (!sig) {
        X509_free(server_cert);
        _err(sess, "handshake_client: failed to receive signature");
        return -1;
    }

    EVP_PKEY *server_pubkey = X509_get_pubkey(server_cert);
    int verify_rc = rsa_pss_verify(server_pubkey, nonce, TETRISSH_NONCE_LEN, sig, sig_len);
    EVP_PKEY_free(server_pubkey);
    free(sig);

    if (verify_rc != 1) {
        X509_free(server_cert);
        _ssl_err(sess, "handshake_client: nonce signature verification failed");
        return -1;
    }

    /* ── Step 5: generate a fresh 32-byte AES session key ───────────────── */
    unsigned char session_key[TETRISSH_SESSION_KEY_LEN];
    if (RAND_bytes(session_key, TETRISSH_SESSION_KEY_LEN) != 1) {
        X509_free(server_cert);
        _ssl_err(sess, "handshake_client: RAND_bytes session key failed");
        return -1;
    }

    /* ── Step 6: RSA-OAEP encrypt the session key; send it ──────────────── */
    unsigned char *enc_key     = NULL;
    size_t         enc_key_len = 0;
    if (rsa_oaep_encrypt(server_cert, session_key, TETRISSH_SESSION_KEY_LEN, &enc_key, &enc_key_len) != 0) {
        X509_free(server_cert);
        _ssl_err(sess, "handshake_client: RSA-OAEP encrypt failed");
        return -1;
    }
    X509_free(server_cert);

    if (send_length_prefixed(sockfd, enc_key, enc_key_len) != 0) {
        free(enc_key);
        _err(sess, "handshake_client: failed to send encrypted session key");
        return -1;
    }
    free(enc_key);

    /* ── Step 7: store session key; mark ready ───────────────────────────── */
    memcpy(sess->session_key, session_key, TETRISSH_SESSION_KEY_LEN);
    memset(session_key, 0, sizeof(session_key));   /* scrub stack copy */

    sess->ready = 1;
    snprintf(sess->last_error, sizeof(sess->last_error), "ok");
    return 0;
}

/* ─────────────────────────── framed send / recv ────────────────────────── */

int tetrissh_send(tetrissh_session_t *sess, int sockfd, const unsigned char *plaintext, size_t plain_len) {
    if (!sess || !tetrissh_is_ready(sess)) {
        if (sess) _err(sess, "tetrissh_send: session not ready");
        return -1;
    }
    if (!plaintext || plain_len == 0) {
        _err(sess, "tetrissh_send: empty plaintext");
        return -1;
    }
    if (plain_len > TETRISSH_MAX_FRAME_LEN) {
        _err(sess, "tetrissh_send: plaintext exceeds max frame length");
        return -1;
    }

    /* Encrypt: output is [ IV (16) ][ ciphertext ] */
    unsigned char *frame     = NULL;
    size_t         frame_len = 0;
    if (aes256_cbc_encrypt(sess->session_key, plaintext, plain_len, &frame, &frame_len) != 0) {
        _ssl_err(sess, "tetrissh_send: AES-256-CBC encrypt failed");
        return -1;
    }

    /* Send as length-prefixed blob. */
    int rc = send_length_prefixed(sockfd, frame, frame_len);
    free(frame);

    if (rc != 0) {
        _err(sess, "tetrissh_send: send failed");
        return -1;
    }
    return 0;
}

unsigned char *tetrissh_recv(tetrissh_session_t *sess, int sockfd, size_t *out_len)
{
    if (out_len) {
        *out_len = 0;
    }

    if (!sess || !tetrissh_is_ready(sess)) {
        if (sess) {
            _err(sess, "tetrissh_recv: session not ready");
        }
        return NULL;
    }

    /* Read the encrypted frame (IV + ciphertext). */
    size_t frame_len = 0;
    unsigned char *frame = recv_length_prefixed(sockfd, &frame_len, TETRISSH_MAX_FRAME_LEN + 16 + 16);
    if (!frame) {
        _err(sess, "tetrissh_recv: failed to receive frame");
        return NULL;
    }

    /* Decrypt. */
    unsigned char *plain = NULL;
    size_t plain_len = 0;
    if (aes256_cbc_decrypt(sess->session_key, frame, frame_len, &plain, &plain_len) != 0) {
        free(frame);
        _ssl_err(sess, "tetrissh_recv: AES-256-CBC decrypt failed");
        return NULL;
    }
    free(frame);

    if (out_len) {
        *out_len = plain_len;
    }
    return plain;   /* caller must free() */
}

/* ─────────────────────────── teardown ──────────────────────────────────── */

void tetrissh_close(tetrissh_session_t *sess) {
    if (!sess) {
        return;
    }
    memset(sess->session_key, 0, sizeof(sess->session_key));
    sess->ready  = 0;
    sess->sockfd = -1;
    snprintf(sess->last_error, sizeof(sess->last_error), "session closed");
}

/* ─────────────────────────── utility ───────────────────────────────────── */

int tetrissh_is_ready(const tetrissh_session_t *sess) {
    return (sess && sess->ready) ? 1 : 0;
}

const char *tetrissh_strerror(const tetrissh_session_t *sess) {
    if (!sess) {
        return "null session";
    }
    return sess->last_error;
}