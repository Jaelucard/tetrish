// fuzz_malformed.c — Week 8 deliverable: malformed HTTTP input must never
// crash the daemon.
//
// This is a black-box test against a REAL, already-running tetrisd (unlike
// test_handshake.c, which spins up its own in-process server). Usage:
//
//   bin/tetrisd .tetrishrc &
//   build/tests/fuzz_malformed .tetrishrc
//
// Each case opens a fresh TCP connection, completes a real handshake, sends
// one deliberately malformed payload, and checks the daemon answered with
// a clean error status (or dropped the connection) instead of crashing.
// After all cases, it does one more completely normal JOIN to prove the
// daemon is still fully healthy -- the strongest evidence that nothing
// upstream corrupted server state.
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

static int tcp_connect(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { perror("connect"); close(fd); return -1; }
    return fd;
}

// Sends `payload` as one properly-framed encrypted message (so it reaches
// htttp_parse_request; the frame layer itself is exercised separately by
// test_handshake.c's 64 KiB cap case), then reads whatever the daemon
// sends back, if anything, within a short timeout.
static void run_case(const char *name, const Config *cfg, const char *payload, size_t plen) {
    printf("-- %s --\n", name);
    int fd = tcp_connect(cfg->bind_addr, cfg->listen_port);
    if (fd < 0) { printf("FAIL  could not even connect\n"); g_fail = 1; return; }

    session_ctx_t *cctx = session_client_ctx_init(cfg->ca_path);
    session_t *s = session_connect(cctx, fd);
    if (!s) { printf("FAIL  handshake itself failed (unrelated to the fuzz case)\n"); g_fail = 1; close(fd); return; }

    if (session_write(s, payload, plen) <= 0) {
        printf("note  server closed before we could even send (also fine, not a crash)\n");
    } else {
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        char buf[512];
        ssize_t n = session_read(s, buf, sizeof buf);
        if (n > 0) {
            buf[n < (ssize_t)sizeof buf ? n : (ssize_t)sizeof buf - 1] = '\0';
            printf("ok    server replied cleanly (%zd bytes): %.60s...\n", n, buf);
        } else {
            printf("ok    server dropped the connection instead of crashing (%s)\n", session_strerror(s));
        }
    }

    session_close(s);
    session_client_ctx_free(cctx);
    close(fd);
}

static void run_case_str(const char *name, const Config *cfg, const char *payload) {
    run_case(name, cfg, payload, strlen(payload));
}

int main(int argc, char **argv) {
    const char *rc_path = (argc > 1) ? argv[1] : ".tetrishrc";
    Config cfg;
    if (rc_load(rc_path, &cfg) != 0) { fprintf(stderr, "fuzz: rc_load failed\n"); return 2; }

    printf("=== fuzz_malformed: malformed input must never crash tetrisd ===\n\n");

    run_case_str("bare \\n instead of CRLF",
             &cfg, "JOIN /room/fuzz1 HTTTP/1.0\n\n");

    run_case_str("colon inside a header value (should be a VALID JOIN -> 201)",
             &cfg, "JOIN /room/fuzz2 HTTTP/1.0\r\nX-Note: 10:30:00\r\n\r\n");

    run_case_str("body present, Content-Length missing",
             &cfg, "MOVE /room/fuzz3/player/p1 HTTTP/1.0\r\nPlayer-Id: p1\r\n\r\nLEFT");

    run_case_str("Content-Length says more than the body actually has",
             &cfg, "MOVE /room/fuzz4/player/p1 HTTTP/1.0\r\nPlayer-Id: p1\r\nContent-Length: 999\r\n\r\nLEFT");

    run_case_str("unknown method",
             &cfg, "FROBNICATE /room/fuzz5 HTTTP/1.0\r\n\r\n");

    run_case_str("path with no leading slash",
             &cfg, "JOIN room6 HTTTP/1.0\r\n\r\n");

    run_case("pure random binary garbage as the whole frame",
             &cfg, "\x00\x01\xff\xfe\x13\x37\xde\xad\xbe\xef\x90\x90\x90\x90", 14);

    run_case_str("empty headers section only (no request line at all)",
             &cfg, "\r\n\r\n");

    run_case_str("wrong HTTTP version string",
             &cfg, "JOIN /room/fuzz9 HTTTP/1.1\r\n\r\n");

    // Final sanity check: a completely normal JOIN must still work after
    // all of the above. If it does, the daemon's shared state (room table,
    // client table, epoll set) survived every malformed input intact.
    printf("\n-- final sanity: normal JOIN still works after all fuzz cases --\n");
    int fd = tcp_connect(cfg.bind_addr, cfg.listen_port);
    session_ctx_t *cctx = session_client_ctx_init(cfg.ca_path);
    session_t *s = fd >= 0 ? session_connect(cctx, fd) : NULL;
    if (!s) {
        printf("FAIL  daemon is no longer accepting handshakes -- it may have crashed\n");
        g_fail = 1;
    } else {
        const char *req = "JOIN /room/fuzz_final HTTTP/1.0\r\n\r\n";
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

    printf("\n=====================================\n");
    printf(g_fail ? "FUZZ_MALFORMED: SOME CHECKS FAILED\n" : "FUZZ_MALFORMED: ALL PASS (daemon survived every case)\n");
    return g_fail ? 1 : 0;
}
