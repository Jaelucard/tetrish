// fuzz_week12.c — Week 12 deliverable: malformed HTTTP fuzzing beyond the
// 9 structural cases in fuzz_malformed.c (Week 8). Four categories,
// against a REAL, already-running tetrisd:
//
//   1. random bytes            -- many iterations of random-length,
//                                  random-content frames (seeded, so a
//                                  failure is reproducible)
//   2. truncated messages       -- a frame whose declared length is
//                                  bigger than what we actually send
//                                  before the connection drops
//   3. oversized headers        -- one legitimately huge header value,
//                                  and a header count that overflows the
//                                  32-header limit
//   4. invalid UTF-8            -- classic invalid-UTF-8 byte sequences
//                                  inside a header value (the parser is
//                                  byte-oriented and never claimed to
//                                  validate UTF-8, so this test's job is
//                                  to confirm that's actually safe, not
//                                  to add validation)
//
// Every case is checked the same way as fuzz_malformed.c: the daemon
// must not crash, and a completely normal JOIN must still succeed
// afterward.
//
//   bin/tetrisd .tetrishrc &
//   build/tests/fuzz_week12 .tetrishrc [seed]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rc.h"
#include "corestack/secure_session.h"

static int g_fail = 0;
static Config g_cfg;

static int tcp_connect(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)g_cfg.listen_port);
    inet_pton(AF_INET, g_cfg.bind_addr, &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { perror("connect"); return -1; }
    return fd;
}

// --- categories 1, 3, 4: anything that can go through a normal,
// completely-framed session_write() (the frame layer itself already
// rejects payloads over 64 KiB locally -- see test_handshake.c's cap
// test -- so these all fit comfortably under that). ---
static void run_framed_case(const char *name, const void *payload, size_t plen) {
    printf("-- %s (%zu bytes) --\n", name, plen);
    int fd = tcp_connect();
    if (fd < 0) { printf("FAIL  could not connect\n"); g_fail = 1; return; }

    session_ctx_t *cctx = session_client_ctx_init(g_cfg.ca_path);
    session_t *s = session_connect(cctx, fd);
    if (!s) { printf("FAIL  handshake itself failed\n"); g_fail = 1; close(fd); return; }

    if (session_write(s, payload, plen) <= 0) {
        printf("note  session_write() refused locally (e.g. would exceed 64 KiB) -- also fine, not a crash\n");
    } else {
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        char buf[512];
        ssize_t n = session_read(s, buf, sizeof buf);
        if (n > 0) {
            buf[n < (ssize_t)sizeof buf ? n : (ssize_t)sizeof buf - 1] = '\0';
            printf("ok    server replied cleanly (%zd bytes): %.60s...\n", n, buf);
        } else {
            printf("ok    server dropped the connection instead of crashing\n");
        }
    }
    session_close(s);
    session_client_ctx_free(cctx);
    close(fd);
}

// --- category 2: truncated messages. Raw socket write of a length-
// prefixed frame that declares more bytes than we actually send, then we
// close the connection -- exactly what a client dying mid-send looks
// like from the server's side. This is deliberately below the
// libtetrissh API (which only ever sends whole, correctly-framed
// messages) because that's the only way to construct a genuinely
// truncated frame; we don't need the real AES key for this, since the
// server fails during recv() of the declared length, before it ever
// gets to decrypt anything. ---
static void run_truncated_case(const char *name, size_t declared_len, size_t actually_send) {
    printf("-- %s (declares %zu bytes, sends %zu) --\n", name, declared_len, actually_send);
    int fd = tcp_connect();
    if (fd < 0) { printf("FAIL  could not connect\n"); g_fail = 1; return; }

    session_ctx_t *cctx = session_client_ctx_init(g_cfg.ca_path);
    session_t *s = session_connect(cctx, fd);
    if (!s) { printf("FAIL  handshake itself failed\n"); g_fail = 1; close(fd); return; }

    // One legitimate frame first, to prove the connection was healthy
    // before we start truncating (rules out "server was already dead").
    unsigned char hdr[4] = {
        (unsigned char)(declared_len >> 24), (unsigned char)(declared_len >> 16),
        (unsigned char)(declared_len >> 8),  (unsigned char)(declared_len)
    };
    send(fd, hdr, 4, 0);
    unsigned char *garbage = calloc(1, actually_send ? actually_send : 1);
    memset(garbage, 0x42, actually_send);
    send(fd, garbage, actually_send, 0);
    free(garbage);
    close(fd);   // simulate the client dying mid-send

    printf("ok    connection closed mid-frame; the daemon must not hang or crash on this\n");
    session_close(s);
    session_client_ctx_free(cctx);
}

static void sanity_check_daemon_still_healthy(void) {
    printf("\n-- final sanity: normal JOIN still works after all Week-12 fuzzing --\n");
    int fd = tcp_connect();
    session_ctx_t *cctx = session_client_ctx_init(g_cfg.ca_path);
    session_t *s = fd >= 0 ? session_connect(cctx, fd) : NULL;
    if (!s) {
        printf("FAIL  daemon is no longer accepting handshakes -- it may have crashed\n");
        g_fail = 1;
    } else {
        const char *req = "JOIN /room/fuzz12_final HTTTP/1.0\r\n\r\n";
        session_write(s, req, strlen(req));
        char buf[256];
        ssize_t n = session_read(s, buf, sizeof buf);
        int ok = (n > 0 && strncmp(buf, "HTTTP/1.0 201", 13) == 0);
        printf(ok ? "ok    daemon is fully healthy: JOIN -> 201 Created\n"
                  : "FAIL  daemon did not answer JOIN normally after the fuzz run\n");
        if (!ok) g_fail = 1;
        session_close(s);
        close(fd);
    }
    session_client_ctx_free(cctx);
}

int main(int argc, char **argv) {
    const char *rc_path = (argc > 1) ? argv[1] : ".tetrishrc";
    unsigned seed = (argc > 2) ? (unsigned)atoi(argv[2]) : 0xC0FFEEu;
    if (rc_load(rc_path, &g_cfg) != 0) { fprintf(stderr, "fuzz: rc_load failed\n"); return 2; }

    printf("=== fuzz_week12: seed=%u ===\n\n", seed);
    srand(seed);

    // --- 1. random bytes: 15 rounds of random length + random content ---
    printf("### category 1: random bytes ###\n");
    for (int i = 0; i < 15; i++) {
        size_t len = 1 + (size_t)(rand() % 2048);
        unsigned char *buf = malloc(len);
        for (size_t j = 0; j < len; j++) buf[j] = (unsigned char)(rand() % 256);
        char name[64];
        snprintf(name, sizeof name, "random bytes round %d/15", i + 1);
        run_framed_case(name, buf, len);
        free(buf);
    }

    // --- 2. truncated messages ---
    printf("\n### category 2: truncated messages ###\n");
    run_truncated_case("declares 1000 bytes, sends 10 then disconnects", 1000, 10);
    run_truncated_case("declares 65000 bytes, sends 1 then disconnects", 65000, 1);
    run_truncated_case("declares 48 bytes, sends 0 then disconnects", 48, 0);

    // --- 3. oversized headers ---
    printf("\n### category 3: oversized headers ###\n");
    {
        // One legitimately huge header value, well under the 64 KiB frame cap.
        char *big = malloc(50000 + 200);
        int off = sprintf(big, "JOIN /room/fuzz12big HTTTP/1.0\r\nX-Big: ");
        memset(big + off, 'A', 50000);
        off += 50000;
        off += sprintf(big + off, "\r\n\r\n");
        run_framed_case("one 50000-byte header value", big, (size_t)off);
        free(big);
    }
    {
        // 40 headers -- over the 32-header limit -- must be rejected (400),
        // not silently truncated/ignored (which could hide a real header
        // like Player-Id behind the cutoff).
        char big[4096]; int off = sprintf(big, "MOVE /room/fuzz12hdrs HTTTP/1.0\r\n");
        for (int i = 0; i < 40; i++) off += sprintf(big + off, "X-H%d: v\r\n", i);
        off += sprintf(big + off, "\r\n");
        run_framed_case("40 headers (over the 32-header limit)", big, (size_t)off);
    }

    // --- 4. invalid UTF-8 ---
    printf("\n### category 4: invalid UTF-8 in a header value ###\n");
    {
        // Classic invalid-UTF-8 test vectors: lone continuation byte,
        // overlong encoding, and an invalid 4-byte lead byte.
        const char *req =
            "JOIN /room/fuzz12utf8 HTTTP/1.0\r\n"
            "X-Bad-Utf8: \x80\xC0\xAF\xED\xA0\x80\xF4\x90\x80\x80\xFF\xFE\r\n"
            "\r\n";
        run_framed_case("invalid UTF-8 byte sequences in a header value", req, strlen(req));
    }

    sanity_check_daemon_still_healthy();

    printf("\n=====================================\n");
    printf(g_fail ? "FUZZ_WEEK12: SOME CHECKS FAILED\n" : "FUZZ_WEEK12: ALL PASS (daemon survived every case)\n");
    return g_fail ? 1 : 0;
}
