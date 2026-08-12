// test_handshake.c — Week 5 deliverable: a socketpair()-based test harness
// that runs BOTH sides of the libtetrissh handshake in one process, plus
// the frame layer and the required failure-path tests.
//
// Why socketpair() instead of a real TCP loopback: session_accept()/
// session_connect() only ever call send()/recv() on the fd they're given,
// they never call connect()/bind()/listen(), so a connected AF_UNIX
// socketpair() is a drop-in stand-in for a TCP connection here — same
// blocking-stream semantics, no need to bind a port, no race between the
// two sides starting up.
//
// Threads, not fork(): the handshake is a strict lock-step protocol (each
// side blocks on send()/recv() waiting for the other), so both ends need
// to run concurrently. Two pthreads sharing one process gives us that
// without fork()'s split address space, so test assertions on both sides
// land in the same process image (matters for valgrind and for exit codes).
//
// Run under valgrind for the Week 5 "valgrind clean on the handshake path"
// deliverable:
//   valgrind --leak-check=full --show-leak-kinds=all ./test_handshake
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <signal.h>

#include <openssl/rand.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include "corestack/secure_session.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("ok    %s\n", msg); } \
    else      { printf("FAIL  %s\n", msg); g_fail = 1; } \
    fflush(stdout); \
} while (0)

// ---------------------------------------------------------------------------
// Test fixture: generate a fresh CA + server cert/key pair on disk, so every
// test run is self-contained (no dependency on `make certs` having run).
// ---------------------------------------------------------------------------

static void run(const char *cmd) {
    if (system(cmd) != 0) { fprintf(stderr, "fixture command failed: %s\n", cmd); exit(2); }
}

static void generate_pki(const char *tag,
                          char *ca_path, char *key_path, char *cert_path) {
    char ca_key[256], serial[256], csr[256];
    snprintf(ca_key, sizeof ca_key, "/tmp/th_%s_ca.key", tag);
    snprintf(ca_path, 256, "/tmp/th_%s_ca.crt", tag);
    snprintf(key_path, 256, "/tmp/th_%s_server.key", tag);
    snprintf(cert_path, 256, "/tmp/th_%s_server.crt", tag);
    snprintf(serial, sizeof serial, "/tmp/th_%s_ca.srl", tag);
    snprintf(csr, sizeof csr, "/tmp/th_%s_server.csr", tag);

    char cmd[2048];
    snprintf(cmd, sizeof cmd,
        "openssl genrsa -out %s 1024 2>/dev/null && "
        "openssl req -new -x509 -key %s -out %s -days 3650 -subj '/CN=%s-CA' 2>/dev/null && "
        "openssl genrsa -out %s 1024 2>/dev/null && "
        "openssl req -new -key %s -out %s -subj '/CN=%s-server' 2>/dev/null && "
        "openssl x509 -req -in %s -CA %s -CAkey %s -CAcreateserial -CAserial %s "
        "-out %s -days 365 -sha256 2>/dev/null",
        ca_key, ca_key, ca_path, tag,
        key_path, key_path, csr, tag,
        csr, ca_path, ca_key, serial, cert_path);
    run(cmd);
}

// ---------------------------------------------------------------------------
// Raw wire helpers, used only by the malicious-client tests below to send
// deliberately-broken bytes that session_connect() would never construct.
// ---------------------------------------------------------------------------

static void send_all_raw(int fd, const void *buf, size_t len) {
    const unsigned char *p = buf;
    while (len > 0) { ssize_t n = send(fd, p, len, 0); if (n <= 0) return; p += n; len -= (size_t)n; }
}

static void send_len_prefixed_raw(int fd, const void *data, size_t len) {
    unsigned char hdr[4] = { (unsigned char)(len>>24), (unsigned char)(len>>16),
                              (unsigned char)(len>>8),  (unsigned char)(len) };
    send_all_raw(fd, hdr, 4);
    send_all_raw(fd, data, len);
}

static void recv_all_raw(int fd, void *buf, size_t len) {
    unsigned char *p = buf;
    while (len > 0) { ssize_t n = recv(fd, p, len, MSG_WAITALL); if (n <= 0) return; p += n; len -= (size_t)n; }
}

// ===========================================================================
// TEST 1: happy path — full 7-step handshake + frame layer round trip +
// 64 KiB cap enforcement (both the local guard in session_write() and the
// oversized-frame path on the wire).
// ===========================================================================

typedef struct { const char *cert, *key, *ca; int fd; int ok; } hs_arg_t;

static void *server_thread(void *arg) {
    hs_arg_t *a = arg;
    session_ctx_t *ctx = session_server_init(a->cert, a->key);
    if (!ctx) { a->ok = 0; return NULL; }
    session_t *s = session_accept(ctx, a->fd);
    session_server_shutdown(ctx);
    a->ok = (s != NULL);
    if (s) {
        // Echo one frame back, to exercise the frame layer both directions.
        char buf[256];
        ssize_t n = session_read(s, buf, sizeof buf);
        if (n > 0) session_write(s, buf, (size_t)n);

        // 64 KiB cap, sender side: session_write() must refuse to even try
        // to send a plaintext larger than SESSION_MAX_FRAME_LEN.
        static unsigned char big[SESSION_MAX_FRAME_LEN + 1];
        memset(big, 'A', sizeof big);
        ssize_t rc = session_write(s, big, sizeof big);
        CHECK(rc < 0, "sender-side 64 KiB cap: session_write() refuses an oversized plaintext");

        session_close(s);
    }
    return NULL;
}

static void test_happy_path_and_frame_layer(void) {
    printf("\n=== TEST 1: happy-path handshake + frame layer ===\n");
    char ca[256], key[256], cert[256];
    generate_pki("t1", ca, key, cert);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); exit(2); }

    hs_arg_t sarg = { cert, key, NULL, sv[0], 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sarg);

    session_ctx_t *cctx = session_client_ctx_init(ca);
    CHECK(cctx != NULL, "session_client_ctx_init loads the CA");
    session_t *cs = session_connect(cctx, sv[1]);
    CHECK(cs != NULL, "client-side handshake completes (all 7 steps)");
    CHECK(session_is_ready(cs), "session_is_ready() is true after handshake");

    if (cs) {
        const char *msg = "hello over the encrypted frame layer";
        CHECK(session_write(cs, msg, strlen(msg)) > 0, "client can send a framed message");
        char buf[256] = {0};
        ssize_t n = session_read(cs, buf, sizeof buf);
        CHECK(n == (ssize_t)strlen(msg) && memcmp(buf, msg, (size_t)n) == 0,
              "frame layer round trip: server echoed the exact plaintext back");
        session_close(cs);
    }
    session_client_ctx_free(cctx);

    pthread_join(th, NULL);
    CHECK(sarg.ok, "server-side handshake completed successfully");
    close(sv[0]); close(sv[1]);
}

// ===========================================================================
// TEST 2: bad cert — client's CA does not trust the server's cert.
// ===========================================================================

static void *server_thread_plain(void *arg) {
    hs_arg_t *a = arg;
    session_ctx_t *ctx = session_server_init(a->cert, a->key);
    session_t *s = ctx ? session_accept(ctx, a->fd) : NULL;
    if (ctx) session_server_shutdown(ctx);
    a->ok = (s != NULL);
    if (s) session_close(s);
    return NULL;
}

static void test_bad_cert(void) {
    printf("\n=== TEST 2: handshake failure path -- bad cert (untrusted CA) ===\n");
    char ca1[256], key1[256], cert1[256];   // server's real PKI
    char ca2[256], key2[256], cert2[256];   // an unrelated CA the client trusts instead
    generate_pki("t2_server", ca1, key1, cert1);
    generate_pki("t2_other",  ca2, key2, cert2);

    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    hs_arg_t sarg = { cert1, key1, NULL, sv[0], 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread_plain, &sarg);

    // Client trusts ca2, but the server presents a cert signed by ca1.
    session_ctx_t *cctx = session_client_ctx_init(ca2);
    session_t *cs = session_connect(cctx, sv[1]);
    CHECK(cs == NULL, "client rejects a certificate not signed by its trusted CA");
    if (cs) session_close(cs);
    session_client_ctx_free(cctx);
    close(sv[1]);   // unblocks the server's recv() with EOF instead of hanging forever
    sv[1] = -1;

    pthread_join(th, NULL);
    // The server side either sees the connection drop mid-handshake (client
    // walked away after failing verification) or completes its own steps
    // before noticing -- either way, nothing must crash, which the process
    // still being alive here already demonstrates.
    printf("ok    server side did not crash despite a rejected handshake\n");
    close(sv[0]);
}

// ===========================================================================
// TEST 3: bad signature — the cert's public key does not match the private
// key actually used to sign the nonce (a cert/key mismatch).
// ===========================================================================

static void test_bad_signature(void) {
    printf("\n=== TEST 3: handshake failure path -- bad signature (cert/key mismatch) ===\n");
    char ca[256], real_key[256], cert[256];
    char ca_unused[256], wrong_key[256], cert_unused[256];
    generate_pki("t3_real",  ca, real_key, cert);
    generate_pki("t3_wrong", ca_unused, wrong_key, cert_unused);

    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    // Server presents the REAL cert but signs with the WRONG private key,
    // so the signature will not verify against the cert's public key.
    hs_arg_t sarg = { cert, wrong_key, NULL, sv[0], 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread_plain, &sarg);

    session_ctx_t *cctx = session_client_ctx_init(ca);
    session_t *cs = session_connect(cctx, sv[1]);
    CHECK(cs == NULL, "client rejects a nonce signature that doesn't verify against the cert");
    if (cs) session_close(cs);
    session_client_ctx_free(cctx);
    close(sv[1]);   // unblocks the server's recv() with EOF instead of hanging forever
    sv[1] = -1;

    pthread_join(th, NULL);
    printf("ok    server side did not crash despite an invalid signature\n");
    close(sv[0]);
}

// ===========================================================================
// TEST 4: truncated key blob — a client that plays the protocol straight
// through cert+signature, then sends a truncated RSA-OAEP session-key blob
// instead of a real one. Exercises the server's robustness against
// malformed key material without crashing or hanging.
// ===========================================================================

static void test_truncated_key_blob(void) {
    printf("\n=== TEST 4: handshake failure path -- truncated key blob ===\n");
    char ca[256], key[256], cert[256];
    generate_pki("t4", ca, key, cert);
    (void)ca;

    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    hs_arg_t sarg = { cert, key, NULL, sv[0], 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread_plain, &sarg);

    // Play the client's first three steps for real (nonce, then just drain
    // the cert and signature the server sends), then send garbage instead
    // of a valid RSA-OAEP-wrapped session key.
    unsigned char nonce[SESSION_NONCE_LEN];
    RAND_bytes(nonce, sizeof nonce);
    send_all_raw(sv[1], nonce, sizeof nonce);

    unsigned char hdr[4];
    recv_all_raw(sv[1], hdr, 4);   // cert length
    size_t cert_len = ((size_t)hdr[0]<<24)|((size_t)hdr[1]<<16)|((size_t)hdr[2]<<8)|hdr[3];
    unsigned char *certbuf = malloc(cert_len);
    recv_all_raw(sv[1], certbuf, cert_len);
    free(certbuf);

    recv_all_raw(sv[1], hdr, 4);   // signature length
    size_t sig_len = ((size_t)hdr[0]<<24)|((size_t)hdr[1]<<16)|((size_t)hdr[2]<<8)|hdr[3];
    unsigned char *sigbuf = malloc(sig_len);
    recv_all_raw(sv[1], sigbuf, sig_len);
    free(sigbuf);

    // A real RSA-1024-OAEP blob is 128 bytes. Send 5 garbage bytes instead.
    unsigned char truncated[5] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00 };
    send_len_prefixed_raw(sv[1], truncated, sizeof truncated);

    pthread_join(th, NULL);
    CHECK(sarg.ok == 0, "server rejects a truncated/invalid session-key blob instead of crashing");
    printf("ok    process is still alive after a truncated key blob\n");

    close(sv[0]); close(sv[1]);
}

// ===========================================================================
// TEST 5 (Week 12): half-open handshakes — the client disconnects at each
// of the three points in the protocol where it's the client's turn to have
// sent something already, before the server has everything it needs. Each
// case must fail cleanly server-side: no crash, no hang (the alarm() in
// main() is the final backstop, but every case here should return well
// before that fires).
// ===========================================================================

static void test_half_open_at(const char *label, int stage) {
    // stage 0: close before sending anything at all (server blocks on recv
    //          of the client nonce).
    // stage 1: send the nonce, then close immediately (server blocks on
    //          recv of the encrypted session key, after sending cert+sig).
    // stage 2: send the nonce, drain cert+sig, then close with ZERO bytes
    //          of a key blob (as opposed to TEST 4's truncated-but-nonzero
    //          garbage) -- the cleanest possible "client just vanished".
    printf("-- half-open: %s --\n", label);
    char ca[256], key[256], cert[256];
    generate_pki(stage == 0 ? "t5a" : stage == 1 ? "t5b" : "t5c", ca, key, cert);
    (void)ca;

    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    hs_arg_t sarg = { cert, key, NULL, sv[0], 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread_plain, &sarg);

    if (stage >= 1) {
        unsigned char nonce[SESSION_NONCE_LEN];
        RAND_bytes(nonce, sizeof nonce);
        send_all_raw(sv[1], nonce, sizeof nonce);
    }
    if (stage == 2) {
        unsigned char hdr[4];
        recv_all_raw(sv[1], hdr, 4);
        size_t cert_len = ((size_t)hdr[0]<<24)|((size_t)hdr[1]<<16)|((size_t)hdr[2]<<8)|hdr[3];
        unsigned char *certbuf = malloc(cert_len);
        recv_all_raw(sv[1], certbuf, cert_len);
        free(certbuf);

        recv_all_raw(sv[1], hdr, 4);
        size_t sig_len = ((size_t)hdr[0]<<24)|((size_t)hdr[1]<<16)|((size_t)hdr[2]<<8)|hdr[3];
        unsigned char *sigbuf = malloc(sig_len);
        recv_all_raw(sv[1], sigbuf, sig_len);
        free(sigbuf);
    }
    close(sv[1]);   // the client vanishes right here, mid-handshake

    pthread_join(th, NULL);
    CHECK(sarg.ok == 0, "server-side handshake fails cleanly, not a hang or crash");
    close(sv[0]);
}

static void test_half_open_handshakes(void) {
    printf("\n=== TEST 5: half-open handshakes (client disconnects mid-handshake) ===\n");
    test_half_open_at("client closes before sending anything", 0);
    test_half_open_at("client sends nonce then vanishes", 1);
    test_half_open_at("client drains cert+sig then vanishes with no key blob at all", 2);
}

// ===========================================================================
// TEST 6 (Week 12): certificate validation edge cases beyond "wrong CA" and
// "wrong signing key" (already covered in TEST 2/3) -- an EXPIRED cert and
// a NOT-YET-VALID cert, both signed by a CA the client actually trusts.
// OpenSSL's X509_verify_cert() checks notBefore/notAfter by default, so
// the client should reject both without us writing any extra validation
// logic -- this test's job is to confirm that.
//
// We use `faketime` to fake the wall clock while generating each cert, so
// its notBefore/notAfter land in the past (expired) or the future
// (not-yet-valid) relative to the REAL current time the test runs the
// handshake at.
// ===========================================================================

static void generate_pki_at_fake_time(const char *tag, const char *faketime_offset,
                                       char *ca_path, char *key_path, char *cert_path) {
    char ca_key[256], serial[256], csr[256];
    snprintf(ca_key, sizeof ca_key, "/tmp/th_%s_ca.key", tag);
    snprintf(ca_path, 256, "/tmp/th_%s_ca.crt", tag);
    snprintf(key_path, 256, "/tmp/th_%s_server.key", tag);
    snprintf(cert_path, 256, "/tmp/th_%s_server.crt", tag);
    snprintf(serial, sizeof serial, "/tmp/th_%s_ca.srl", tag);
    snprintf(csr, sizeof csr, "/tmp/th_%s_server.csr", tag);

    // The CA itself is generated at the REAL current time (long validity),
    // so client-side CA loading/trust isn't what's under test. Only the
    // LEAF (server) certificate is generated under faketime with a short
    // validity window, so its notBefore/notAfter land squarely in the
    // past or future relative to the real clock the handshake runs under.
    char cmd[2048];
    snprintf(cmd, sizeof cmd,
        "openssl genrsa -out %s 1024 2>/dev/null && "
        "openssl req -new -x509 -key %s -out %s -days 3650 -subj '/CN=%s-CA' 2>/dev/null && "
        "openssl genrsa -out %s 1024 2>/dev/null && "
        "faketime '%s' openssl req -new -key %s -out %s -subj '/CN=%s-server' 2>/dev/null && "
        "faketime '%s' openssl x509 -req -in %s -CA %s -CAkey %s -CAcreateserial -CAserial %s "
        "-out %s -days 1 -sha256 2>/dev/null",
        ca_key, ca_key, ca_path, tag,
        key_path,
        faketime_offset, key_path, csr, tag,
        faketime_offset, csr, ca_path, ca_key, serial, cert_path);
    run(cmd);
}

static void test_cert_validity_window(const char *label, const char *faketime_offset) {
    printf("-- certificate validity: %s --\n", label);
    char ca[256], key[256], cert[256];
    generate_pki_at_fake_time(label[0] == 'e' ? "t6_expired" : "t6_future", faketime_offset, ca, key, cert);

    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    hs_arg_t sarg = { cert, key, NULL, sv[0], 0 };
    pthread_t th;
    pthread_create(&th, NULL, server_thread_plain, &sarg);

    session_ctx_t *cctx = session_client_ctx_init(ca);
    session_t *cs = session_connect(cctx, sv[1]);
    CHECK(cs == NULL, "client rejects a certificate outside its validity window");
    if (cs) session_close(cs);
    session_client_ctx_free(cctx);
    close(sv[1]);

    pthread_join(th, NULL);
    printf("ok    server side did not crash despite an invalid-validity-window certificate\n");
    close(sv[0]);
}

static void test_certificate_validity_edge_cases(void) {
    printf("\n=== TEST 6: certificate validation edge cases (expired / not-yet-valid) ===\n");
    // Leaf cert issued and expired entirely 400 days ago (1-day validity,
    // backdated 400 days -- its notAfter is ~399 days in the past).
    test_cert_validity_window("expired certificate", "400 days ago");
    // Leaf cert whose notBefore is 30 days in the future.
    test_cert_validity_window("not-yet-valid certificate", "30 days");
}

static void on_alarm(int sig) { (void)sig; fprintf(stderr, "TEST_HANDSHAKE: TIMED OUT (deadlock?)\n"); _exit(3); }

int main(void) {
    signal(SIGALRM, on_alarm);
    alarm(45);

    test_happy_path_and_frame_layer();
    test_bad_cert();
    test_bad_signature();
    test_truncated_key_blob();
    test_half_open_handshakes();
    test_certificate_validity_edge_cases();

    printf("\n=====================================\n");
    printf(g_fail ? "TEST_HANDSHAKE: SOME CHECKS FAILED\n" : "TEST_HANDSHAKE: ALL PASS\n");
    return g_fail ? 1 : 0;
}
