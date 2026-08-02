// tetrisd is the game server daemon.
//
// The overall shape of this file: the epoll loop now runs the full pipeline for
// a game client, which is:
//
//   accept -> libtetrissh handshake -> [4-byte length][AES frame] -> decrypt ->
//   libhtttp parse -> dispatch (JOIN, LEAVE, START, MOVE, ROTATE, DROP) ->
//   rooms + libtetrisbrain -> build the response -> encrypt -> send
//
// On top of that, each room that has started has a timerfd firing at tick_hz.
// Every time it fires we advance that room's games by one tick and send a
// STATE frame to the players.
//
// The epoll set now watches six kinds of file descriptor:
//   listen_fd   TCP            new game clients (their handshake runs inline, see below)
//   client fds  TCP            encrypted HTTTP frames from players who finished the handshake
//   timer fds   timerfd        one per started room, firing at tick_hz
//   sig_fd      signalfd       SIGTERM, SIGHUP, SIGUSR1
//   ctl_fd      AF_UNIX STREAM the control plane, which now speaks plaintext HTTTP
//   mq          POSIX mqueue   the Battle Royale garbage channel (still a stub)
//
// A note on the inline handshake, which is worth being able to explain.
// The handshake runs inline in the accept handler, on a blocking socket that is
// bounded by 5 second send and receive timeouts. This is a deliberate choice:
//   1. libtetrissh's receive and send helpers use MSG_WAITALL loops, which
//      assume a blocking socket. A non-blocking fd would fail partway through
//      the handshake with EAGAIN.
//   2. At our scale (a few tens of clients, and an RSA handshake on the local
//      network that takes a few milliseconds) the pause in the loop is not
//      noticeable.
//   3. A malicious client that handshakes very slowly can only stall the loop
//      for up to 5 seconds because of the timeouts, and then it is dropped.
// A production server would move handshakes onto a small pool of threads, or
// use a non-blocking state machine. At this project's scale, doing it inline
// with a timeout is the simple and honest baseline. The same reasoning applies
// to connected clients: epoll tells us a frame has started arriving, and the
// bounded blocking read then finishes it (or times the client out).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>          // slog(level, fmt, ...)
#include <ctype.h>           // isalnum for room-id validation
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>           // O_* flags for mq_open
#include <pthread.h>
#include <stdatomic.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>     // per-room tickers
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>            // clock_gettime, struct timespec (uptime)
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <mqueue.h>
#include "rc.h"
#include "daemon.h"
#include "ring.h"
#include "tetrissh.h"        // NET teammate: secure session
#include "htttp.h"           // NET teammate: HTTTP parse/serialise
#include "clients.h"
#include "rooms.h"
#include "garbage.h"         // the Battle Royale garbage event format

// The garbage event format lives in garbage.h so that test tools can build a
// well-formed message without duplicating the struct.

// A few daemon-wide singletons. A daemon has exactly one config, one epoll
// set and one log ring
static Config g_cfg;
static int    g_ep = -1;

// When the daemon started, used only to answer "uptime" on the control plane.
// Redis uses time(NULL) which is the wall clock. if NTP steps the clock backwards, then redis would report negative uptime
// Instead I will use CLOCK_MONOTONIC which would count from an arbitary boot-relative origin and cannot go backwards
// This is more consistent with the rest of the daemon and it is already used for the room ticker
// TLDR: I am diverging from redis here because monotonic clocks cannot be stepped (cannot make sudden jumps forward or backward)
// compared to redis which uses wall clock time which can be stepped by NTP or other system adjustments
static struct timespec g_started;

// --- Battle Royale garbage channel -----------------------------------------
// The queue descriptor is a singleton like the others, because the tick handler
// has to reach it to send and main() has to reach it to receive.
static mqd_t g_mq = (mqd_t)-1;

// Accounting for the garbage channel. Same discipline as the log ring's two drop
// counters: the increment lives on the same line as the event, so you cannot
// send or drop without counting it.
//   sent      the event was handed to the queue successfully
//   dropped   the queue was full and we threw the event away. See handle_garbage.
static atomic_ulong garbage_sent;
static atomic_ulong garbage_dropped;

// A tiny xorshift32, used only to pick a target room and a hole column. This is
// deliberately NOT libtetrisbrain's PRNG. The engine's randomness is part of
// replayable game state and must not be perturbed by network events. This one
// only affects targeting, which the log records after the fact.
static uint32_t g_rng = 2463534242u;
static uint32_t rng_next(void){
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

// --- logging: ring buffer + logshipper thread ------------------------------
static int log_fd = -1;
static struct sockaddr_un log_addr;
static Ring g_ring;
static atomic_ulong dropped_send;
static atomic_int   shipper_stop;

static void log_init(const char *path){
    log_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    memset(&log_addr, 0, sizeof log_addr);
    log_addr.sun_family = AF_UNIX;
    strncpy(log_addr.sun_path, path, sizeof log_addr.sun_path - 1);
}

// A small logging helper that adds a level tag (like "info" or "warn") and
// formats the message like printf does. tetrislogd adds the timestamp when the
// record arrives. This keeps the same non-blocking promise as before: it pushes
// the record into the ring, drops it if the ring is busy, and never waits.
// (It is called slog, not logf, because logf() is already the name of the C
// math library function for the logarithm of a float.)
static void slog(const char *level, const char *fmt, ...){
    char buf[RING_REC_MAX];
    int off = snprintf(buf, sizeof buf, "[%s] ", level);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + off, sizeof buf - (size_t)off, fmt, ap);
    va_end(ap);
    ring_push(&g_ring, buf, strlen(buf));
}

#define SHIP_BATCH 64
static void *logshipper(void *arg){
    (void)arg;
    static char batch[SHIP_BATCH][RING_REC_MAX];
    size_t lens[SHIP_BATCH];
    for (;;){
        size_t n = ring_pop_batch(&g_ring, batch, lens, SHIP_BATCH);
        if (n == 0){
            if (atomic_load(&shipper_stop))
                break;
            usleep(1000);
            continue;
        }
        for (size_t i = 0; i < n; i++){
            if (sendto(log_fd, batch[i], lens[i], 0,
                       (struct sockaddr *)&log_addr, sizeof log_addr) < 0)
                atomic_fetch_add(&dropped_send, 1);
        }
    }
    return NULL;
}

static unsigned long total_dropped(void){
    return ring_dropped(&g_ring) + atomic_load(&dropped_send);
}

// a function to check how many seconds since the daemon started
// start off with making a blank timespec to hold a time
// then call clock_gettime with CLOCK_MONOTONIC to get the current time
// then return the difference between the current time and the start time in seconds
static long uptime_seconds(void){
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - g_started.tv_sec);
}

// --- listeners --------------------------------------------------------------

static int tcp_listen(const Config *cfg){
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0){ perror("socket"); return -1; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)cfg->listen_port);
    if (inet_pton(AF_INET, cfg->bind_addr, &sa.sin_addr) != 1){
        fprintf(stderr, "tetrisd: bad bind address '%s'\n", cfg->bind_addr);
        close(fd); return -1;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0){ perror("bind"); close(fd); return -1; }
    if (listen(fd, cfg->tcp_backlog) < 0){ perror("listen"); close(fd); return -1; }
    return fd;
}

static int ctl_listen(const Config *cfg){
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0){ perror("socket(ctl)"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(cfg->ctl_path) >= sizeof addr.sun_path){
        fprintf(stderr, "tetrisd: ctl_path too long: %s\n", cfg->ctl_path);
        close(fd); return -1;
    }
    strncpy(addr.sun_path, cfg->ctl_path, sizeof addr.sun_path - 1);

    unlink(cfg->ctl_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0){
        perror("bind(ctl)"); close(fd); return -1;
    }
    if (listen(fd, 8) < 0){ perror("listen(ctl)"); close(fd); return -1; }
    return fd;
}

static int write_pidfile(const char *path){
    FILE *f = fopen(path, "w");
    if (f == NULL){
        fprintf(stderr, "tetrisd: cannot write pid file '%s': %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
    return 0;
}

static int ep_add(int ep, int fd){
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events  = EPOLLIN;
    ev.data.fd = fd;
    return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

// --- responses ---------------------------------------------------------------

// Build an HTTTP response, encrypt it, and send it. If pid_hdr is given, we add
// a Player-Id header (we use this on JOIN, where the server hands the client
// its identity). The function returns the status code so the caller can log the
// request. If the send fails we log it but do not treat it as fatal here,
// because the client's next event will error out and disconnect it anyway.
static int send_response(client_t *c, htttp_status_t status,
                         const char *body, const char *pid_hdr){
    htttp_builder_t b;
    htttp_builder_init_response(&b, status);
    htttp_builder_add_header(&b, "Content-Type", "application/json");
    if (pid_hdr)
        htttp_builder_add_header(&b, "Player-Id", pid_hdr);
    if (body)
        htttp_builder_set_body(&b, (const unsigned char *)body, strlen(body));

    size_t wlen = 0;
    char *wire = htttp_serialise(&b, &wlen);      // malloc'd; adds Date header
    if (wire == NULL)
        return 500;
    if (tetrissh_send(c->sess, c->fd, (unsigned char *)wire, wlen) != 0)
        slog("warn", "%s: response send failed (%s)", c->player_id, tetrissh_strerror(c->sess));
    free(wire);
    return (int)status;
}

// --- request path parsing -----------------------------------------------------

// Parse the path of a request. If it looks like "/room/<id>" we return 0 and
// fill in rid. If it looks like "/room/<id>/player/<p>" we return 1 and fill in
// both rid and pid. For anything else we return -1.
// We only allow letters, digits, dashes and underscores in a room id. Because
// the id later ends up inside JSON and inside log lines, we check the allowed
// characters here at the edge, rather than trying to escape it later.
static int parse_room_path(const char *path, char *rid, size_t rsz,
                           char *pid, size_t psz){
    if (strncmp(path, "/room/", 6) != 0) return -1;
    const char *p = path + 6;
    const char *slash = strchr(p, '/');
    size_t idlen = slash ? (size_t)(slash - p) : strlen(p);
    if (idlen == 0 || idlen >= rsz) return -1;
    for (size_t i = 0; i < idlen; i++){
        char ch = p[i];
        if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_') return -1;
    }
    memcpy(rid, p, idlen);
    rid[idlen] = '\0';
    if (slash == NULL) return 0;

    if (strncmp(slash, "/player/", 8) != 0) return -1;
    const char *q = slash + 8;
    size_t plen = strlen(q);
    if (plen == 0 || plen >= psz || strchr(q, '/')) return -1;
    memcpy(pid, q, plen);
    pid[plen] = '\0';
    return 1;
}

// Check the caller's identity. After JOIN, every request must carry a Player-Id
// header that matches the identity the server gave this connection. If the
// header is missing we return 401. If it is present but does not match, which
// would mean the client is trying to control someone else's board, we return 403.
static int check_player_id(client_t *c, const htttp_msg_t *msg){
    size_t vlen = 0;
    const char *v = htttp_find_header(msg, "Player-Id", &vlen);
    if (v == NULL) return 401;
    if (vlen != strlen(c->player_id) || strncmp(v, c->player_id, vlen) != 0)
        return 403;
    return 0;
}

// --- room lifecycle handlers ---------------------------------------------------

// Declared ahead of time because disconnecting a client and cleaning up a room
// each need to call into the other.
static void disconnect_client(int fd, const char *reason);

static int do_join(client_t *c, const char *rid){
    if (c->room != -1)
        return send_response(c, HTTTP_409_CONFLICT, "{\"error\": \"already in a room\"}", NULL);

    int status = 0;
    room_t *r = room_join(rid, c, &status);
    if (r == NULL)
        return send_response(c, (htttp_status_t)status,
                             status == 409 ? "{\"error\": \"room full or already started\"}"
                                           : "{\"error\": \"room table full\"}", NULL);

    if (status == 201)
        slog("info", "room %s created by %s", r->id, c->player_id);
    slog("info", "room %s: %s joined (%d player%s)", r->id, c->player_id,
         r->nplayers, r->nplayers == 1 ? "" : "s");

    char body[128];
    snprintf(body, sizeof body, "{\"room\": \"%s\", \"player\": \"%s\", \"players\": %d}",
             r->id, c->player_id, r->nplayers);
    // The response to JOIN carries the assigned identity in a Player-Id header.
    // This is the moment where the client first learns which player it is.
    return send_response(c, (htttp_status_t)status, body, c->player_id);
}

static int do_leave(client_t *c, const char *rid){
    room_t *r = room_at(c->room);
    if (r == NULL || strcmp(r->id, rid) != 0)
        return send_response(c, HTTTP_404_NOT_FOUND, "{\"error\": \"not in that room\"}", NULL);

    char room_id[ROOM_ID_MAX];
    snprintf(room_id, sizeof room_id, "%s", r->id);
    int orphan_tfd = room_leave(c);
    if (orphan_tfd >= 0){                     // room died with this leave
        epoll_ctl(g_ep, EPOLL_CTL_DEL, orphan_tfd, NULL);
        close(orphan_tfd);
    }
    if (orphan_tfd != -1 || rooms_count() >= 0)   // always log the leave
        slog("info", "room %s: %s left%s", room_id, c->player_id,
             orphan_tfd >= 0 ? " (room destroyed)" : "");
    return send_response(c, HTTTP_200_OK, "{\"ok\": true}", NULL);
}

static int do_start(client_t *c, const char *rid){
    room_t *r = room_at(c->room);
    if (r == NULL || strcmp(r->id, rid) != 0)
        return send_response(c, HTTTP_403_FORBIDDEN, "{\"error\": \"not in that room\"}", NULL);
    if (r->started)
        return send_response(c, HTTTP_409_CONFLICT, "{\"error\": \"already started\"}", NULL);

    // Set up one authoritative game for each seated player, each with its own
    // random seed. We write the seed to the log on purpose, because the replay
    // feature later on needs the seed together with the inputs to rebuild the
    // exact same game.
    static uint32_t seed_counter = 0;
    pthread_mutex_lock(&r->mu);
    for (int i = 0; i < ROOM_MAX_PLAYERS; i++){
        if (r->players[i] == NULL) continue;
        uint32_t seed = (uint32_t)getpid() * 2654435761u + ++seed_counter;
        tb_init(&r->games[i], seed);
        r->pending[i] = TB_INPUT_NONE;
        slog("info", "room %s: SEED seat=%d player=%s seed=%u",
             r->id, i, r->players[i]->player_id, seed);
    }
    r->started = 1;
    r->ticks   = 0;
    pthread_mutex_unlock(&r->mu);

    // Set up the room's ticker. It is a timerfd that fires at tick_hz, and the
    // epoll loop owns it. There is no separate ticker thread, because a tick is
    // just another readable file descriptor in the same loop.
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0){
        slog("error", "room %s: timerfd_create failed: %s", r->id, strerror(errno));
        r->started = 0;
        return send_response(c, HTTTP_500_INTERNAL_ERROR, "{\"error\": \"timer\"}", NULL);
    }
    long period_ns = 1000000000L / (g_cfg.tick_hz > 0 ? g_cfg.tick_hz : 20);
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_interval.tv_sec  = period_ns / 1000000000L;
    its.it_interval.tv_nsec = period_ns % 1000000000L;
    its.it_value = its.it_interval;           // first tick one period from now
    timerfd_settime(tfd, 0, &its, NULL);
    r->timer_fd = tfd;
    ep_add(g_ep, tfd);

    slog("info", "room %s: STARTED by %s (%d players, %d Hz)",
         r->id, c->player_id, r->nplayers, g_cfg.tick_hz);
    return send_response(c, HTTTP_200_OK, "{\"ok\": true}", NULL);
}

// Map a MOVE/ROTATE/DROP body onto a tetrisbrain input. -1 = invalid body.
static int map_input(htttp_method_t m, const char *body, size_t blen){
    if (body == NULL) return -1;
    if (m == HTTTP_METHOD_MOVE){
        if (blen == 4 && strncmp(body, "LEFT", 4)  == 0) return TB_INPUT_LEFT;
        if (blen == 5 && strncmp(body, "RIGHT", 5) == 0) return TB_INPUT_RIGHT;
    } else if (m == HTTTP_METHOD_ROTATE){
        // The game engine currently only has one rotation (counter-clockwise,
        // with no wall kicks yet, which is the application team's later work).
        // We map both CW and CCW onto that single rotation, so the protocol
        // already accepts both even though they do the same thing for now.
        if ((blen == 2 && strncmp(body, "CW", 2)  == 0) ||
            (blen == 3 && strncmp(body, "CCW", 3) == 0)) return TB_INPUT_ROTATE;
    } else if (m == HTTTP_METHOD_DROP){
        if (blen == 4 && strncmp(body, "SOFT", 4) == 0) return TB_INPUT_SOFT_DROP;
        if (blen == 4 && strncmp(body, "HARD", 4) == 0) return TB_INPUT_HARD_DROP;
    }
    return -1;
}

static int do_input(client_t *c, const char *rid, const char *pid,
                    const htttp_msg_t *msg){
    // The path names a player, and that player has to be you, because the
    // identity is tied to this connection.
    if (strcmp(pid, c->player_id) != 0)
        return send_response(c, HTTTP_403_FORBIDDEN, "{\"error\": \"not your board\"}", NULL);
    room_t *r = room_at(c->room);
    if (r == NULL || strcmp(r->id, rid) != 0)
        return send_response(c, HTTTP_403_FORBIDDEN, "{\"error\": \"not in that room\"}", NULL);
    if (!r->started)
        return send_response(c, HTTTP_409_CONFLICT, "{\"error\": \"game not started\"}", NULL);

    int input = map_input(msg->method, msg->body, msg->body_len);
    if (input < 0)
        return send_response(c, HTTTP_400_BAD_REQUEST, "{\"error\": \"bad input body\"}", NULL);

    // We keep just one pending input per seat, and it is applied on the next
    // tick. If several inputs arrive before that tick, the last one wins. At
    // 20 ticks per second this keeps the input rate reasonable on its own. A
    // proper queue could be added later.
    pthread_mutex_lock(&r->mu);
    r->pending[c->seat] = (tb_input)input;
    pthread_mutex_unlock(&r->mu);
    return send_response(c, HTTTP_200_OK, "{\"ok\": true}", NULL);
}

// --- the dispatch pipeline ----------------------------------------------------

static void dispatch_request(client_t *c, const htttp_msg_t *msg){
    char rid[ROOM_ID_MAX], pid[CLIENT_ID_MAX];
    int status;

    int pathkind = parse_room_path(msg->path, rid, sizeof rid, pid, sizeof pid);

    switch (msg->method){
    case HTTTP_METHOD_JOIN:
        // JOIN is the request that hands out the identity, so it does not need
        // a Player-Id header yet.
        status = (pathkind == 0) ? do_join(c, rid)
               : send_response(c, HTTTP_400_BAD_REQUEST, "{\"error\": \"bad path\"}", NULL);
        break;

    case HTTTP_METHOD_LEAVE:
    case HTTTP_METHOD_START: {
        int auth = check_player_id(c, msg);
        if (auth != 0){
            status = send_response(c, (htttp_status_t)auth, "{\"error\": \"auth\"}", NULL);
            break;
        }
        if (pathkind != 0){
            status = send_response(c, HTTTP_400_BAD_REQUEST, "{\"error\": \"bad path\"}", NULL);
            break;
        }
        status = (msg->method == HTTTP_METHOD_LEAVE) ? do_leave(c, rid) : do_start(c, rid);
        break;
    }

    case HTTTP_METHOD_MOVE:
    case HTTTP_METHOD_ROTATE:
    case HTTTP_METHOD_DROP: {
        int auth = check_player_id(c, msg);
        if (auth != 0){
            status = send_response(c, (htttp_status_t)auth, "{\"error\": \"auth\"}", NULL);
            break;
        }
        if (pathkind != 1){
            status = send_response(c, HTTTP_400_BAD_REQUEST, "{\"error\": \"bad path\"}", NULL);
            break;
        }
        status = do_input(c, rid, pid, msg);
        break;
    }

    default:
        status = send_response(c, HTTTP_400_BAD_REQUEST, "{\"error\": \"unknown method\"}", NULL);
        break;
    }

    // We log every request together with the status we replied with.
    slog("info", "req %s %s %s -> %d", c->player_id, msg->method_str, msg->path, status);
}

// A connected client's socket has data to read. One encrypted frame becomes
// one request.
static void handle_client_data(int fd){
    client_t *c = client_get(fd);
    if (c == NULL){                    // defensive: unknown fd in the set
        epoll_ctl(g_ep, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        return;
    }

    size_t plen = 0;
    unsigned char *plain = tetrissh_recv(c->sess, fd, &plen);   // malloc'd
    if (plain == NULL){
        // This one path covers a clean close, a failed decrypt, and a timeout.
        // We deliberately treat them all the same and do not report different
        // errors, so that an attacker cannot learn anything from how we fail.
        disconnect_client(fd, "connection closed or bad frame");
        return;
    }

    htttp_msg_t msg;
    htttp_err_t err = htttp_parse_request((const char *)plain, plen, &msg);
    if (err != HTTTP_OK){
        slog("warn", "%s: unparseable request (%s)", c->player_id, htttp_strerror(err));
        send_response(c, HTTTP_400_BAD_REQUEST, "{\"error\": \"malformed\"}", NULL);
    } else {
        dispatch_request(c, &msg);     // msg points into `plain`, so we must not free it yet
    }
    free(plain);
}

// --- the accept path ------------------------------------------------------------

static void handle_new_client(int listen_fd){
    int cfd = accept(listen_fd, NULL, NULL);
    if (cfd < 0) return;

    // We use a blocking socket with 5 second timeouts here. The reasoning is in
    // the note at the top of this file.
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    tetrissh_session_t *sess = tetrissh_session_alloc();
    if (sess == NULL){ close(cfd); return; }

    if (tetrissh_handshake_server(sess, cfd, g_cfg.cert_path, g_cfg.key_path) != 0){
        slog("warn", "handshake FAILED on fd %d: %s", cfd, tetrissh_strerror(sess));
        tetrissh_session_free(sess);
        close(cfd);
        return;
    }

    client_t *c = client_add(cfd, sess);
    if (c == NULL){                    // fd beyond registry bound
        slog("warn", "fd %d beyond client table, refusing", cfd);
        tetrissh_session_free(sess);
        close(cfd);
        return;
    }
    if (ep_add(g_ep, cfd) < 0){
        slog("error", "epoll add failed for fd %d: %s", cfd, strerror(errno));
        client_remove(cfd);
        close(cfd);
        return;
    }
    slog("info", "client fd %d: handshake OK, assigned id %s (%d connected)",
         cfd, c->player_id, client_count());
}

static void disconnect_client(int fd, const char *reason){
    client_t *c = client_get(fd);
    if (c != NULL){
        char room_id[ROOM_ID_MAX] = "";
        room_t *r = room_at(c->room);
        if (r) snprintf(room_id, sizeof room_id, "%s", r->id);

        slog("info", "client %s (fd %d) disconnected: %s", c->player_id, fd, reason);
        int orphan_tfd = room_leave(c);
        if (orphan_tfd >= 0){
            epoll_ctl(g_ep, EPOLL_CTL_DEL, orphan_tfd, NULL);
            close(orphan_tfd);
            slog("info", "room %s destroyed (last player gone)", room_id);
        }
        client_remove(fd);             // frees the session
    }
    epoll_ctl(g_ep, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

// --- the room ticker: where the server runs the authoritative game --------------

// Draw one player's board into dst, in the text format we send as the STATE
// body. The first line has the player id and their score, level, lines and a
// game-over flag. After that come 20 rows of 10 columns each, where a '.' is an
// empty cell and a digit marks a filled cell.
static size_t render_board(char *dst, size_t cap, const char *pid, const tb_game *g){
    int8_t cells[TB_ROWS][TB_COLS];
    tb_render(g, cells);
    size_t off = 0;
    off += (size_t)snprintf(dst + off, cap - off,
                            "player %s score %u level %u lines %u over %d\n",
                            pid, g->score, g->level, g->lines_total, g->game_over ? 1 : 0);
    for (int y = 0; y < TB_ROWS && off + TB_COLS + 2 < cap; y++){
        for (int x = 0; x < TB_COLS; x++)
            dst[off++] = cells[y][x] ? (char)('0' + (cells[y][x] % 10)) : '.';
        dst[off++] = '\n';
    }
    dst[off] = '\0';
    return off;
}

static void handle_room_tick(room_t *r){
    // Read how many times the timer has fired since we last checked. Normally
    // this is 1. If the loop fell behind, for example because a slow handshake
    // held it up, this can be more than 1, and we run that many catch-up ticks
    // so the game keeps up with real time.
    uint64_t expirations = 0;
    if (read(r->timer_fd, &expirations, sizeof expirations) != (ssize_t)sizeof expirations)
        return;

    char body[8192];
    size_t blen = 0;
    client_t *recipients[ROOM_MAX_PLAYERS];
    int nrec = 0;
    int garbage_rows = 0;      // total rows this room earned this tick

    // We advance the games and draw the boards while holding the room's lock,
    // and we build and send the network frame after we release it. This is the
    // same rule as the log ring: only quick memory work under the lock, and the
    // slow input and output outside it.
    pthread_mutex_lock(&r->mu);
    r->ticks += expirations;
    for (int i = 0; i < ROOM_MAX_PLAYERS; i++){
        if (r->players[i] == NULL) continue;

        // tb_tick only returns a bool, so it cannot tell us how many lines were
        // cleared. lines_total is cumulative though, so the difference across
        // the tick is the answer, and no engine change is needed to detect it.
        uint32_t lines_before = r->games[i].lines_total;

        for (uint64_t e = 0; e < expirations; e++){
            // The player's queued input is applied on the first tick only. Any
            // extra catch-up ticks just apply gravity with no input.
            tb_input in = (e == 0) ? r->pending[i] : TB_INPUT_NONE;
            if (!r->games[i].game_over)
                tb_tick(&r->games[i], in);
        }

        int cleared = (int)(r->games[i].lines_total - lines_before);
        garbage_rows += garbage_rows_for(cleared);   // see the table in garbage.h

        r->pending[i] = TB_INPUT_NONE;
        blen += render_board(body + blen, sizeof body - blen,
                             r->players[i]->player_id, &r->games[i]);
        recipients[nrec++] = r->players[i];
    }
    uint64_t tick_now = r->ticks;
    char path[ROOM_ID_MAX + 8];
    snprintf(path, sizeof path, "/room/%s", r->id);
    pthread_mutex_unlock(&r->mu);

    // Send any garbage this room earned. This happens AFTER the unlock, for the
    // same reason the STATE frame does: mq_send is a system call, and the rule
    // in rooms.h is that a room lock is never held across one. It also means we
    // never hold this room's lock while touching the room table to pick a
    // target. That is what keeps two-lock deadlock structurally impossible.
    if (garbage_rows > 0){
        room_t *dst = room_pick_garbage_target(r, rng_next());
        if (dst != NULL){
            garbage_msg_t gm;
            memset(&gm, 0, sizeof gm);     // no uninitialised padding on the wire
            gm.magic        = GARBAGE_MAGIC;
            gm.rows         = (uint8_t)garbage_rows;
            gm.hole_pattern = (uint16_t)(1u << (rng_next() % TB_COLS));
            snprintf(gm.src_room, sizeof gm.src_room, "%s", r->id);
            snprintf(gm.dst_room, sizeof gm.dst_room, "%s", dst->id);

            // The queue is O_NONBLOCK, so a full queue returns immediately with
            // EAGAIN instead of parking the event loop. We then throw the event
            // away and keep ticking. This is the same policy ioq3 uses for an
            // overflowing snapshot buffer (sv_snapshot.c:628): notice it, log
            // it, discard it, carry on. A game server must never block on a
            // full buffer. A dropped garbage row costs one player a slightly
            // easier board, whereas a stalled tick freezes everybody.
            if (mq_send(g_mq, (const char *)&gm, sizeof gm, 0) < 0){
                atomic_fetch_add(&garbage_dropped, 1);
                slog("warn", "garbage DROPPED (queue full): %s -> %s rows=%d: %s",
                     gm.src_room, gm.dst_room, garbage_rows, strerror(errno));
            } else {
                atomic_fetch_add(&garbage_sent, 1);
                slog("info", "garbage sent: %s -> %s rows=%d hole=0x%03x",
                     gm.src_room, gm.dst_room, garbage_rows, gm.hole_pattern);
            }
        }
    }

    if (nrec == 0) return;

    // We build one STATE frame for the whole room on this tick, and then
    // encrypt it separately for each player. STATE is the only message the
    // server starts on its own. It is a request that never expects a reply, so
    // the client has to look at each incoming frame and handle it by its type.
    htttp_builder_t b;
    htttp_builder_init_request(&b, HTTTP_METHOD_STATE, path);
    char tickbuf[24];
    snprintf(tickbuf, sizeof tickbuf, "%llu", (unsigned long long)tick_now);
    htttp_builder_add_header(&b, "Content-Type", "application/tetris-state");
    htttp_builder_add_header(&b, "Tick", tickbuf);
    htttp_builder_set_body(&b, (const unsigned char *)body, blen);

    size_t wlen = 0;
    char *wire = htttp_serialise(&b, &wlen);
    if (wire == NULL) return;

    int failed[ROOM_MAX_PLAYERS], nfailed = 0;
    for (int i = 0; i < nrec; i++){
        if (tetrissh_send(recipients[i]->sess, recipients[i]->fd,
                          (unsigned char *)wire, wlen) != 0)
            failed[nfailed++] = recipients[i]->fd;
    }
    free(wire);

    // If a client cannot receive a STATE frame within the 5 second send
    // timeout, we disconnect it. Each STATE frame is the complete current
    // board, so an old one is not worth keeping. That means there is nothing to
    // gain from queueing frames for a slow client.
    for (int i = 0; i < nfailed; i++)
        disconnect_client(failed[i], "STATE send failed (slow client)");
}

// --- signal + mq + ctl handlers -------------------------------------------------

static void handle_signal_event(int sig_fd, const char *rc_path, int *running){
    struct signalfd_siginfo si;
    ssize_t r = read(sig_fd, &si, sizeof si);
    if (r != (ssize_t)sizeof si) return;

    switch (si.ssi_signo){
    case SIGTERM:
        slog("info", "tetrisd: SIGTERM -> shutting down");
        *running = 0;
        break;
    case SIGHUP: {
        // Load the new config into a temporary struct first. rc_load fills in
        // default values before it reads the file, so if we loaded straight
        // into the live g_cfg and the reload then failed, we would have already
        // wrecked the running config. By loading into tmp and only copying it
        // over on full success, a failed reload leaves the old config untouched.
        Config tmp;
        if (rc_load(rc_path, &tmp) == 0){
            g_cfg = tmp;
            slog("info", "tetrisd: SIGHUP -> config reloaded (listeners keep old port/paths)");
        } else {
            slog("warn", "tetrisd: SIGHUP -> reload FAILED, keeping old config");
        }
        break;
    }
    case SIGUSR1:
        slog("info", "tetrisd: SIGUSR1 -> STATE DUMP: rooms=%d players=%d dropped_ring=%lu dropped_send=%lu",
             rooms_count(), client_count(),
             ring_dropped(&g_ring), atomic_load(&dropped_send));
        break;
    }
}

static void handle_garbage(mqd_t mq){
    // The queue is level-triggered in the epoll set and non-blocking, so we
    // drain it in a loop rather than taking one message per wakeup. Several
    // rooms can finish a tick before the loop comes back round to us, and a
    // single-message handler would fall steadily behind under load.
    for (;;){
        char gbuf[GARBAGE_MSG_MAX];
        ssize_t n = mq_receive(mq, gbuf, sizeof gbuf, NULL);
        if (n < 0) return;                       // EAGAIN: queue drained

        if ((size_t)n != sizeof(garbage_msg_t)){
            slog("warn", "garbage: bad message size %zd, expected %zu", n,
                 sizeof(garbage_msg_t));
            continue;
        }
        garbage_msg_t gm;
        memcpy(&gm, gbuf, sizeof gm);            // memcpy, not a cast: gbuf is a
                                                 // char array with no guaranteed
                                                 // alignment for the struct
        if (gm.magic != GARBAGE_MAGIC){
            // A POSIX message queue outlives the process that made it, so a
            // stale message from an older build can still be sitting there at
            // startup. The magic number catches that instead of injecting junk.
            slog("warn", "garbage: bad magic 0x%08x, ignoring stale message",
                 gm.magic);
            continue;
        }

        // The target may have been destroyed between the send and now, because
        // its last player can leave inside the same epoll batch. That is not an
        // error, it just means the attack missed.
        room_t *dst = room_find(gm.dst_room);
        if (dst == NULL || !dst->started){
            slog("info", "garbage: target room %s is gone, event discarded",
                 gm.dst_room);
            continue;
        }

        // Injection touches the per-seat games[] array, so it takes the target
        // room's lock. We hold exactly one room lock here and call nothing that
        // blocks while holding it, which is what rooms.h promises.
        int hit = 0;
        pthread_mutex_lock(&dst->mu);
        for (int i = 0; i < ROOM_MAX_PLAYERS; i++){
            if (dst->players[i] == NULL) continue;
            if (dst->games[i].game_over) continue;   // already out, leave them
            tb_inject_garbage(&dst->games[i], gm.rows, gm.hole_pattern);
            hit++;
        }
        pthread_mutex_unlock(&dst->mu);

        slog("info", "garbage applied: %s -> %s rows=%u players=%d",
             gm.src_room, gm.dst_room, (unsigned)gm.rows, hit);
    }
}

// --- control plane: plaintext HTTTP over the UDS --------------------------------
// We deliberately send this in plain text, with no encryption. The socket is a
// file on the local machine, reachable only by users the operating system
// already trusts with it. Adding encryption would not protect against an
// attacker who can already open local sockets as us, so it would add work for
// no real benefit. The channel still speaks real HTTTP through libhtttp, as the
// assignment requires.

struct jbuf { char buf[4096]; size_t off; int first; };

static void jb_append(struct jbuf *j, const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    j->off += (size_t)vsnprintf(j->buf + j->off, sizeof j->buf - j->off, fmt, ap);
    va_end(ap);
}

static void jb_room(room_t *r, void *arg){
    struct jbuf *j = arg;
    jb_append(j, "%s{\"id\": \"%s\", \"players\": %d, \"started\": %d, \"ticks\": %llu}",
              j->first ? "" : ", ", r->id, r->nplayers, r->started,
              (unsigned long long)r->ticks);
    j->first = 0;
}

static void jb_player(client_t *c, void *arg){
    struct jbuf *j = arg;
    room_t *r = room_at(c->room);
    jb_append(j, "%s{\"player\": \"%s\", \"fd\": %d, \"room\": \"%s\"}",
              j->first ? "" : ", ", c->player_id, c->fd, r ? r->id : "");
    j->first = 0;
}

static void handle_ctl(int ctl_fd, int *running){
    int cfd = accept(ctl_fd, NULL, NULL);
    if (cfd < 0) return;

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    // Keep reading until libhtttp tells us the message is complete. Its
    // "incomplete" result doubles as a "need more bytes" signal, so this is a
    // small loop that reads a bit, tries to parse, and repeats.
    char req[2048];
    size_t got = 0;
    htttp_msg_t msg;
    htttp_err_t err = HTTTP_ERR_INCOMPLETE;
    while (err == HTTTP_ERR_INCOMPLETE && got < sizeof req - 1){
        ssize_t n = read(cfd, req + got, sizeof req - 1 - got);
        if (n <= 0) break;                       // timeout or client gone
        got += (size_t)n;
        err = htttp_parse_request(req, got, &msg);
    }

    htttp_status_t status;
    struct jbuf j = { .off = 0, .first = 1 };

    if (err != HTTTP_OK){
        status = HTTTP_400_BAD_REQUEST;
        jb_append(&j, "{\"error\": \"%s\"}", htttp_strerror(err));
    } else if (msg.method != HTTTP_METHOD_GET){
        status = HTTTP_400_BAD_REQUEST;
        jb_append(&j, "{\"error\": \"control plane accepts GET only\"}");
    } else if (strcmp(msg.path, "/status") == 0){
        status = HTTTP_200_OK;
        jb_append(&j, "{\"uptime_seconds\": %ld, \"rooms\": %d, "
                      "\"players\": %d, \"dropped_logs\": %lu, "
                      "\"garbage_sent\": %lu, \"garbage_dropped\": %lu}",
                  uptime_seconds(), rooms_count(), client_count(),
                  total_dropped(),
                  atomic_load(&garbage_sent), atomic_load(&garbage_dropped));
    } else if (strcmp(msg.path, "/rooms") == 0){
        status = HTTTP_200_OK;
        jb_append(&j, "[");
        rooms_foreach(jb_room, &j);
        jb_append(&j, "]");
    } else if (strcmp(msg.path, "/players") == 0){
        status = HTTTP_200_OK;
        jb_append(&j, "[");
        client_foreach(jb_player, &j);
        jb_append(&j, "]");
    } else if (strcmp(msg.path, "/dropped-logs") == 0){
        status = HTTTP_200_OK;
        jb_append(&j, "{\"ring\": %lu, \"send\": %lu, \"total\": %lu}",
            ring_dropped(&g_ring), atomic_load(&dropped_send),
            total_dropped());
    } else if (strcmp(msg.path, "/shutdown") == 0){
        status = HTTTP_200_OK;
        jb_append(&j, "{\"ok\": true, \"shutting_down\": true}");
        slog("info", "admin: SHUTDOWN via tetrisctl");
        *running = 0;
    } else {
        status = HTTTP_404_NOT_FOUND;
        jb_append(&j, "{\"error\": \"unknown control path\"}");
    }

    if (err == HTTTP_OK)
        slog("info", "admin: %s %s -> %d", msg.method_str, msg.path, (int)status);

    htttp_builder_t b;
    htttp_builder_init_response(&b, status);
    htttp_builder_add_header(&b, "Content-Type", "application/json");
    htttp_builder_set_body(&b, (const unsigned char *)j.buf, j.off);
    size_t wlen = 0;
    char *wire = htttp_serialise(&b, &wlen);
    if (wire != NULL){
        // We ignore SIGPIPE process-wide, so a write to a control client that
        // has already gone away returns -1 with errno == EPIPE instead of
        // killing the daemon. That is the whole point of ignoring the signal:
        // the error becomes an ordinary return value we can inspect. A short
        // write is reported separately. It is not an error, but it does mean
        // the admin saw a truncated reply, so it should not pass silently.
        // msg is only filled in when the request actually parsed, so on a
        // parse failure we must not read msg.path.
        const char *what = (err == HTTTP_OK) ? msg.path : "(unparsed request)";
        ssize_t wr = write(cfd, wire, wlen);
        if (wr < 0)
            slog("warn", "admin: reply to %s failed: %s", what, strerror(errno));
        else if ((size_t)wr != wlen)
            slog("warn", "admin: reply to %s truncated, %zd of %zu bytes",
                 what, wr, wlen);
        free(wire);
    }
    close(cfd);
}

// --- main -----------------------------------------------------------------------

int main(int argc, char **argv){
    // 1. Load the config file (.tetrishrc), then set up the table that will
    //    hold the game rooms.
    const char *rc_path = (argc > 1) ? argv[1] : ".tetrishrc";
    if (rc_load(rc_path, &g_cfg) != 0){
        fprintf(stderr, "tetrisd: failed to load configuration from %s\n", rc_path);
        return 1;
    }
    // After the room system is initialized, record the daemon's start time in g_started
    rooms_init(g_cfg.max_rooms, g_cfg.max_players_per_room);
    clock_gettime(CLOCK_MONOTONIC, &g_started);
    

    // 2. Deal with signals. First we ignore SIGPIPE. We write to network peers
    //    that may disconnect at any time, and without this a write to a peer
    //    that has gone away would kill the whole daemon. Then we block the
    //    three signals we care about (SIGTERM, SIGHUP, SIGUSR1) so that later
    //    we can receive them as readable events through signalfd, instead of
    //    through a separate handler function.
    signal(SIGPIPE, SIG_IGN);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0){ perror("sigprocmask"); return 1; }

    // 3 and 4. Create both listening sockets before we daemonize. We do it in
    //    this order so that if a socket fails to bind, the error is still
    //    printed to the terminal, and so that the open sockets carry over into
    //    the daemon after the fork.
    int listen_fd = tcp_listen(&g_cfg);
    if (listen_fd < 0) return 1;
    int ctl_fd = ctl_listen(&g_cfg);
    if (ctl_fd < 0){ close(listen_fd); return 1; }

    // 5. Open the POSIX message queue that carries Battle Royale garbage.
    //
    //    O_RDWR, not O_RDONLY: tetrisd is both ends of this channel. A room
    //    that clears two or more lines sends an event, and the same process
    //    reads it back out of the epoll set and applies it to another room.
    //    With O_RDONLY the descriptor is valid but write-only operations fail
    //    with EBADF, so mq_send would silently never work.
    //
    //    O_NONBLOCK is what makes the drop policy possible: mq_send on a full
    //    queue returns EAGAIN immediately instead of parking the event loop,
    //    and mq_receive on an empty one lets the drain loop terminate.
    //
    //    mq_maxmsg 10 bounds the queue. That bound is deliberate and is the
    //    opposite of the choice Redis makes for its background job list
    //    (bio.c:72), which is unbounded because dropping an fsync would lose
    //    data. Dropping a garbage row only makes one player's board slightly
    //    easier, so here a bounded queue with an explicit drop is correct.
    struct mq_attr attr = { .mq_flags = 0, .mq_maxmsg = 10,
                            .mq_msgsize = GARBAGE_MSG_MAX, .mq_curmsgs = 0 };
    mqd_t mq = mq_open(g_cfg.garbage_mq, O_CREAT | O_RDWR | O_NONBLOCK, 0600, &attr);
    if (mq == (mqd_t)-1){
        perror("mq_open");
        close(listen_fd); close(ctl_fd); return 1;
    }
    g_mq = mq;                 // the tick handler sends through this

    // Seed the targeting PRNG. getpid keeps two daemons on one machine from
    // picking identical target sequences. This randomness never touches game
    // state, because libtetrisbrain has its own seeded PRNG for that.
    g_rng ^= (uint32_t)getpid() * 2654435761u;

    // 6. Set up the socket we use to send logs to tetrislogd. After this point
    //    only the logshipper thread ever uses it.
    log_init(g_cfg.log_ipc);

    // 7. Turn the process into a background daemon, unless the config asked us
    //    to stay in the foreground with daemonize=no.
    if (g_cfg.daemonize){
        if (daemonize() < 0){ close(listen_fd); close(ctl_fd); return 1; }
    }

    // 8. Write the process id file. We do this after daemonizing, so that the
    //    file holds the daemon's real process id and not the one of the parent
    //    that has already exited.
    write_pidfile(g_cfg.pid_path);

    // 9. Set up the log ring buffer and start the logshipper thread. This must
    //    happen after daemonizing, because fork() only keeps the thread that
    //    called it. A thread started before the fork would not exist inside the
    //    daemon.
    ring_init(&g_ring);
    atomic_init(&dropped_send, 0);
    atomic_init(&shipper_stop, 0);
    atomic_init(&garbage_sent, 0);
    atomic_init(&garbage_dropped, 0);
    pthread_t shipper;
    if (pthread_create(&shipper, NULL, logshipper, NULL) != 0){
        perror("pthread_create");
        close(listen_fd); close(ctl_fd); mq_close(mq); return 1;
    }

    // 10. Create the signalfd. This turns the signals we blocked earlier into
    //     something we can read from as if it were a file, so the event loop
    //     can wait on them together with the sockets.
    int sig_fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sig_fd < 0){ perror("signalfd"); close(listen_fd); close(ctl_fd); return 1; }

    // 11. Create the epoll set and add our fixed file descriptors to it: the
    //     TCP listener, the signalfd, the control socket, and the message
    //     queue. epoll is what lets one thread wait on all of them at once.
    g_ep = epoll_create1(EPOLL_CLOEXEC);
    if (g_ep < 0){ perror("epoll_create1"); return 1; }
    if (ep_add(g_ep, listen_fd) < 0 || ep_add(g_ep, sig_fd) < 0 ||
        ep_add(g_ep, ctl_fd)    < 0 || ep_add(g_ep, (int)mq) < 0){
        perror("epoll_ctl"); return 1;
    }

    slog("info", "tetrisd: started (full pipeline: handshake+HTTTP+rooms+ticker)");

    // 12. The main event loop. epoll_wait sleeps until one or more file
    //     descriptors have something to read, and then we handle each one. We
    //     first check it against our four fixed descriptors. If it is none of
    //     those, it is either a connected client (which we look up quickly in
    //     the client table) or a room's game timer.
    int running = 1;
    while (running){
        struct epoll_event events[32];
        int n = epoll_wait(g_ep, events, 32, -1);
        if (n < 0){
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n && running; i++){
            int fd = events[i].data.fd;
            if      (fd == listen_fd) handle_new_client(listen_fd);
            else if (fd == sig_fd)    handle_signal_event(sig_fd, rc_path, &running);
            else if (fd == ctl_fd)    handle_ctl(ctl_fd, &running);
            else if (fd == (int)mq)   handle_garbage(mq);
            else if (client_get(fd) != NULL) handle_client_data(fd);
            else {
                room_t *r = room_by_timerfd(fd);
                if (r != NULL) handle_room_tick(r);
                else epoll_ctl(g_ep, EPOLL_CTL_DEL, fd, NULL);   // stale fd safety
            }
        }
    }

    // 13. Shut down cleanly in four phases, with each one being logged
    // so the sequence is visible in tetrislogd's file afterwards.
    //
    // Phase 1: stop accepting. Take the TCP listener out of the epoll set,
    // then close it. Closing a descriptor DOES remove it from the epoll set
    // on its own, but only once the last reference to the underlying open
    // file description goes away, so doing it explicitly first leaves no
    // window in which the loop could be handed an accept event for a socket
    // we have already decided to abandon. This is a policy step (take no new
    // work), not resource cleanup, which is why it lives here and not in the
    // close() block at the bottom. Redis draws the same line by giving it a
    // dedicated function, closeListeningSockets() (server.c:4872).
    epoll_ctl(g_ep, EPOLL_CTL_DEL, listen_fd, NULL);
    close(listen_fd);
    slog("info", "tetrisd: phase 1: stopped accepting new clients");

    // Phase 2: drain. Disconnecting a client also tears down its room and
    // that room's game timer, and frees its secure session, so this one loop
    // reclaims everything the connection owned.
    int drained = 0;
    for (int fd = 0; fd < CLIENT_MAX_FD; fd++)
        if (client_get(fd) != NULL){
            disconnect_client(fd, "server shutting down");
            drained++;
        }
    slog("info", "tetrisd: phase 2: disconnected %d clients", drained);

    // Phases 3 and 4: flush, then exit. Both records are pushed BEFORE the
    // shipper is stopped, because the shipper is the only thing that can
    // deliver them. Once it is joined, nothing can be logged any more, so
    // record 4 describes what is about to happen rather than what already has.
    // That is unavoidable: the log channel is itself part of what shutdown
    // tears down.
    slog("info", "tetrisd: phase 3: flushing log ring");
    slog("info", "tetrisd: phase 4: closing descriptors and exiting");

    // The shipper exits only once the ring is empty AND the stop flag is set,
    // so the join below is what guarantees the four records above actually
    // reached tetrislogd before we close the socket.
    atomic_store(&shipper_stop, 1);
    pthread_join(shipper, NULL);

    close(g_ep);
    close(sig_fd);
    close(ctl_fd);
    mq_close(mq);
    mq_unlink(g_cfg.garbage_mq);
    if (log_fd >= 0) close(log_fd);
    unlink(g_cfg.ctl_path);
    unlink(g_cfg.pid_path);
    ring_destroy(&g_ring);
    return 0;
}
