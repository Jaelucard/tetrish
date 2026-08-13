/*
 * tetrissh.c - libtetrissh: the secure session. Protocol lives in tetrissh.h.
 *
 * Crypto, all EVP, none of the deprecated low-level calls:
 *   - RAND_bytes           : nonce and session key generation
 *   - EVP_DigestSign*      : RSA-PSS signing   (server signs nonce)
 *   - EVP_DigestVerify*    : RSA-PSS verify    (client verifies nonce sig)
 *   - EVP_PKEY_encrypt*    : RSA-OAEP encrypt  (client wraps session key)
 *   - EVP_PKEY_decrypt*    : RSA-OAEP decrypt  (server unwraps session key)
 *   - EVP_Encrypt* /Decrypt*: AES-256-CBC        (framed payload encryption)
 *   - X509_verify_cert     : cert chain check   (client validates server cert)
 *
 * Every message after the handshake goes out as:
 *   [ 4-byte big-endian length ][ IV (16 bytes) ][ AES-256-CBC ciphertext ]
 *
 * The length counts the IV and ciphertext, not itself.
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
#include <sys/time.h>   /* struct timeval, for SO_RCVTIMEO / SO_SNDTIMEO */
#include <time.h>       /* clock_gettime(CLOCK_MONOTONIC) */

/* OpenSSL */
#include <openssl/rand.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/err.h>
#include <openssl/rsa.h>

#include "tetrissh.h"

/* session struct */

struct tetrissh_session {
    unsigned char session_key[TETRISSH_SESSION_KEY_LEN];
    int ready;
    char last_error[512];
    int sockfd;
};

/* internal helpers */

/* Pull the last OpenSSL error out of its queue and park it in sess->last_error. */
static void _ssl_err(tetrissh_session_t *sess, const char *context) {
    unsigned long e = ERR_get_error();
    char ssl_msg[256] = "(no OpenSSL error)";
    if (e) {
        ERR_error_string_n(e, ssl_msg, sizeof(ssl_msg));
    }
    snprintf(sess->last_error, sizeof(sess->last_error), "%s: %s", context, ssl_msg);
    fprintf(stderr, "[tetrissh] %s\n", sess->last_error);
}

/* Same, for errors that have nothing to do with OpenSSL. */
static void _err(tetrissh_session_t *sess, const char *msg) {
    snprintf(sess->last_error, sizeof(sess->last_error), "%s", msg);
    fprintf(stderr, "[tetrissh] %s\n", msg);
}

/* I/O budgets */

/*
 * Monotonic ms. CLOCK_MONOTONIC, not the wall clock, because nothing can step
 * it: an NTP correction must not be able to extend or expire a peer's budget.
 */
static long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * One time budget per logical operation (a whole handshake, or one framed
 * message), shared by every syscall inside it.
 *
 * Two different waits live in here. Conflating them is a bug either way, so
 * they get separate limits:
 *
 *   idle_ms    how long to wait for the peer's FIRST byte. 0 = wait forever.
 *   budget_ms  how long the operation gets once that byte has landed.
 *
 * Waiting for a peer to start talking is not the same as a peer talking too
 * slowly. A client sitting in tetrissh_recv between STATE broadcasts is idle,
 * not stalled, and killing it for that would disconnect every player any time
 * the server paused. The attack is a peer that sends a length header and then
 * dribbles the body out, and budget_ms is what bounds that.
 *
 * Why bother with a budget instead of just the socket's own timeout:
 * SO_RCVTIMEO and SO_SNDTIMEO expire per syscall. A recv() that times out
 * after copying some bytes returns the short count instead of failing, so the
 * reassembly loops below advance and call recv() again with a fresh, complete
 * timeout. One byte per timeout period trips nothing and holds the connection
 * forever, and since tetrisd runs these loops on its single event-loop thread,
 * "forever" means every room on the server stops ticking. Deriving each
 * syscall's timeout from an absolute deadline kills the reset: making progress
 * no longer buys more time, the remaining budget only shrinks.
 */
typedef struct {
    long long deadline;    /* absolute ms to finish by; 0 until the first byte */
    int       idle_ms;
    int       budget_ms;
} io_budget_t;

static io_budget_t io_budget(int idle_ms, int budget_ms)
{
    io_budget_t b;
    b.deadline  = 0;
    b.idle_ms   = idle_ms;
    b.budget_ms = budget_ms;
    return b;
}

/*
 * Arm the socket timeout for the next syscall: idle_ms while nothing has moved
 * yet, otherwise whatever is left of the deadline. A zero timeval means "no
 * timeout" to the kernel, which is exactly what idle_ms == 0 wants.
 *
 * 0 while the operation may continue, -1 once it is out of time.
 */
static int io_arm(int fd, int optname, const io_budget_t *b)
{
    struct timeval tv;

    if (b->deadline == 0) {
        tv.tv_sec  = (time_t)(b->idle_ms / 1000);
        tv.tv_usec = (suseconds_t)((b->idle_ms % 1000) * 1000);
    } else {
        long long remaining = b->deadline - mono_ms();
        if (remaining <= 0) {
            return -1;
        }
        tv.tv_sec  = (time_t)(remaining / 1000);
        tv.tv_usec = (suseconds_t)((remaining % 1000) * 1000);
    }

    if (setsockopt(fd, SOL_SOCKET, optname, &tv, sizeof tv) < 0) {
        return -1;
    }
    return 0;
}

/* First byte moved, so the operation has begun. Start its clock. */
static void io_started(io_budget_t *b)
{
    if (b->deadline == 0) {
        b->deadline = mono_ms() + b->budget_ms;
    }
}

/*
 * send_all / recv_all
 *
 * TCP is a stream, so one send() or recv() can move fewer bytes than asked.
 * These loop until exactly `len` bytes have moved, the shared budget runs
 * out, or something breaks.
 *
 * `b` is shared across every transfer in one operation, so a length header
 * and the body behind it are bounded together instead of each getting its own
 * fresh allowance.
 *
 * 0 on success, -1 on error.
 */
static int send_all(int fd, const void *buf, size_t len, io_budget_t *b)
{
    const unsigned char *p = buf;
    while (len > 0) {
        if (io_arm(fd, SO_SNDTIMEO, b) != 0) {
            return -1;
        }
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            return -1;
        }
        io_started(b);
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len, io_budget_t *b) {
    unsigned char *p = buf;
    while (len > 0) {
        if (io_arm(fd, SO_RCVTIMEO, b) != 0) {
            return -1;
        }
        ssize_t n = recv(fd, p, len, MSG_WAITALL);

        /* 0 = peer closed cleanly */
        if (n <= 0) {
            return -1;
        }
        io_started(b);
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

/*
 * send_length_prefixed / recv_length_prefixed
 *
 * Every variable-length blob on the wire carries a 4-byte big-endian length
 * in front of it. These do that field and the blob as one operation.
 *
 * recv_length_prefixed mallocs for the caller, who free()s it. NULL on any
 * error. max_len is the guard: check the declared length BEFORE allocating,
 * or a peer gets to pick our malloc size.
 */
static int send_length_prefixed(int fd, const unsigned char *data, size_t len,
                                io_budget_t *b) {
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(len >> 24);
    hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >>  8);
    hdr[3] = (uint8_t)(len);
    if (send_all(fd, hdr, 4, b) != 0) {
        return -1;
    }
    if (send_all(fd, data, len, b) != 0) {
        return -1;
    }
    return 0;
}

static unsigned char *recv_length_prefixed(int fd, size_t *out_len, size_t  max_len,
                                           io_budget_t *b) {
    uint8_t hdr[4];
    if (recv_all(fd, hdr, 4, b) != 0) {
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

    if (recv_all(fd, buf, len, b) != 0) {
        free(buf);
        return NULL;
    }

    *out_len = len;
    return buf;
}

/* crypto helpers */

/*
 * Reads a PEM private key off disk.
 * Caller EVP_PKEY_free()s it. Miss that and valgrind will tell on you.
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
 * Reads a PEM X.509 cert off disk.
 * Caller X509_free()s it.
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
 * X509* out to a heap PEM buffer, via a memory BIO.
 * Caller free()s the buffer; the BIO is ours and goes on every path.
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
 * The other direction: PEM bytes (NOT NUL-terminated, hence the length) to
 * an X509*. Caller X509_free()s it.
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
 * 1 if `cert` chains up to the CA in `ca_path`, 0 otherwise. Handshake step 3.
 * Free order below is ctx, then store, then CA, and it is that way round on
 * purpose: the ctx borrows the store, the store holds a ref on the CA.
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
 * RSA-PSS + SHA-256 over `msg_len` bytes at `msg`, signed with `pkey`.
 * Signature lands in a fresh malloc; caller free()s *sig.
 * 0 on success, -1 on failure.
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

    /* First call with a NULL buffer just asks how big the signature will be. */
    size_t needed = 0;
    if (EVP_DigestSignFinal(ctx, NULL, &needed) != 1) {
        goto fail;
    }

    *sig = malloc(needed);
    if (!*sig) {
        goto fail;
    }

    /* Second call does the actual signing. */
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
 * The verify half. Three-way return, and the caller has to respect it:
 * 1 valid, 0 invalid, -1 something broke. Treating -1 as "not 0" would
 * accept a bad signature whenever OpenSSL had a bad day.
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
 * RSA-OAEP (SHA-256) encrypt of `plain` under the pubkey inside `cert`.
 * Ciphertext lands in a fresh malloc; caller free()s *ct.
 * 0 on success, -1 on failure.
 *
 * X509_get_pubkey hands back a reference, not a borrow, so pubkey gets
 * EVP_PKEY_free()d on both paths out of here.
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

    /* Size query first, same NULL-buffer trick as the signing path. */
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
 * The unwrap half: RSA-OAEP (SHA-256) decrypt of `ct` with `pkey`.
 * Plaintext lands in a fresh malloc; caller free()s *plain.
 * 0 on success, -1 on failure.
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
 * AES-256-CBC over `plain`, with a fresh random 16-byte IV every call.
 * Layout out: [ IV (16 bytes) ][ ciphertext ]. Caller free()s *out.
 * 0 on success, -1 on failure.
 */
static int aes256_cbc_encrypt(const unsigned char *key,
                                const unsigned char *plain,    size_t plain_len,
                                unsigned char      **out,      size_t *out_len)
{
    /* AES block size is 16, always, whatever the key size. */
    const int IV_LEN    = 16;
    const int BLOCK_LEN = 16;

    /* Worst case is plain_len plus one whole padding block. */
    size_t buf_size = IV_LEN + plain_len + BLOCK_LEN;
    *out = malloc(buf_size);
    if (!*out) return -1;

    /* IV goes at the front of the same buffer, so the receiver gets it free. */
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
 * AES-256-CBC decrypt of `data`, laid out [ IV (16) ][ ciphertext ].
 * Caller free()s *plain.
 * 0 on success, -1 on failure. Bad padding is a failure, not a warning.
 */
static int aes256_cbc_decrypt(const unsigned char *key, const unsigned char *data, size_t data_len, unsigned char **plain, size_t *plain_len) {
    const int IV_LEN = 16;
    if (data_len <= (size_t)IV_LEN) {
        return -1;
    }

    const unsigned char *iv = data;
    const unsigned char *ct = data + IV_LEN;
    size_t ct_len = data_len - IV_LEN;

    *plain = malloc(ct_len);   /* padding only ever comes off, so this fits */
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

/* lifecycle */

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

/* handshake, server side */

int tetrissh_handshake_server(tetrissh_session_t *sess, int sockfd, const char *cert_path, const char *key_path) {
    if (!sess || sockfd < 0 || !cert_path || !key_path) {
        if (sess) {
            _err(sess, "handshake_server: invalid arguments");
        }
        return -1;
    }

    sess->sockfd = sockfd;

    /*
     * One deadline for the whole handshake, stamped before the first byte.
     *
     * This is the unauthenticated path. Nothing below has cost the peer more
     * than a connect(), and tetrisd runs it on the same thread that drives
     * every room's ticker. Bounding the operation instead of each syscall is
     * what stops a peer dripping its 32-byte nonce out a byte at a time and
     * stalling the whole server for as long as it feels like.
     */
    io_budget_t io = io_budget(TETRISSH_HANDSHAKE_TIMEOUT_MS,
                               TETRISSH_HANDSHAKE_TIMEOUT_MS);

    /* Step 1: the client nonce, 32 raw bytes, no length prefix. */
    unsigned char nonce[TETRISSH_NONCE_LEN];
    if (recv_all(sockfd, nonce, TETRISSH_NONCE_LEN, &io) != 0) {
        _err(sess, "handshake_server: failed to receive nonce");
        return -1;
    }

    /* Step 2: our cert, length-prefixed. */
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

    if (send_length_prefixed(sockfd, pem, pem_len, &io) != 0) {
        free(pem);
        _err(sess, "handshake_server: failed to send certificate");
        return -1;
    }
    free(pem);

    /* Step 3: RSA-PSS over their nonce, then ship the signature. */
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

    if (send_length_prefixed(sockfd, sig, sig_len, &io) != 0) {
        free(sig);
        _err(sess, "handshake_server: failed to send signature");
        return -1;
    }
    free(sig);

    /* Step 4: the wrapped session key comes back; unwrap it. */
    size_t         enc_key_len = 0;
    unsigned char *enc_key     = recv_length_prefixed(sockfd, &enc_key_len, 4096, &io);
    if (!enc_key) {
        _err(sess, "handshake_server: failed to receive encrypted session key");
        return -1;
    }

    /* Key was freed after signing, so load it again to decrypt. */
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

    /* Step 5: keep the key, flip ready, we are encrypted from here on. */
    memcpy(sess->session_key, session_key, TETRISSH_SESSION_KEY_LEN);
    memset(session_key, 0, session_key_len);
    free(session_key);

    sess->ready = 1;
    snprintf(sess->last_error, sizeof(sess->last_error), "ok");
    return 0;
}

/* handshake, client side */

int tetrissh_handshake_client(tetrissh_session_t *sess, int sockfd, const char *ca_path) {
    if (!sess || sockfd < 0 || !ca_path) {
        if (sess) {
            _err(sess, "handshake_client: invalid arguments");
        }
        return -1;
    }

    sess->sockfd = sockfd;

    /*
     * Same one-budget-per-handshake as the server side, for the mirror-image
     * reason: a hostile or wedged server dribbling its certificate out a byte
     * at a time would park the client in here forever, terminal already in
     * curses mode, no way back to the input loop.
     */
    io_budget_t io = io_budget(TETRISSH_HANDSHAKE_TIMEOUT_MS,
                               TETRISSH_HANDSHAKE_TIMEOUT_MS);

    /* Step 1: fresh nonce, every connection, no reuse. */
    unsigned char nonce[TETRISSH_NONCE_LEN];
    if (RAND_bytes(nonce, TETRISSH_NONCE_LEN) != 1) {
        _ssl_err(sess, "handshake_client: RAND_bytes nonce failed");
        return -1;
    }

    if (send_all(sockfd, nonce, TETRISSH_NONCE_LEN, &io) != 0) {
        _err(sess, "handshake_client: failed to send nonce");
        return -1;
    }

    /* Step 2: server cert arrives; parse it out of the PEM bytes. */
    size_t cert_pem_len = 0;
    unsigned char *cert_pem = recv_length_prefixed(sockfd, &cert_pem_len, 64 * 1024, &io);
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

    /* Step 3: does it chain to our bundled CA? If not, walk away. */
    if (!verify_cert_against_ca(server_cert, ca_path)) {
        X509_free(server_cert);
        _err(sess, "handshake_client: server certificate failed CA verification");
        return -1;
    }

    /* Step 4: signature over OUR nonce, checked against the cert's pubkey. */
    size_t sig_len = 0;
    unsigned char *sig = recv_length_prefixed(sockfd, &sig_len, 4096, &io);
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

    /* Step 5: 32 bytes of AES key, from RAND_bytes, not rand(). */
    unsigned char session_key[TETRISSH_SESSION_KEY_LEN];
    if (RAND_bytes(session_key, TETRISSH_SESSION_KEY_LEN) != 1) {
        X509_free(server_cert);
        _ssl_err(sess, "handshake_client: RAND_bytes session key failed");
        return -1;
    }

    /* Step 6: wrap it under the server's pubkey and send. */
    unsigned char *enc_key     = NULL;
    size_t         enc_key_len = 0;
    if (rsa_oaep_encrypt(server_cert, session_key, TETRISSH_SESSION_KEY_LEN, &enc_key, &enc_key_len) != 0) {
        X509_free(server_cert);
        _ssl_err(sess, "handshake_client: RSA-OAEP encrypt failed");
        return -1;
    }
    X509_free(server_cert);

    if (send_length_prefixed(sockfd, enc_key, enc_key_len, &io) != 0) {
        free(enc_key);
        _err(sess, "handshake_client: failed to send encrypted session key");
        return -1;
    }
    free(enc_key);

    /* Step 7: keep the key, flip ready. Handshake done. */
    memcpy(sess->session_key, session_key, TETRISSH_SESSION_KEY_LEN);
    memset(session_key, 0, sizeof(session_key));   /* wipe the stack copy */

    sess->ready = 1;
    snprintf(sess->last_error, sizeof(sess->last_error), "ok");
    return 0;
}

/* framed send / recv */

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

    /* Encrypt first, then frame. Output is [ IV (16) ][ ciphertext ]. */
    unsigned char *frame     = NULL;
    size_t         frame_len = 0;
    if (aes256_cbc_encrypt(sess->session_key, plaintext, plain_len, &frame, &frame_len) != 0) {
        _ssl_err(sess, "tetrissh_send: AES-256-CBC encrypt failed");
        return -1;
    }

    /*
     * Out as a length-prefixed blob, one deadline for the frame.
     *
     * tetrisd calls this from the room ticker, once per recipient, so a peer
     * that stops reading backs up this socket's send buffer and blocks us
     * right here. Without a per-operation bound, one player hitting Ctrl-Z
     * freezes every room on the server, not just their own.
     */
    io_budget_t io = io_budget(TETRISSH_FRAME_TIMEOUT_MS,
                               TETRISSH_FRAME_TIMEOUT_MS);
    int rc = send_length_prefixed(sockfd, frame, frame_len, &io);
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

    /*
     * Read the encrypted frame (IV + ciphertext).
     *
     * idle_ms is 0, i.e. wait as long as it takes for the frame to BEGIN. A
     * caller sitting between STATE broadcasts is legitimately idle and must
     * not be dropped for it. Both real callers gate this on readiness anyway
     * (tetrisd on epoll, tetrisu on select), so the first byte is normally
     * already there. What is bounded is everything after that first byte:
     * once a length header has arrived, the body has TETRISSH_FRAME_TIMEOUT_MS
     * to show up, however the peer chooses to pace it.
     */
    size_t frame_len = 0;
    io_budget_t io = io_budget(0, TETRISSH_FRAME_TIMEOUT_MS);
    unsigned char *frame = recv_length_prefixed(sockfd, &frame_len,
                                                TETRISSH_MAX_FRAME_LEN + 16 + 16,
                                                &io);
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

/* teardown */

void tetrissh_close(tetrissh_session_t *sess) {
    if (!sess) {
        return;
    }
    memset(sess->session_key, 0, sizeof(sess->session_key));
    sess->ready  = 0;
    sess->sockfd = -1;
    snprintf(sess->last_error, sizeof(sess->last_error), "session closed");
}

/* utility */

int tetrissh_is_ready(const tetrissh_session_t *sess) {
    return (sess && sess->ready) ? 1 : 0;
}

const char *tetrissh_strerror(const tetrissh_session_t *sess) {
    if (!sess) {
        return "null session";
    }
    return sess->last_error;
}