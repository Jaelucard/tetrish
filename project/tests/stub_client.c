// stub_client is our Week 6 integration test harness for the whole tetrisd
// pipeline, end to end.
//
// It walks through the full flow a real client would use. First it makes a
// TCP connection and completes the libtetrissh client handshake, which
// verifies the server's certificate against our CA. Then it sends JOIN and
// receives the Player-Id that the server assigns us. Then it sends START,
// which arms the room's ticker, and it watches STATE frames arrive at
// roughly tick_hz. Partway through, it sends a MOVE and checks that the
// server acknowledges it with a 200 status. Finally it sends LEAVE.
//
// Usage: stub_client <rc-file> [room-id] [seconds]
// Exit code: 0 means every check passed, 1 means a step failed.
//
// One thing worth understanding up front is the interleaving rule: after we
// send a request, the next frames we read back are not guaranteed to be the
// response to that request. The server can also push STATE frames to us at
// any time. So we check the start of each frame: if it starts with
// "HTTTP/", it is a RESPONSE, and since we only ever have one request
// outstanding at a time, we know it is the response to that request.
// Anything else is a server-originated STATE request. The function
// read_until_response() below is where this rule is implemented.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "rc.h"
#include "tetrissh.h"
#include "htttp.h"

static tetrissh_session_t *sess;
static int sockfd = -1;
static long state_frames = 0;      // This counts every STATE broadcast we see, no matter which part of the program is reading frames at the time.

// This function keeps reading frames from the server until it finds a
// RESPONSE frame, counting any STATE frames it passes along the way. It
// returns the parsed status code from that response, or -1 if the
// connection breaks or a frame fails to parse.
static int read_until_response(htttp_msg_t *msg, char *keep, size_t keep_sz){
    for (;;){
        size_t plen = 0;
        unsigned char *plain = tetrissh_recv(sess, sockfd, &plen);
        if (plain == NULL) return -1;

        if (plen >= 6 && memcmp(plain, "HTTTP/", 6) == 0){       // The frame starts with "HTTTP/", so this is a response.
            if (plen >= keep_sz) plen = keep_sz - 1;
            memcpy(keep, plain, plen);                // We copy the bytes into keep because msg's fields will point into this buffer, so keep has to stay alive while we use msg.
            free(plain);
            if (htttp_parse_response(keep, plen, msg) != HTTTP_OK) return -1;
            return (int)msg->status;
        }
        // If it was not a response, it must be a server-originated STATE request.
        htttp_msg_t st;
        if (htttp_parse_request((const char *)plain, plen, &st) == HTTTP_OK &&
            st.method == HTTTP_METHOD_STATE)
            state_frames++;
        free(plain);
    }
}

static int send_request(htttp_method_t m, const char *path,
                        const char *player_id, const char *body){
    htttp_builder_t b;
    htttp_builder_init_request(&b, m, path);
    if (player_id) htttp_builder_add_header(&b, "Player-Id", player_id);
    if (body){
        htttp_builder_add_header(&b, "Content-Type", "application/tetris-command");
        htttp_builder_set_body(&b, (const unsigned char *)body, strlen(body));
    }
    size_t wlen = 0;
    char *wire = htttp_serialise(&b, &wlen);
    if (wire == NULL) return -1;
    int rc = tetrissh_send(sess, sockfd, (unsigned char *)wire, wlen);
    free(wire);
    return rc;
}

int main(int argc, char **argv){
    if (argc < 2){
        fprintf(stderr, "usage: %s <rc-file> [room-id] [seconds]\n", argv[0]);
        return 2;
    }
    const char *room    = (argc > 2) ? argv[2] : "demo";
    int         seconds = (argc > 3) ? atoi(argv[3]) : 3;

    Config cfg;
    if (rc_load(argv[1], &cfg) != 0) return 1;

    // Here we open a TCP connection to tetrisd.
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)cfg.listen_port);
    inet_pton(AF_INET, cfg.bind_addr, &sa.sin_addr);
    if (connect(sockfd, (struct sockaddr *)&sa, sizeof sa) < 0){
        perror("connect"); return 1;
    }

    // Now we run the client side of the handshake, which checks the server's certificate against our CA.
    sess = tetrissh_session_alloc();
    if (tetrissh_handshake_client(sess, sockfd, cfg.ca_path) != 0){
        fprintf(stderr, "FAIL handshake: %s\n", tetrissh_strerror(sess));
        return 1;
    }
    printf("ok  handshake (cert verified against %s)\n", cfg.ca_path);

    char path[64], keep[4096];
    htttp_msg_t msg;

    // We send JOIN and expect either a 200 or 201 status, along with a Player-Id header that the server assigns to us.
    snprintf(path, sizeof path, "/room/%s", room);
    if (send_request(HTTTP_METHOD_JOIN, path, NULL, NULL) != 0) return 1;
    int st = read_until_response(&msg, keep, sizeof keep);
    if (st != 200 && st != 201){ fprintf(stderr, "FAIL JOIN -> %d\n", st); return 1; }

    char player_id[32] = "";
    size_t vlen = 0;
    const char *v = htttp_find_header(&msg, "Player-Id", &vlen);
    if (v == NULL || vlen == 0 || vlen >= sizeof player_id){
        fprintf(stderr, "FAIL JOIN: no Player-Id issued\n"); return 1;
    }
    memcpy(player_id, v, vlen);
    player_id[vlen] = '\0';
    printf("ok  JOIN -> %d, issued Player-Id: %s\n", st, player_id);

    // We send START and expect a 200 status. This arms the room's ticker.
    if (send_request(HTTTP_METHOD_START, path, player_id, NULL) != 0) return 1;
    st = read_until_response(&msg, keep, sizeof keep);
    if (st != 200){ fprintf(stderr, "FAIL START -> %d\n", st); return 1; }
    printf("ok  START -> 200\n");

    // Now we receive STATE frames for the given number of seconds, and partway through we send a MOVE.
    // We use CLOCK_MONOTONIC here instead of time(), because time() only has
    // whole-second granularity. That would make our measured frame rate,
    // and therefore whether the test passes or fails, unreliable.
    long before = state_frames;
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    int move_status = 0;
    char ppath[96];
    snprintf(ppath, sizeof ppath, "/room/%s/player/%s", room, player_id);

    for (;;){
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        double elapsed = (double)(ts1.tv_sec - ts0.tv_sec) +
                         (double)(ts1.tv_nsec - ts0.tv_nsec) / 1e9;
        if (elapsed >= (double)seconds) break;
        size_t plen = 0;
        unsigned char *plain = tetrissh_recv(sess, sockfd, &plen);
        if (plain == NULL){ fprintf(stderr, "FAIL: stream died mid-game\n"); return 1; }
        if (plen >= 6 && memcmp(plain, "HTTTP/", 6) == 0){
            htttp_msg_t resp;
            char keep2[4096];
            size_t klen = plen < sizeof keep2 ? plen : sizeof keep2 - 1;
            memcpy(keep2, plain, klen);
            if (htttp_parse_response(keep2, klen, &resp) == HTTTP_OK)
                move_status = (int)resp.status;              // This is the server's acknowledgement of our MOVE request.
        } else {
            htttp_msg_t streq;
            if (htttp_parse_request((const char *)plain, plen, &streq) == HTTTP_OK &&
                streq.method == HTTTP_METHOD_STATE){
                state_frames++;
                if (state_frames == before + 5)              // We wait until we are partway through the stream before sending our input.
                    send_request(HTTTP_METHOD_MOVE, ppath, player_id, "LEFT");
            }
        }
        free(plain);
    }

    clock_gettime(CLOCK_MONOTONIC, &ts1);
    double elapsed = (double)(ts1.tv_sec - ts0.tv_sec) +
                     (double)(ts1.tv_nsec - ts0.tv_nsec) / 1e9;
    long got = state_frames - before;
    long expected = (long)((double)cfg.tick_hz * elapsed);
    double hz = (double)got / elapsed;
    printf("ok  STATE stream: %ld frames in %.2fs (%.1f Hz, expected %d Hz)\n",
           got, elapsed, hz, cfg.tick_hz);
    printf("%s MOVE ack -> %d\n", move_status == 200 ? "ok " : "FAIL", move_status);

    // We send LEAVE and expect a 200 status. This also exercises the server's room destruction logic.
    if (send_request(HTTTP_METHOD_LEAVE, path, player_id, NULL) != 0) return 1;
    st = read_until_response(&msg, keep, sizeof keep);
    printf("%s LEAVE -> %d\n", st == 200 ? "ok " : "FAIL", st);

    tetrissh_close(sess);
    tetrissh_session_free(sess);
    close(sockfd);

    // Here we check the frame rate: we allow it to be within 25% of tick_hz, to leave some room for scheduling jitter.
    int pass = got > expected * 3 / 4 && got < expected * 5 / 4 &&
               move_status == 200 && st == 200;
    printf("%s\n", pass ? "STUB CLIENT: ALL PASS" : "STUB CLIENT: FAIL");
    return pass ? 0 : 1;
}
