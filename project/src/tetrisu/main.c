// tetrisu/main.c — the network game client.
//
// Pipeline: TCP connect -> libtetrissh client handshake -> libhtttp
// request/response over the encrypted frame layer -> render server-pushed
// STATE frames. This is the client-side half of the same pipeline tetrisd
// runs on the server side (see src/tetrisd/main.c's file header comment).
//
// Usage: tetrisu [rc-file] [room-id]
//   rc-file  defaults to ".tetrishrc" -- we reuse the SAME config file
//            tetrisd reads, because it already has bind_addr/listen_port
//            (where to connect) and ca_path (who to trust). A real
//            deployment would give the client its own smaller config, but
//            for this project reusing tetrisd's is the simplest thing that
//            is still honest about where these values come from.
//   room-id  defaults to "lobby".
//
// Controls once connected: a/d move, w rotate, s soft drop, SPACE hard
// drop, g start the room (any seated player can), q leave and quit.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "rc.h"
#include "tetrissh.h"
#include "htttp.h"

static struct termios g_orig_term;
static int g_term_saved = 0;

static void restore_terminal(void) {
    if (g_term_saved) tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_term);
}

// Raw-ish mode: no line buffering, no echo, but VMIN=0/VTIME=0 so reads
// never block -- we multiplex stdin against the socket with select()
// instead, and use this only to fetch a keypress once select() says one
// is waiting.
static void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &g_orig_term) != 0) return;
    g_term_saved = 1;
    struct termios raw = g_orig_term;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    atexit(restore_terminal);
}

static int tcp_connect(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr, "tetrisu: bad host '%s'\n", host);
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

// Sends one HTTTP request over the session and blocks for exactly one
// response frame. Returns 0 and fills *out on success (out->body points
// into `resp_buf`, which the caller must keep alive while using *out).
static int do_request(tetrissh_session_t *sess, int fd,
                      htttp_method_t method, const char *path,
                      const char *player_id, const char *body,
                      htttp_msg_t *out, unsigned char **resp_buf) {
    htttp_builder_t b;
    htttp_builder_init_request(&b, method, path);
    if (player_id) htttp_builder_add_header(&b, "Player-Id", player_id);
    if (body) htttp_builder_set_body(&b, (const unsigned char *)body, strlen(body));

    size_t wlen = 0;
    char *wire = htttp_serialise(&b, &wlen);
    if (!wire) return -1;

    int rc = tetrissh_send(sess, fd, (unsigned char *)wire, wlen);
    free(wire);
    if (rc != 0) {
        fprintf(stderr, "tetrisu: send failed: %s\n", tetrissh_strerror(sess));
        return -1;
    }

    size_t plen = 0;
    unsigned char *plain = tetrissh_recv(sess, fd, &plen);
    if (!plain) {
        fprintf(stderr, "tetrisu: recv failed: %s\n", tetrissh_strerror(sess));
        return -1;
    }

    htttp_err_t err = htttp_parse_response((const char *)plain, plen, out);
    if (err != HTTTP_OK) {
        fprintf(stderr, "tetrisu: bad response (%s)\n", htttp_strerror(err));
        free(plain);
        return -1;
    }
    *resp_buf = plain;   // caller frees once done reading out->body
    return 0;
}

// Renders one server-pushed STATE frame (already plain text -- see
// tetrisd's render_board()) straight to the terminal. We reset the cursor
// to the top-left first so each frame overwrites the last one in place.
static void render_state(const htttp_msg_t *state) {
    printf("\033[H\033[J");   // cursor home + clear screen
    if (state->body && state->body_len > 0)
        fwrite(state->body, 1, state->body_len, stdout);
    printf("\n[a/d move  w rotate  s soft  SPACE hard  g start  q quit]\n");
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *rc_path = (argc > 1) ? argv[1] : ".tetrishrc";
    const char *room_id = (argc > 2) ? argv[2] : "lobby";

    Config cfg;
    if (rc_load(rc_path, &cfg) != 0) {
        fprintf(stderr, "tetrisu: failed to load configuration from %s\n", rc_path);
        return 1;
    }

    int fd = tcp_connect(cfg.bind_addr, cfg.listen_port);
    if (fd < 0) return 1;

    tetrissh_session_t *sess = tetrissh_session_alloc();
    if (!sess) { close(fd); return 1; }

    printf("tetrisu: connecting to %s:%d ...\n", cfg.bind_addr, cfg.listen_port);
    if (tetrissh_handshake_client(sess, fd, cfg.ca_path) != 0) {
        fprintf(stderr, "tetrisu: handshake failed: %s\n", tetrissh_strerror(sess));
        tetrissh_session_free(sess);
        close(fd);
        return 1;
    }
    printf("tetrisu: handshake OK (cert verified against %s)\n", cfg.ca_path);

    // --- JOIN: this is the full round trip the Week 7 deliverable asks
    // for -- tetrisu sends JOIN, tetrisd responds 200/201 with a Player-Id
    // header, and we hang on to that id for every request after this.
    char path[300];
    snprintf(path, sizeof path, "/room/%s", room_id);

    htttp_msg_t resp;
    unsigned char *resp_buf = NULL;
    if (do_request(sess, fd, HTTTP_METHOD_JOIN, path, NULL, NULL, &resp, &resp_buf) != 0) {
        tetrissh_session_free(sess);
        close(fd);
        return 1;
    }
    if (resp.status != HTTTP_200_OK && resp.status != HTTTP_201_CREATED) {
        fprintf(stderr, "tetrisu: JOIN failed: %d %s\n", (int)resp.status, resp.reason);
        free(resp_buf);
        tetrissh_session_free(sess);
        close(fd);
        return 1;
    }

    size_t pid_len = 0;
    const char *pid_ptr = htttp_find_header(&resp, "Player-Id", &pid_len);
    char player_id[32] = {0};
    if (!pid_ptr || pid_len == 0 || pid_len >= sizeof player_id) {
        fprintf(stderr, "tetrisu: JOIN succeeded but no usable Player-Id header\n");
        free(resp_buf);
        tetrissh_session_free(sess);
        close(fd);
        return 1;
    }
    memcpy(player_id, pid_ptr, pid_len);
    player_id[pid_len] = '\0';
    free(resp_buf);

    printf("tetrisu: JOIN -> %d, assigned Player-Id: %s\n", (int)resp.status, player_id);
    printf("tetrisu: press any key to start playing...\n");

    // --- interactive loop -------------------------------------------------
    enable_raw_mode();
    int quit = 0;

    while (!quit) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(fd, &rfds);
        int maxfd = fd > STDIN_FILENO ? fd : STDIN_FILENO;

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (FD_ISSET(fd, &rfds)) {
            // A frame arrived unprompted: on this connection, that's only
            // ever a server-pushed STATE broadcast (see tetrisd's ticker).
            size_t plen = 0;
            unsigned char *plain = tetrissh_recv(sess, fd, &plen);
            if (!plain) {
                fprintf(stderr, "\ntetrisu: connection lost: %s\n", tetrissh_strerror(sess));
                break;
            }
            htttp_msg_t state;
            if (htttp_parse_request((const char *)plain, plen, &state) == HTTTP_OK &&
                state.method == HTTTP_METHOD_STATE) {
                render_state(&state);
            }
            free(plain);
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char ch;
            if (read(STDIN_FILENO, &ch, 1) != 1) continue;

            htttp_method_t m = HTTTP_METHOD_UNKNOWN;
            const char *body = NULL;

            switch (ch) {
                case 'a': m = HTTTP_METHOD_MOVE;   body = "LEFT";  break;
                case 'd': m = HTTTP_METHOD_MOVE;   body = "RIGHT"; break;
                case 'w': m = HTTTP_METHOD_ROTATE; body = "CW";    break;
                case 's': m = HTTTP_METHOD_DROP;   body = "SOFT";  break;
                case ' ': m = HTTTP_METHOD_DROP;   body = "HARD";  break;
                case 'g': m = HTTTP_METHOD_START;  body = NULL;    break;
                case 'q': quit = 1; break;
                default: break;
            }

            if (quit) {
                htttp_msg_t leave_resp; unsigned char *leave_buf = NULL;
                do_request(sess, fd, HTTTP_METHOD_LEAVE, path, player_id, NULL, &leave_resp, &leave_buf);
                free(leave_buf);
                break;
            }
            if (m == HTTTP_METHOD_UNKNOWN) continue;

            htttp_msg_t r; unsigned char *rbuf = NULL;
            if (do_request(sess, fd, m, path, player_id, body, &r, &rbuf) != 0) break;
            if (r.status >= 400)
                fprintf(stderr, "\ntetrisu: %s -> %d %s\n", htttp_method_str(m), (int)r.status, r.reason);
            free(rbuf);
        }
    }

    restore_terminal();
    printf("tetrisu: disconnected.\n");
    tetrissh_session_free(sess);
    close(fd);
    return 0;
}
