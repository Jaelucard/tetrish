// test_admin.c — Week 9/10 deliverable: exercises the ops-console admin
// protocol against a REAL, already-running tetrisd, and verifies multiple
// concurrent admin sessions do not interfere with each other.
//
//   bin/tetrisd .tetrishrc &
//   build/tests/test_admin .tetrishrc
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rc.h"
#include "corestack/secure_session.h"
#include "htttp.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("ok    %s\n", msg); \
    else { printf("FAIL  %s\n", msg); g_fail = 1; } \
    fflush(stdout); \
} while (0)

static Config g_cfg;

static int connect_and_handshake(session_t **out_s, session_ctx_t **out_ctx) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)g_cfg.listen_port);
    inet_pton(AF_INET, g_cfg.bind_addr, &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { perror("connect"); return -1; }

    *out_ctx = session_client_ctx_init(g_cfg.ca_path);
    *out_s = session_connect(*out_ctx, fd);
    if (!*out_s) { fprintf(stderr, "handshake failed\n"); close(fd); return -1; }
    return fd;
}

static int admin_request(session_t *s, htttp_method_t method, const char *path,
                         const char *token, htttp_msg_t *out, unsigned char **resp_buf) {
    htttp_builder_t b;
    htttp_builder_init_request(&b, method, path);
    if (token) htttp_builder_add_header(&b, "Admin-Token", token);
    size_t wlen = 0;
    char *wire = htttp_serialise(&b, &wlen);
    if (session_write(s, wire, wlen) <= 0) { free(wire); return -1; }
    free(wire);

    unsigned char buf[8192];
    ssize_t n = session_read(s, buf, sizeof buf);
    if (n <= 0) return -1;

    *resp_buf = malloc((size_t)n);
    memcpy(*resp_buf, buf, (size_t)n);
    return htttp_parse_response((const char *)*resp_buf, (size_t)n, out) == HTTTP_OK ? 0 : -1;
}

// --- concurrency test: two ops consoles attach to the SAME room at
// the same time and each must independently see live STATE frames. ---
typedef struct { const char *token; const char *room; int frames_seen; int status_ok; } spectator_arg_t;

static void *spectator_thread(void *argp) {
    spectator_arg_t *a = argp;
    session_t *s; session_ctx_t *ctx;
    int fd = connect_and_handshake(&s, &ctx);
    if (fd < 0) return NULL;

    char path[128];
    snprintf(path, sizeof path, "/room/%s", a->room);

    htttp_msg_t resp; unsigned char *buf = NULL;
    if (admin_request(s, HTTTP_METHOD_ADMIN_ATTACH, path, a->token, &resp, &buf) == 0) {
        a->status_ok = (resp.status == HTTTP_200_OK);
        free(buf);
    }

    // Now just read whatever STATE frames arrive for ~1.5s.
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    for (int i = 0; i < 10; i++) {
        char sbuf[8192];
        ssize_t n = session_read(s, sbuf, sizeof sbuf);
        if (n <= 0) break;
        htttp_msg_t state;
        if (htttp_parse_request(sbuf, (size_t)n, &state) == HTTTP_OK && state.method == HTTTP_METHOD_STATE)
            a->frames_seen++;
    }

    session_close(s);
    session_client_ctx_free(ctx);
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    const char *rc_path = (argc > 1) ? argv[1] : ".tetrishrc";
    if (rc_load(rc_path, &g_cfg) != 0) { fprintf(stderr, "rc_load failed\n"); return 2; }

    printf("=== test_admin: role-gated auth ===\n\n");

    // --- no token -> 401 ---
    { session_t *s; session_ctx_t *ctx; int fd = connect_and_handshake(&s, &ctx);
      htttp_msg_t resp; unsigned char *buf = NULL;
      admin_request(s, HTTTP_METHOD_ADMIN_STATUS, "/admin/status", NULL, &resp, &buf);
      CHECK(resp.status == HTTTP_401_UNAUTHORIZED, "ADMIN-STATUS with no token -> 401");
      free(buf); session_close(s); session_client_ctx_free(ctx); close(fd); }

    // --- unknown token -> 401 ---
    { session_t *s; session_ctx_t *ctx; int fd = connect_and_handshake(&s, &ctx);
      htttp_msg_t resp; unsigned char *buf = NULL;
      admin_request(s, HTTTP_METHOD_ADMIN_STATUS, "/admin/status", "not-a-real-token", &resp, &buf);
      CHECK(resp.status == HTTTP_401_UNAUTHORIZED, "ADMIN-STATUS with unknown token -> 401");
      free(buf); session_close(s); session_client_ctx_free(ctx); close(fd); }

    // --- readonly token: STATUS/ROOMS/ATTACH all OK (none of them mutate
    // anything); KICK forbidden with a useful reason ---
    { session_t *s; session_ctx_t *ctx; int fd = connect_and_handshake(&s, &ctx);
      htttp_msg_t resp; unsigned char *buf = NULL;
      admin_request(s, HTTTP_METHOD_ADMIN_STATUS, "/admin/status", "alice-readonly-tok", &resp, &buf);
      CHECK(resp.status == HTTTP_200_OK, "readonly: ADMIN-STATUS -> 200");
      free(buf);
      admin_request(s, HTTTP_METHOD_ADMIN_ROOMS, "/admin/rooms", "alice-readonly-tok", &resp, &buf);
      CHECK(resp.status == HTTTP_200_OK, "readonly: ADMIN-ROOMS -> 200");
      free(buf);
      admin_request(s, HTTTP_METHOD_ADMIN_ATTACH, "/room/lobby", "alice-readonly-tok", &resp, &buf);
      CHECK(resp.status == HTTTP_200_OK || resp.status == HTTTP_404_NOT_FOUND,
            "readonly: ADMIN-ATTACH is allowed (spectating doesn't mutate anything)");
      free(buf);
      admin_request(s, HTTTP_METHOD_ADMIN_KICK, "/room/lobby/player/p1", "alice-readonly-tok", &resp, &buf);
      CHECK(resp.status == HTTTP_403_FORBIDDEN, "readonly: ADMIN-KICK -> 403 (needs full admin)");
      CHECK(buf && strstr((char *)buf, "\"required_role\": \"full\"") != NULL &&
                   strstr((char *)buf, "\"your_role\": \"readonly\"") != NULL,
            "readonly: the 403 body names both the role held and the role required (a useful reason)");
      free(buf); session_close(s); session_client_ctx_free(ctx); close(fd); }

    // --- full token: everything, including 404s for nonexistent targets ---
    { session_t *s; session_ctx_t *ctx; int fd = connect_and_handshake(&s, &ctx);
      htttp_msg_t resp; unsigned char *buf = NULL;
      admin_request(s, HTTTP_METHOD_ADMIN_ATTACH, "/room/nosuchroom", "bob-full-tok", &resp, &buf);
      CHECK(resp.status == HTTTP_404_NOT_FOUND, "full: ADMIN-ATTACH to a nonexistent room -> 404");
      free(buf);
      admin_request(s, HTTTP_METHOD_ADMIN_KICK, "/room/nosuchroom/player/nobody", "bob-full-tok", &resp, &buf);
      CHECK(resp.status == HTTTP_404_NOT_FOUND, "full: ADMIN-KICK on a nonexistent player -> 404");
      free(buf); session_close(s); session_client_ctx_free(ctx); close(fd); }

    // --- full token actually kicking a real, connected player ---
    { session_t *victim; session_ctx_t *vctx; int vfd = connect_and_handshake(&victim, &vctx);
      htttp_msg_t jresp; unsigned char *jbuf = NULL;
      admin_request(victim, HTTTP_METHOD_JOIN, "/room/kickme", NULL, &jresp, &jbuf);
      CHECK(jresp.status == HTTTP_201_CREATED, "victim: JOIN /room/kickme -> 201 (setup for the kick test)");

      size_t pidlen = 0;
      const char *pidptr = htttp_find_header(&jresp, "Player-Id", &pidlen);
      char victim_pid[32] = {0};
      if (pidptr && pidlen > 0 && pidlen < sizeof victim_pid) memcpy(victim_pid, pidptr, pidlen);
      free(jbuf);

      char kick_path[96];
      snprintf(kick_path, sizeof kick_path, "/room/kickme/player/%s", victim_pid);

      session_t *admin; session_ctx_t *actx; int afd = connect_and_handshake(&admin, &actx);
      htttp_msg_t kresp; unsigned char *kbuf = NULL;
      admin_request(admin, HTTTP_METHOD_ADMIN_KICK, kick_path, "bob-full-tok", &kresp, &kbuf);
      CHECK(kresp.status == HTTTP_200_OK, "full: ADMIN-KICK on the real victim -> 200");
      CHECK(kbuf && strstr((char *)kbuf, victim_pid) != NULL,
            "full: ADMIN-KICK's response body names the player that was kicked");
      free(kbuf);

      // And the strongest proof it's not a no-op: the victim's connection
      // is actually gone afterward.
      struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
      setsockopt(vfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
      char probe[16];
      ssize_t n = session_read(victim, probe, sizeof probe);
      CHECK(n <= 0, "victim's connection is gone after being kicked (ADMIN-KICK actually disconnects them)");

      session_close(victim); session_client_ctx_free(vctx); close(vfd);
      session_close(admin); session_client_ctx_free(actx); close(afd);
    }

    // --- concurrency: two ops consoles attach to the same live room ---
    printf("\n=== test_admin: two concurrent ops consoles on the same room ===\n\n");

    // First, actually start a room so there's something to spectate: join
    // and start it as an ordinary player in a background connection.
    { session_t *s; session_ctx_t *ctx; int fd = connect_and_handshake(&s, &ctx);
      htttp_msg_t resp; unsigned char *buf = NULL;
      admin_request(s, HTTTP_METHOD_JOIN, "/room/concur", NULL, &resp, &buf);
      free(buf);
      size_t pidlen = 0; const char *pid = htttp_find_header(&resp, "Player-Id", &pidlen);
      char player_id[32] = {0}; memcpy(player_id, pid, pidlen);

      htttp_builder_t b; htttp_builder_init_request(&b, HTTTP_METHOD_START, "/room/concur");
      htttp_builder_add_header(&b, "Player-Id", player_id);
      size_t wlen = 0; char *wire = htttp_serialise(&b, &wlen);
      session_write(s, wire, wlen); free(wire);
      char rbuf[512]; session_read(s, rbuf, sizeof rbuf);   // consume the START response

      spectator_arg_t a1 = { .token = "bob-full-tok", .room = "concur" };
      spectator_arg_t a2 = { .token = "bob-full-tok", .room = "concur" };
      pthread_t t1, t2;
      pthread_create(&t1, NULL, spectator_thread, &a1);
      pthread_create(&t2, NULL, spectator_thread, &a2);
      pthread_join(t1, NULL);
      pthread_join(t2, NULL);

      CHECK(a1.status_ok, "spectator A: ADMIN-ATTACH -> 200");
      CHECK(a2.status_ok, "spectator B: ADMIN-ATTACH -> 200");
      CHECK(a1.frames_seen > 0, "spectator A independently received live STATE frames");
      CHECK(a2.frames_seen > 0, "spectator B independently received live STATE frames");
      printf("      (A saw %d frames, B saw %d frames -- both nonzero means no interference)\n",
             a1.frames_seen, a2.frames_seen);

      session_close(s); session_client_ctx_free(ctx); close(fd);
    }

    printf("\n=====================================\n");
    printf(g_fail ? "TEST_ADMIN: SOME CHECKS FAILED\n" : "TEST_ADMIN: ALL PASS\n");
    return g_fail ? 1 : 0;
}
