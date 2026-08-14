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
// A note on the blocking socket I/O, which is worth being able to explain.
//
// Handshakes and frame reads both run on this one thread, on blocking sockets.
// That is deliberate: libtetrissh's send and receive helpers use MSG_WAITALL
// loops, which assume a blocking descriptor, and a non-blocking fd would fail
// partway through a handshake with EAGAIN. At our scale (an RSA handshake on
// the local network costs a few milliseconds) the pause is not noticeable.
//
// What bounds that pause is a DEADLINE, not a socket timeout, and the
// difference is the whole reason the code looks the way it does.
// SO_RCVTIMEO and SO_SNDTIMEO expire per syscall. A recv() that times out
// having already copied some bytes returns the short count instead of failing,
// so a reassembly loop advances and calls recv() again with a fresh, full
// timeout: a peer sending one byte per timeout period resets the clock forever
// and never trips the limit. An earlier version of this comment claimed such a
// client "can only stall the loop for up to 5 seconds". That was wrong, and it
// was wrong in the dangerous direction, because it is the unauthenticated
// pre-handshake read that a peer can stall the longest.
//
// libtetrissh therefore stamps one absolute deadline per logical operation
// (see TETRISSH_HANDSHAKE_TIMEOUT_MS and TETRISSH_FRAME_TIMEOUT_MS) and
// derives every syscall's timeout from the time left until it. Forward
// progress no longer buys the peer extra time, so the worst a slow or hostile
// client can cost this thread is one budget, once, before it is dropped.
//
// A production server would move handshakes onto a pool of threads, or drive
// clients with a non-blocking state machine and per-connection input and
// output buffers. At this project's scale, inline work under a deadline is the
// simple and honest baseline.
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
#include <sys/stat.h>        // umask, mode_t: the control socket's permissions
#include <sys/time.h>
#include <time.h>            // clock_gettime, struct timespec (uptime)
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <sys/resource.h>   // getrlimit/setrlimit for the fd budget
#include <mqueue.h>
#include "rc.h"
#include "daemon.h"
#include "ring.h"
#include "tetrissh.h"        // NET teammate: secure session
#include "htttp.h"           // NET teammate: HTTTP parse/serialise
#include "clients.h"
#include "rooms.h"
#include "garbage.h"         // the Battle Royale garbage event format

// Daemon-wide singletons: one config, one epoll set, one log ring.
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

// Connections refused at accept() by the per-IP limit. Redis keeps the same
// counter for the same reason and reports it as rejected_connections in INFO
// (server.h:2132, networking.c:1679). Without it, a rejection is invisible
// unless somebody happens to be reading the log at the time.
static atomic_ulong rejected_conns;

// --- admission: the pending-handshake queue ---------------------------------
//
// The handshake used to run inline in the accept handler. That is an RSA
// signature plus an RSA decrypt, measured at about 8 ms, and while it runs the
// event loop does nothing else: no ticks, no input, no control plane. It was
// fine at twenty players. At five hundred arriving together it was not: the
// measured rate is roughly 130 handshakes a second, so the loop froze for about
// four seconds and clients at the back of the queue timed out waiting for the
// certificate. A 500-client burst lost 69 of them that way.
//
// So accept() now does almost nothing. It takes the connection, applies the
// per-IP limit, and parks the descriptor here. The loop then completes at most
// HANDSHAKE_BUDGET of them per iteration, which bounds how long any single pass
// can stall and lets ticks, input and admin commands interleave with a rush of
// arrivals. Total throughput is about the same; what changes is that the game
// no longer stops while the queue drains.
//
// This is the ordering principle Quake 3 uses, arrived at from the opposite
// direction. It makes a client prove its address with a cheap challenge round
// trip before the server will spend anything substantial on it, precisely so a
// flood cannot buy expensive work. TCP's own handshake already proves the
// address for us, so the challenge is not the part we need. The other half of
// the idea is: expensive per-connection work gets rationed by the loop, not
// done on demand.
// The queue depth is fixed; how many we complete per pass is configurable,
// because it is the one real tuning knob in this design and its best value
// depends on how fast the machine's RSA is. Measured here with 500 clients all
// connecting at once, against 63 rooms ticking at 20 Hz:
//
//     budget    admitted    tick rate
//     inline       431/500    19.8 Hz     (the old behaviour, no queue)
//          4       394/500    19.6 Hz
//         16       470/500    19.7 Hz
//         32       500/500    19.4 Hz
//
// A larger budget admits more of a burst at the cost of a slightly slower tick
// while the burst lasts. Even a small budget beats the old inline path on
// admissions once accept() itself is cheap, because the kernel's listen backlog
// can then drain instead of overflowing.
// One more thing the queue has to get right, and it is the reason the parked
// descriptors go into the epoll set rather than being swept in a loop.
//
// A parked connection is not necessarily one that is ready to be handshaken.
// The client speaks first (it sends the 32-byte nonce), so a peer that
// connects and then says nothing leaves finish_handshake blocked in that first
// read. Draining the queue on a timer therefore made a connect() with no
// follow-up into a way to stop the loop for as long as the read allowed, and
// because the drain is serial, a queue full of such peers concatenated their
// stalls. That is a freeze bought with a bare TCP connection, no certificate
// and no key exchange, which is cheaper than any attack the per-IP cap was
// written to stop.
//
// So a parked fd is registered with epoll like everything else in this daemon,
// and its handshake begins only when the kernel says bytes have arrived. A
// silent peer then costs nothing at all: it occupies a queue slot and is
// reaped by PENDING_HS_TIMEOUT_MS, and the loop never waits on it. A peer that
// starts talking and then stalls is bounded by libtetrissh's per-operation
// budget instead. This is the same principle as every other fd here: the loop
// is told when there is work, it does not go looking for it.
#define PENDING_HS_MAX        512   // parked connections; beyond this we refuse
#define PENDING_HS_TIMEOUT_MS 10000 // connected but never sent a byte -> reap

// How many ready descriptors one epoll_wait may return.
//
// This is not a throughput knob to tune by feel; it has to stay comfortably
// above the number of descriptors that can be ready at once, because
// admission is now driven by readiness. Every parked connection is in the
// epoll set alongside every room's ticker, so with a full admission queue and
// rooms ticking, PENDING_HS_MAX + ROOM_HARD_MAX descriptors can all be ready
// in the same pass. A batch smaller than that does not lose events (the set is
// level-triggered, so they come back), but it does make handshakes queue
// behind ticks for several passes, and a client waiting that long gives up on
// its own handshake deadline before the server ever reaches it.
//
// Measured: at 32, a 500-client burst intermittently lost about 7% of arrivals
// to exactly that timeout. Sized to cover the whole set, the same burst seats
// all 500. The cost is stack: one epoll_event is 12 bytes, so this array is
// about 7.7 KB in main's frame, which is a fair price for making admission
// independent of how many rooms happen to be ticking.
#define EPOLL_BATCH (PENDING_HS_MAX + ROOM_HARD_MAX + 16)

typedef struct {
    int       fd;
    uint32_t  addr;              // peer IPv4, network order, for the log
    long long parked_ms;         // when it was accepted, for the reaper
} pending_hs_t;

static pending_hs_t g_pending_hs[PENDING_HS_MAX];
static int          g_npending_hs = 0;
static atomic_ulong hs_queued, hs_refused_full;

// How many connections from this address are parked waiting to be admitted.
//
// The per-IP limit has to count these as well as the established clients.
// client_count_addr only sees connections that finished their handshake,
// because client_add runs at the end of finish_handshake, so counting it alone
// leaves the queue itself unmetered: one address could park all PENDING_HS_MAX
// slots and never trip the limit, since none of those connections has been
// admitted yet. Both halves together are the real per-IP footprint.
static int pending_count_addr(uint32_t addr){
    int n = 0;
    for (int i = 0; i < g_npending_hs; i++)
        if (g_pending_hs[i].addr == addr)
            n++;
    return n;
}

// Where this fd sits in the queue, or -1 if it is not parked.
static int pending_index(int fd){
    for (int i = 0; i < g_npending_hs; i++)
        if (g_pending_hs[i].fd == fd)
            return i;
    return -1;
}

// Drop slot i, keeping the queue contiguous and oldest-first.
static void pending_remove(int i){
    g_npending_hs--;
    if (i < g_npending_hs)
        memmove(&g_pending_hs[i], &g_pending_hs[i + 1],
                (size_t)(g_npending_hs - i) * sizeof g_pending_hs[0]);
}

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
    // O_NONBLOCK via fcntl rather than Linux's SOCK_NONBLOCK type flag, so
    // this builds on macOS too. There is no atomicity concern: no thread
    // touches this fd until after log_init returns.
    log_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (log_fd >= 0)
        fcntl(log_fd, F_SETFL, fcntl(log_fd, F_GETFL, 0) | O_NONBLOCK);
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

// --- the replay log -------------------------------------------------------
//
// Replay records travel the same ring and the same socket as the human-readable
// log, because tetrislogd is the process that owns persistence. tetrisd
// generates the records, tetrislogd only writes them down. That split matters:
// tetrislogd cannot see a tick happen, so it could not generate a snapshot even
// if we asked it to.
//
// Two record types, both prefixed so a parser can pick them out of a file that
// also contains ordinary prose logging. tetrislogd prepends its own wall-clock
// stamp to every line it writes, so a record arrives on disk looking like:
//
//   2026-08-02 10:25:23 E 1234567890 480 roomA p7 INPUT LEFT
//   \_________________/ \_________________________________/
//    tetrislogd's stamp        the record we generated
//
// A reader skips the first two whitespace-separated fields and dispatches on
// the third.
//
//   E <mono_ns> <tick> <room> <player> <action> [params...]
//   S <mono_ns> <tick> <room> <player> <w> <h> <cells_hex> <seed>
//
// The tick number is what makes replay exact rather than approximate.
// libtetrisbrain advances gravity per tick and has no clock of its own, so a
// reader with only timestamps would have to guess how many ticks passed between
// two events. With the tick recorded, replay is: seed the engine, then for tick
// 0..N apply whatever input that tick carried and call tb_tick once.
//
// The actions that replay actually needs are SEED, INPUT and GARBAGE. CLEAR and
// OVER are derivable from replaying those, and are emitted anyway so a reader
// can verify its own reconstruction against what the server recorded.
//
// The S record re-states the seat's seed as its last field. The SEED record is
// written exactly once, at START, and a hundred rooms starting together emit
// hundreds of records into a datagram queue that holds about ten, so the burst
// that creates SEED records is the burst that drops them. Re-stating the seed
// on every snapshot means any single surviving snapshot is enough to seed a
// replay; the log stops having one indispensable line per session.
static void rlog(const char *fmt, ...){
    char buf[RING_REC_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ring_push(&g_ring, buf, strlen(buf));
}

// Monotonic nanoseconds, the timestamp both record types carry. Monotonic for
// the same reason uptime uses it: it cannot be stepped backwards, so records
// can never appear to go back in time mid-session.
static unsigned long long mono_ns(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull
         + (unsigned long long)ts.tv_nsec;
}

// One hex digit per visible cell, which is what keeps a snapshot inside one
// ring record. The engine stores 0 for empty, 1..7 for a locked piece, and
// TB_CELL_GARBAGE for garbage; garbage maps to 8 so every value fits a nibble.
// TB_CELL_WALL never appears because we only walk the visible playfield.
static void board_hex(const tb_game *g, char *out, size_t cap){
    static const char digits[] = "0123456789abcdef";
    size_t n = 0;
    for (int y = 1; y <= TB_ROWS && n + 1 < cap; y++)
        for (int x = 1; x <= TB_COLS && n + 1 < cap; x++){
            int8_t c = g->cells[y][x];
            int v = (c == TB_CELL_EMPTY) ? 0
                  : (c == TB_CELL_GARBAGE) ? 8
                  : (c >= 1 && c <= 7) ? c : 0;
            out[n++] = digits[v];
        }
    out[n] = '\0';
}

static const char *input_name(tb_input in){
    switch (in){
    case TB_INPUT_LEFT:      return "LEFT";
    case TB_INPUT_RIGHT:     return "RIGHT";
    case TB_INPUT_ROTATE_CW:  return "ROTATE_CW";
    case TB_INPUT_ROTATE_CCW: return "ROTATE_CCW";
    case TB_INPUT_SOFT_DROP: return "SOFT";
    case TB_INPUT_HARD_DROP: return "HARD";
    case TB_INPUT_ROTATE_180: return "ROTATE_180";
    case TB_INPUT_HOLD:      return "HOLD";
    default:                 return "NONE";
    }
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

// seconds since the daemon started. monotonic, per the note on g_started, so
// this can never come out negative.
static long uptime_seconds(void){
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - g_started.tv_sec);
}

// Make sure the process can actually open as many descriptors as the config
// implies, and if it cannot, lower our own ambitions rather than discovering it
// at the worst moment.
//
// Every player costs one descriptor and every running room costs another, on
// top of the listeners, the signalfd, the epoll set, the log socket and the
// message queue. Without this check the failure mode is a bare accept()
// returning EMFILE somewhere around the thousandth connection, which looks like
// a mysterious refusal rather than a configuration problem.
//
// The shape is Redis's adjustOpenFilesLimit (server.c:2647): ask for what we
// want, and if the kernel refuses, walk the request down until it accepts,
// never ending up below where we started. Then clamp our own limits to what we
// actually got and say so, three separate times, so an operator can see what
// was asked for, what the OS allowed, and what we settled on. Running with a
// smaller capacity is acceptable; accepting players we cannot serve is not.
#define FD_RESERVE 32          // listeners, signalfd, epoll, log socket, mq, log files
static void adjust_fd_limit(void){
    struct rlimit lim;
    if (getrlimit(RLIMIT_NOFILE, &lim) < 0){
        fprintf(stderr, "tetrisd: cannot read the open-file limit: %s\n",
                strerror(errno));
        return;
    }

    rlim_t want = (rlim_t)g_cfg.max_clients + (rlim_t)g_cfg.max_rooms + FD_RESERVE;
    rlim_t have = lim.rlim_cur;

    if (have < want){
        rlim_t best = want;
        while (best > have){
            struct rlimit try_lim = { .rlim_cur = best, .rlim_max = best };
            if (setrlimit(RLIMIT_NOFILE, &try_lim) == 0) break;
            if (best < 16){ best = have; break; }
            best -= 16;                      // walk it down until the kernel agrees
        }
        if (best < have) best = have;        // never end up worse than we started
        have = best;
    }

    // Clamp what we will accept to what we can actually serve.
    int serveable = (int)have - FD_RESERVE - g_cfg.max_rooms;
    if (serveable < 1){
        fprintf(stderr, "tetrisd: an open-file limit of %llu is too low to run at all; "
                        "raise it with ulimit -n\n", (unsigned long long)have);
        exit(1);
    }
    if (g_cfg.max_clients > serveable){
        fprintf(stderr, "tetrisd: max_clients %d needs %llu descriptors\n",
                g_cfg.max_clients, (unsigned long long)want);
        fprintf(stderr, "tetrisd: the OS allows %llu\n", (unsigned long long)have);
        fprintf(stderr, "tetrisd: max_clients reduced to %d to fit; "
                        "raise 'ulimit -n' if you need more\n", serveable);
        g_cfg.max_clients = serveable;
    }
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

    // Refuse to start if another tetrisd already owns this control socket.
    //
    // bind() cannot tell us, because the unlink() below removes whatever is
    // sitting there: a second daemon would silently take the address from the
    // first, which then keeps running attached to a socket no tetrisctl can
    // reach any more. A different listen_port is no protection either, since
    // the TCP listener and the control socket are independent addresses and
    // only the former can report EADDRINUSE.
    //
    // The AF_UNIX way to ask "is anyone actually there?" is to try to connect.
    // A live listener accepts, a socket file left behind by a crashed run
    // refuses with ECONNREFUSED, and a path with nothing at it gives ENOENT.
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0){
        int live = (connect(probe, (struct sockaddr *)&addr, sizeof addr) == 0);
        close(probe);
        if (live){
            fprintf(stderr, "tetrisd: another tetrisd already owns %s\n", cfg->ctl_path);
            close(fd); return -1;
        }
    }

    unlink(cfg->ctl_path);

    // Create the socket node with the permissions ctl_perm asks for.
    //
    // bind() is what creates the node, and it applies the process umask like
    // any other file creation. Without this the mode is whatever mask we happen
    // to have inherited, which leaves the control plane readable and writable
    // by every local user: /shutdown takes no credentials, so anyone who can
    // open the socket can stop a running tournament.
    //
    // The mask is set around bind() rather than chmod()ing afterwards because
    // bind() and chmod() are two steps, and between them the socket exists at
    // the wrong mode and will accept a connection from anyone who gets there
    // first. Setting the mask makes the node arrive correct instead.
    mode_t old_umask = umask((mode_t)(0777 & ~cfg->ctl_perm));
    int rc = bind(fd, (struct sockaddr *)&addr, sizeof addr);
    umask(old_umask);
    if (rc < 0){
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
    // A 429 without Retry-After tells the client it was throttled but not for
    // how long, so its only options are to guess or to spin. The window is one
    // second and the header is measured in seconds, so 1 is the exact wait.
    if (status == HTTTP_429_TOO_MANY_REQUESTS)
        htttp_builder_add_header(&b, "Retry-After", "1");
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

// JOIN carrying an X-Spectate header: attach as a watcher, not a player.
// The spectator gets no seat and no game. It goes into the room's STATE
// fan-out and receives exactly the frames a player receives. 404 when the
// room does not exist (see room_spectate for why there is no auto-create),
// 409 when every spectator slot is taken.
static int do_spectate(client_t *c, const char *rid){
    if (c->room >= 0)
        return send_response(c, HTTTP_409_CONFLICT,
                             "{\"error\": \"already in a room\"}", NULL);
    int status = 0;
    room_t *r = room_spectate(rid, c, &status);
    if (r == NULL)
        return send_response(c, (htttp_status_t)status,
                             status == 404 ? "{\"error\": \"no such room\"}"
                                           : "{\"error\": \"spectator slots full\"}", NULL);
    slog("info", "room %s: %s spectating", r->id, c->player_id);
    char body[128];
    snprintf(body, sizeof body, "{\"room\": \"%s\", \"spectating\": true}", r->id);
    return send_response(c, HTTTP_200_OK, body, c->player_id);
}

static int do_join(client_t *c, const char *rid){
    // A spectator has to LEAVE (or disconnect) before it can play. Letting
    // this through would seat the client in a room while it still occupies
    // another room's spectator slot, and that slot would dangle once the
    // client disconnects: a freed pointer in the broadcast fan-out.
    if (c->room >= 0 && c->seat < 0)
        return send_response(c, HTTTP_409_CONFLICT,
                             "{\"error\": \"leave the spectated room first\"}", NULL);
    if (c->room != -1)
        return send_response(c, HTTTP_409_CONFLICT, "{\"error\": \"already in a room\"}", NULL);

    int status = 0;
    room_t *r = room_join(rid, c, &status);
    if (r == NULL)
        return send_response(c, (htttp_status_t)status,
                             status == 409 ? "{\"error\": \"room is full\"}"
                                           : "{\"error\": \"room table full\"}", NULL);

    if (status == 201)
        slog("info", "room %s created by %s", r->id, c->player_id);
    slog("info", "room %s: %s joined (%d player%s)", r->id, c->player_id,
         r->nplayers, r->nplayers == 1 ? "" : "s");

    // A player who joined a room that is already running has missed the
    // seeding that START does, so their seat still holds whatever the previous
    // occupant left behind. Seed it now, at the tick they actually arrived.
    //
    // The replay record matters as much as the game state. A reader
    // reconstructs a player by starting from their SEED and replaying forward,
    // so a seed recorded at the wrong tick would rebuild a board that never
    // existed. Recording the room's current tick is what lets a late joiner be
    // replayed correctly alongside players who were there from the start.
    if (r->started && c->seat >= 0){
        static uint32_t late_seed_counter = 0;
        pthread_mutex_lock(&r->mu);
        uint32_t seed = (uint32_t)getpid() * 2654435761u + 0x9E3779B9u
                      + ++late_seed_counter;
        tb_init(&r->games[c->seat], seed);
        r->seeds[c->seat] = seed;        // snapshots re-state this; see rooms.h
        // This room's rate, not the live config's: see the note in rooms.h.
        // A reload between START and this join would otherwise hand the late
        // arrival a lock delay that does not match the ticker they are joining.
        tb_set_lock_delay(&r->games[c->seat], (uint32_t)(r->tick_hz / 2));
        r->pending[c->seat] = TB_INPUT_NONE;
        uint64_t at_tick = r->ticks;
        pthread_mutex_unlock(&r->mu);

        slog("info", "room %s: SEED seat=%d player=%s seed=%u (joined at tick %llu)",
             r->id, c->seat, c->player_id, seed, (unsigned long long)at_tick);
        rlog("E %llu %llu %s %s SEED %u", mono_ns(),
             (unsigned long long)at_tick, r->id, c->player_id, seed);
    }

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
    if (c->seat < 0)
        return send_response(c, HTTTP_403_FORBIDDEN, "{\"error\": \"spectators cannot start\"}", NULL);
    if (r->started)
        return send_response(c, HTTTP_409_CONFLICT, "{\"error\": \"already started\"}", NULL);

    // Set up one authoritative game for each seated player, each with its own
    // random seed. We write the seed to the log on purpose, because the replay
    // feature later on needs the seed together with the inputs to rebuild the
    // exact same game.
    // The rate this room will actually tick at, resolved once, here, and then
    // stored on the room. Everything downstream (the lock delay below, the
    // timerfd period, a late joiner's lock delay in do_join) reads it from the
    // room rather than from g_cfg, so a SIGHUP reload cannot leave a running
    // room's seats disagreeing with its ticker. This is also the single place a
    // nonsensical configured value is corrected, which keeps a negative tick_hz
    // from reaching the unsigned conversion in tb_set_lock_delay.
    int hz = (g_cfg.tick_hz > 0) ? g_cfg.tick_hz : 20;

    static uint32_t seed_counter = 0;
    pthread_mutex_lock(&r->mu);
    r->tick_hz = hz;
    for (int i = 0; i < ROOM_MAX_PLAYERS; i++){
        if (r->players[i] == NULL) continue;
        uint32_t seed = (uint32_t)getpid() * 2654435761u + ++seed_counter;
        tb_init(&r->games[i], seed);
        r->seeds[i] = seed;              // snapshots re-state this; see rooms.h
        // The engine counts lock delay in ticks and has no clock of its own, so
        // the wall-clock figure has to be converted here, where the rate is
        // known. Half a second at this room's rate, which is 10 ticks at the
        // default tick_hz of 20.
        tb_set_lock_delay(&r->games[i], (uint32_t)(hz / 2));
        r->pending[i] = TB_INPUT_NONE;
        slog("info", "room %s: SEED seat=%d player=%s seed=%u",
             r->id, i, r->players[i]->player_id, seed);
        // The replay record. Everything a reader needs to rebuild this board
        // starts here: the same seed fed to tb_init produces the same piece
        // sequence, because the engine's PRNG is part of its state and it
        // never calls rand(). Tick 0 is the moment START was accepted.
        rlog("E %llu 0 %s %s SEED %u",
             mono_ns(), r->id, r->players[i]->player_id, seed);
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
    long period_ns = 1000000000L / hz;
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_interval.tv_sec  = period_ns / 1000000000L;
    its.it_interval.tv_nsec = period_ns % 1000000000L;
    its.it_value = its.it_interval;           // first tick one period from now
    timerfd_settime(tfd, 0, &its, NULL);
    r->timer_fd = tfd;
    ep_add(g_ep, tfd);

    slog("info", "room %s: STARTED by %s (%d players, %d Hz)",
         r->id, c->player_id, r->nplayers, hz);
    return send_response(c, HTTTP_200_OK, "{\"ok\": true}", NULL);
}

// Map a MOVE/ROTATE/DROP/HOLD request onto a tetrisbrain input.
// -1 = invalid body.
//
// HOLD is checked before the body test because it is the one input that
// carries no body at all: the method IS the whole instruction, so requiring
// a body would reject every well-formed HOLD.
static int map_input(htttp_method_t m, const char *body, size_t blen){
    if (m == HTTTP_METHOD_HOLD) return TB_INPUT_HOLD;
    if (body == NULL) return -1;
    if (m == HTTTP_METHOD_MOVE){
        if (blen == 4 && strncmp(body, "LEFT", 4)  == 0) return TB_INPUT_LEFT;
        if (blen == 5 && strncmp(body, "RIGHT", 5) == 0) return TB_INPUT_RIGHT;
    } else if (m == HTTTP_METHOD_ROTATE){
        // Both directions are real now. Check CCW first: "CW" is a prefix of
        // nothing here, but the lengths differ, so comparing the longer literal
        // first keeps the two cases from ever being confused.
        if (blen == 3 && strncmp(body, "CCW", 3) == 0) return TB_INPUT_ROTATE_CCW;
        if (blen == 2 && strncmp(body, "CW", 2)  == 0) return TB_INPUT_ROTATE_CW;
        // A third body rather than a third method: 180 is a rotation, and the
        // brain has always had TB_INPUT_ROTATE_180 for it.
        if (blen == 3 && strncmp(body, "180", 3) == 0) return TB_INPUT_ROTATE_180;
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
    // As much a bounds check as a permission check: a spectator's seat is
    // -1, and pending[c->seat] below would write before the array.
    if (c->seat < 0)
        return send_response(c, HTTTP_403_FORBIDDEN, "{\"error\": \"spectators cannot play\"}", NULL);
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

// How many requests one connection may send per second before it is throttled.
//
// A human player at the default 20 Hz tick can usefully send about 20 inputs a
// second; anything beyond that is discarded by the one-pending-input-per-seat
// rule anyway (see do_input). 60 leaves room for a fast player plus JOIN/START
// traffic while still catching a client that has gone into a send loop, which
// is what actually costs the event loop time: every request is parsed,
// dispatched and answered on the single epoll thread.
#define CLIENT_MAX_REQ_PER_SEC 60

// Returns 1 if this request should be refused with 429.
//
// A fixed one-second window rather than a sliding one or a token bucket: the
// state is two fields per client and the failure mode of a fixed window (a
// burst spanning a window boundary passes) is harmless here, because the
// limit exists to stop a runaway client from monopolising the loop, not to
// enforce a precise rate.
static int rate_limited(client_t *c){
    time_t now = time(NULL);
    if (now != c->rl_window){       // a new second: reset the window
        c->rl_window = now;
        c->rl_count  = 0;
    }
    return ++c->rl_count > CLIENT_MAX_REQ_PER_SEC;
}

static void dispatch_request(client_t *c, const htttp_msg_t *msg){
    char rid[ROOM_ID_MAX], pid[CLIENT_ID_MAX];
    int status;

    // Throttling happens before parsing the path or checking identity, because
    // the point is to spend as little of the event loop's time as possible on
    // a client that is flooding it.
    if (rate_limited(c)){
        slog("warn", "%s: rate limited (over %d requests/second)",
             c->player_id, CLIENT_MAX_REQ_PER_SEC);
        send_response(c, HTTTP_429_TOO_MANY_REQUESTS,
                      "{\"error\": \"too many requests\"}", NULL);
        return;
    }

    int pathkind = parse_room_path(msg->path, rid, sizeof rid, pid, sizeof pid);

    switch (msg->method){
    case HTTTP_METHOD_JOIN: {
        // JOIN is the request that hands out the identity, so it does not need
        // a Player-Id header yet. With an X-Spectate header it attaches the
        // client as a watcher instead of seating it. Same verb, same path: a
        // spectator "joins the room" in every sense except holding a seat, and
        // a new method would ripple through parser, dispatch and client for no
        // added meaning.
        size_t xlen = 0;
        int spectate = htttp_find_header(msg, "X-Spectate", &xlen) != NULL;
        status = (pathkind != 0)
               ? send_response(c, HTTTP_400_BAD_REQUEST, "{\"error\": \"bad path\"}", NULL)
               : spectate ? do_spectate(c, rid)
                          : do_join(c, rid);
        break;
    }

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
    case HTTTP_METHOD_DROP:
    case HTTTP_METHOD_HOLD: {
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

    // one line per request, whatever happened to it
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
    // We ask accept() for the peer address, because the per-IP limit below
    // needs it. Passing NULL here (as this did before) simply discards it.
    struct sockaddr_in peer;
    socklen_t peerlen = sizeof peer;
    int cfd = accept(listen_fd, (struct sockaddr *)&peer, &peerlen);
    if (cfd < 0) return;

    // Per-IP admission control. This happens BEFORE the handshake, and the
    // ordering is the whole point of the check rather than an optimisation.
    // tetrissh_handshake_server does an RSA signature, so every connection we
    // let through costs real CPU before the peer has proved anything at all.
    // A connect flood from one host would otherwise be cheap for the attacker
    // and expensive for us. Redis makes the same argument in
    // acceptCommonHandler (networking.c:1658): "Admission control will happen
    // before a client is created and connAccept() called, because we don't
    // want to even start transport-level negotiation if rejected."
    //
    // We close without sending anything back. There is no way to send a
    // meaningful error yet, because the session that would encrypt it does not
    // exist until the handshake completes. Redis notes the identical situation
    // for its TLS listeners: "no handshake was done yet so nothing is written
    // and the connection will just drop."
    //
    // 0 disables the limit, which is what you want for a local stress test
    // where every connection legitimately comes from 127.0.0.1.
    uint32_t peer_addr = (uint32_t)peer.sin_addr.s_addr;
    if (g_cfg.max_conns_per_ip > 0 &&
        client_count_addr(peer_addr) + pending_count_addr(peer_addr)
            >= g_cfg.max_conns_per_ip){
        char ipbuf[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ipbuf, sizeof ipbuf);
        atomic_fetch_add(&rejected_conns, 1);
        slog("warn", "connection from %s refused: already at the per-IP limit of %d",
             ipbuf, g_cfg.max_conns_per_ip);
        close(cfd);
        return;
    }

    // A starting timeout, so the descriptor is never left with none at all
    // while it waits in the admission queue. It is only a floor: libtetrissh
    // re-arms both options before every syscall from the deadline it stamps at
    // the start of each handshake or frame, which is what actually bounds the
    // work. See the note at the top of this file.
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    // Park it. The handshake is deliberately NOT run here; see the note above
    // the queue. A full queue is refused immediately rather than queued and
    // forgotten, because a client waiting behind an unbounded backlog times out
    // anyway and we would rather tell it now.
    if (g_npending_hs >= PENDING_HS_MAX){
        atomic_fetch_add(&hs_refused_full, 1);
        slog("warn", "admission queue full (%d), refusing a connection",
             PENDING_HS_MAX);
        close(cfd);
        return;
    }
    // Park it and let epoll tell us when this peer actually starts speaking.
    // Until then it costs us a queue slot and nothing else.
    if (ep_add(g_ep, cfd) < 0){
        slog("error", "epoll add failed for pending fd %d: %s", cfd, strerror(errno));
        close(cfd);
        return;
    }
    g_pending_hs[g_npending_hs].fd        = cfd;
    g_pending_hs[g_npending_hs].addr      = peer_addr;
    g_pending_hs[g_npending_hs].parked_ms = (long long)(mono_ns() / 1000000ull);
    g_npending_hs++;
    atomic_fetch_add(&hs_queued, 1);
}

// Complete one parked handshake. Returns 0 if the client is now connected.
//
// This is the code that used to live inline in the accept handler; nothing
// about the handshake itself changed, only when it runs.
static int finish_handshake(int cfd, uint32_t peer_addr){
    tetrissh_session_t *sess = tetrissh_session_alloc();
    if (sess == NULL){ close(cfd); return -1; }

    if (tetrissh_handshake_server(sess, cfd, g_cfg.cert_path, g_cfg.key_path) != 0){
        slog("warn", "handshake FAILED on fd %d: %s", cfd, tetrissh_strerror(sess));
        tetrissh_session_free(sess);
        close(cfd);
        return -1;
    }

    client_t *c = client_add(cfd, sess, peer_addr);
    if (c == NULL){                    // fd beyond registry bound
        slog("warn", "fd %d beyond client table, refusing", cfd);
        tetrissh_session_free(sess);
        close(cfd);
        return -1;
    }
    if (ep_add(g_ep, cfd) < 0){
        slog("error", "epoll add failed for fd %d: %s", cfd, strerror(errno));
        client_remove(cfd);
        close(cfd);
        return -1;
    }
    slog("info", "client fd %d: handshake OK, assigned id %s (%d connected)",
         cfd, c->player_id, client_count());
    return 0;
}

// A parked connection has become readable, so its peer has started talking and
// the handshake can run without waiting on anyone.
//
// The budget still applies, and still means what it meant before: at most this
// many handshakes complete per pass of the loop, so a crowd arriving at once
// cannot spend the whole pass on RSA while rooms are waiting to tick. What
// changed is only which connections are eligible. Anything held back stays in
// the queue and in the epoll set, and since the set is level-triggered it is
// offered again on the next pass; nothing needs to remember it.
static void handle_pending_handshake(int fd, int *budget_left){
    int i = pending_index(fd);
    if (i < 0) return;
    if (*budget_left <= 0) return;              // next pass; epoll will re-offer
    (*budget_left)--;

    uint32_t addr = g_pending_hs[i].addr;
    pending_remove(i);

    // finish_handshake re-adds the fd on success and closes it on failure, so
    // it must not already be in the set when either of those happens.
    epoll_ctl(g_ep, EPOLL_CTL_DEL, fd, NULL);
    finish_handshake(fd, addr);
}

// Close out connections that were accepted and then never said anything.
//
// With the handshake driven by readability, a silent peer no longer stalls the
// loop, but it does hold a queue slot, and PENDING_HS_MAX of them would still
// deny the queue to real players. This is the other half of that defence: a
// peer gets PENDING_HS_TIMEOUT_MS to send its first byte, and the per-IP cap
// (which counts parked connections as well as established ones) bounds how
// many any single host can be sitting on meanwhile.
static void reap_stale_handshakes(void){
    long long now = (long long)(mono_ns() / 1000000ull);
    for (int i = g_npending_hs - 1; i >= 0; i--){
        if (now - g_pending_hs[i].parked_ms < PENDING_HS_TIMEOUT_MS)
            continue;
        int fd = g_pending_hs[i].fd;
        slog("warn", "fd %d: connected but sent nothing in %d ms, dropping",
             fd, PENDING_HS_TIMEOUT_MS);
        pending_remove(i);
        epoll_ctl(g_ep, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
    }
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

// Which visible cells the active piece would occupy if hard-dropped now.
//
// This is the server's copy of the projection tetrisu draws offline, and it
// lives here because the client cannot reproduce it: tb_render MERGES the
// active piece into the grid, so by the time a board reaches the wire there
// is nothing marking which cells are falling and which are locked. Sending
// the landing position as part of the board is what lets a client that owns
// no game state still draw a ghost.
static void mark_ghost(const tb_game *g, bool ghost[TB_ROWS][TB_COLS]){
    memset(ghost, 0, TB_ROWS * TB_COLS * sizeof(bool));
    if (g->game_over) return;
    int gy = tb_ghost_y(g);
    const tb_position *p = &tb_positions[g->active.type][g->active.orientation];
    for (int i = 0; i < TB_CELLS_PER_PIECE; i++){
        int x = g->active.origin.x + p->pos[i].x;
        int y = gy + p->pos[i].y;
        if (x >= 0 && x < TB_COLS && y >= 0 && y < TB_ROWS)
            ghost[y][x] = true;
    }
}

// Draw one player's board into dst, in the text format we send as the STATE
// body. The first line has the player id, their score, level, lines and a
// game-over flag, then the NEXT and HOLD piece types (-1 for none) and
// whether hold is spent this turn. After that come 20 rows of 10 columns
// each, where a '.' is an empty cell, a digit marks a filled cell, and 'g'
// marks an empty cell the active piece would land on.
static size_t render_board(char *dst, size_t cap, const char *pid, const tb_game *g){
    int8_t cells[TB_ROWS][TB_COLS];
    tb_render(g, cells);
    bool ghost[TB_ROWS][TB_COLS];
    mark_ghost(g, ghost);
    size_t off = 0;
    off += (size_t)snprintf(dst + off, cap - off,
                            "player %s score %u level %u lines %u over %d "
                            "next %d hold %d held %d\n",
                            pid, g->score, g->level, g->lines_total,
                            g->game_over ? 1 : 0,
                            tb_next_piece(g), (int)g->hold,
                            g->held_this_turn ? 1 : 0);
    for (int y = 0; y < TB_ROWS && off + TB_COLS + 2 < cap; y++){
        for (int x = 0; x < TB_COLS; x++){
            int8_t c = cells[y][x];
            // tb_render's contract: -1 is empty, 0..6 is a piece type (locked
            // or active), anything larger is garbage. A truthiness test here
            // reads that contract backwards twice: -1 (empty) is truthy, so
            // every empty cell rendered as a block, and type 0 (the O piece)
            // is falsy, so O pieces rendered as holes. No automated harness
            // ever LOOKED at a drawn frame, which is how it survived until
            // the first visual pass. '1'..'7' matches the client renderer's
            // colour pairs; '8' is its garbage/fallback pair.
            // Ghost only ever replaces an EMPTY cell, so a real block is
            // never hidden by the projection of the piece that will land on
            // it -- the same precedence the offline renderer uses.
            dst[off++] = (c >= 0)   ? ((c <= 6) ? (char)('1' + c) : '8')
                       : ghost[y][x] ? 'g'
                                     : '.';
        }
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
    client_t *recipients[ROOM_MAX_PLAYERS + ROOM_MAX_SPECS];
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
        bool     over_before  = r->games[i].game_over;

        // The tick this player's input lands on. r->ticks has already been
        // advanced past every catch-up expiration, so the first of them, which
        // is the only one that carries input, is this many ticks back.
        uint64_t input_tick = r->ticks - expirations + 1;

        if (r->pending[i] != TB_INPUT_NONE && !r->games[i].game_over)
            rlog("E %llu %llu %s %s INPUT %s", mono_ns(),
                 (unsigned long long)input_tick, r->id,
                 r->players[i]->player_id, input_name(r->pending[i]));

        for (uint64_t e = 0; e < expirations; e++){
            // The player's queued input is applied on the first tick only. Any
            // extra catch-up ticks just apply gravity with no input.
            tb_input in = (e == 0) ? r->pending[i] : TB_INPUT_NONE;
            if (!r->games[i].game_over)
                tb_tick(&r->games[i], in);
        }

        int cleared = (int)(r->games[i].lines_total - lines_before);
        garbage_rows += garbage_rows_for(cleared);   // see the table in garbage.h

        // CLEAR and OVER are not needed to reconstruct the board, since
        // replaying SEED plus INPUT plus GARBAGE reproduces both. They are
        // recorded so a reader can check its reconstruction against what the
        // server actually saw, which is how you catch a replay that has
        // silently diverged.
        if (cleared > 0)
            rlog("E %llu %llu %s %s CLEAR %d", mono_ns(),
                 (unsigned long long)r->ticks, r->id,
                 r->players[i]->player_id, cleared);
        if (r->games[i].game_over && !over_before)
            rlog("E %llu %llu %s %s OVER", mono_ns(),
                 (unsigned long long)r->ticks, r->id,
                 r->players[i]->player_id);

        // A periodic full board, plus the seat's seed. The board is what lets
        // a reader detect (and resync from) a reconstruction that diverged
        // because a record was dropped; the seed is re-stated here so that
        // replay does not hinge on the one write-once SEED record surviving
        // the START burst. See the format comment above rlog.
        //
        // The phase is staggered by room. Rooms started in the same second all
        // reach ticks % interval == 0 within a few milliseconds of each other,
        // so an unstaggered schedule fires every room's snapshots as one burst
        // into a datagram queue holding about ten. The drain order is stable
        // too, so the SAME seats lose their snapshots every interval, some of
        // them losing all ten. Offsetting each room by its table index turns
        // the burst into a trickle at no change in per-seat rate.
        if (g_cfg.snapshot_interval > 0 &&
            r->ticks % (uint64_t)g_cfg.snapshot_interval ==
                (uint64_t)(room_index(r) % g_cfg.snapshot_interval)){
            char hex[TB_ROWS * TB_COLS + 1];
            board_hex(&r->games[i], hex, sizeof hex);
            rlog("S %llu %llu %s %s %d %d %s %u", mono_ns(),
                 (unsigned long long)r->ticks, r->id,
                 r->players[i]->player_id, TB_COLS, TB_ROWS, hex,
                 r->seeds[i]);
        }

        r->pending[i] = TB_INPUT_NONE;
        blen += render_board(body + blen, sizeof body - blen,
                             r->players[i]->player_id, &r->games[i]);
        recipients[nrec++] = r->players[i];
    }
    // Spectators receive the identical frame, appended to the same list so
    // there is one send loop and one failure policy: a spectator that stops
    // reading is dropped by the same deadline that drops a slow player,
    // because a stalled watcher must not stall the room.
    for (int i = 0; i < ROOM_MAX_SPECS; i++)
        if (r->specs[i] != NULL)
            recipients[nrec++] = r->specs[i];
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

    int failed[ROOM_MAX_PLAYERS + ROOM_MAX_SPECS], nfailed = 0;
    for (int i = 0; i < nrec; i++){
        if (tetrissh_send(recipients[i]->sess, recipients[i]->fd,
                          (unsigned char *)wire, wlen) != 0)
            failed[nfailed++] = recipients[i]->fd;
    }
    free(wire);

    // If a client cannot take a STATE frame within libtetrissh's per-frame
    // deadline, we disconnect it. Each STATE frame is the complete current
    // board, so an old one is not worth keeping, and there is nothing to gain
    // from queueing frames for a slow reader.
    //
    // The deadline is doing real work here, not just tidying up after a dead
    // client. These sends run on the event-loop thread, one after another, so
    // a player whose client has stopped reading (a suspended tetrisu is the
    // easy case) backs up their socket's send buffer and blocks this loop. The
    // bound is what keeps that one player's problem from stopping every other
    // room's ticker as well.
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
        uint64_t at_tick = dst->ticks;
        for (int i = 0; i < ROOM_MAX_PLAYERS; i++){
            if (dst->players[i] == NULL) continue;
            if (dst->games[i].game_over) continue;   // already out, leave them
            tb_inject_garbage(&dst->games[i], gm.rows, gm.hole_pattern);
            hit++;
            // Garbage is the one input to a board that does not come from its
            // own player, so replay cannot derive it and it has to be recorded.
            // The hole pattern goes in the record for the same reason the
            // caller supplies it to the engine: it is the thing that would
            // otherwise be random, and recording it keeps replay exact.
            rlog("E %llu %llu %s %s GARBAGE %u %04x", mono_ns(),
                 (unsigned long long)at_tick, dst->id,
                 dst->players[i]->player_id,
                 (unsigned)gm.rows, (unsigned)gm.hole_pattern);
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

// Append to the reply buffer, truncating rather than overflowing.
//
// vsnprintf returns the length it WOULD have written, not the length it did.
// Advancing off by that return value lets off grow past the end of the buffer
// once it fills; the next call then computes `sizeof j->buf - j->off` as a
// size_t subtraction that underflows to a huge value, and vsnprintf writes
// freely past the end of a stack buffer. Reproduced with 90 rooms: a 5392-byte
// body into 4096 bytes, overwriting the htttp_msg_t in the same frame.
//
// So: refuse to write when the buffer is full, and advance by what was
// actually stored. The reply truncates instead. Returning the full list at
// that size would need a bigger buffer or a streamed body, which is a larger
// change than this bug deserves.
static void jb_append(struct jbuf *j, const char *fmt, ...){
    size_t space = sizeof j->buf - j->off;
    if (space == 0) return;                  // full; another append would underflow
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->off, space, fmt, ap);
    va_end(ap);
    if (n < 0) return;                       // encoding error: leave the buffer as it was
    j->off += ((size_t)n >= space) ? space - 1 : (size_t)n;   // what was stored, not what was wanted
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
                      "\"garbage_sent\": %lu, \"garbage_dropped\": %lu, "
                      "\"rejected_conns\": %lu, \"hs_queued\": %lu, "
                      "\"hs_pending\": %d, \"hs_refused_full\": %lu}",
                  uptime_seconds(), rooms_count(), client_count(),
                  total_dropped(),
                  atomic_load(&garbage_sent), atomic_load(&garbage_dropped),
                  atomic_load(&rejected_conns), atomic_load(&hs_queued),
                  g_npending_hs, atomic_load(&hs_refused_full));
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
    rooms_init(g_cfg.max_rooms, g_cfg.max_players_per_room);
    adjust_fd_limit();
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

    // Remember the names we actually bound, so shutdown removes those and not
    // whatever the config happens to say by then.
    //
    // A SIGHUP reload replaces g_cfg wholesale, but the listeners and the queue
    // keep the addresses they were created with (the reload log line says so).
    // Reading g_cfg at shutdown therefore unlinks the NEW paths: names this
    // process never created, which may well belong to a second daemon started
    // against the edited config, while leaving its own socket, pid file and
    // message queue behind. Snapshotting the three names at the point of
    // creation keeps "what we made" and "what we destroy" the same set.
    char bound_ctl_path[RC_PATHLEN];
    char bound_pid_path[RC_PATHLEN];
    char bound_garbage_mq[RC_PATHLEN];
    snprintf(bound_ctl_path,   sizeof bound_ctl_path,   "%s", g_cfg.ctl_path);
    snprintf(bound_pid_path,   sizeof bound_pid_path,   "%s", g_cfg.pid_path);
    snprintf(bound_garbage_mq, sizeof bound_garbage_mq, "%s", g_cfg.garbage_mq);

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
    write_pidfile(bound_pid_path);

    // 9. Set up the log ring buffer and start the logshipper thread. This must
    //    happen after daemonizing, because fork() only keeps the thread that
    //    called it. A thread started before the fork would not exist inside the
    //    daemon.
    ring_init(&g_ring);
    atomic_init(&dropped_send, 0);
    atomic_init(&shipper_stop, 0);
    atomic_init(&garbage_sent, 0);
    atomic_init(&garbage_dropped, 0);
    atomic_init(&rejected_conns, 0);
    atomic_init(&hs_queued, 0);
    atomic_init(&hs_refused_full, 0);
    pthread_t shipper;
    if (pthread_create(&shipper, NULL, logshipper, NULL) != 0){
        perror("pthread_create");
        close(listen_fd); close(ctl_fd); mq_close(mq); return 1;
    }

    // 10. Create the signalfd. This turns the signals we blocked earlier into
    //     something we can read from as if it were a file, so the event loop
    //     can wait on them together with the sockets.
    //     SFD_NONBLOCK matters as much as the signalfd itself. epoll telling us
    //     the descriptor is readable is not a promise that a full siginfo is
    //     waiting, and a blocking read() that guesses wrong parks the one
    //     thread that runs every room, with no timeout to recover from. The
    //     handler already copes with a short read by returning, so the only
    //     thing missing was the flag that lets the read fail instead of block.
    int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
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
        struct epoll_event events[EPOLL_BATCH];
        // Parked connections no longer need polling: each one is in the epoll
        // set and announces itself when its peer speaks. A bounded wait is
        // still used while any are parked, but only so the reaper below runs
        // on a quiet server; it is not how progress is made.
        int timeout_ms = (g_npending_hs > 0) ? 1000 : -1;
        int n = epoll_wait(g_ep, events, EPOLL_BATCH, timeout_ms);
        if (n < 0){
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        // Handshakes completed on this pass. See handle_pending_handshake.
        // ctl_fd is tested before the client branch, so an admin command is serviced in teh same pass as a flood
        int hs_budget = g_cfg.handshake_budget > 0 ? g_cfg.handshake_budget : 32;

        for (int i = 0; i < n && running; i++){
            int fd = events[i].data.fd;
            if      (fd == listen_fd) handle_new_client(listen_fd);
            else if (fd == sig_fd)    handle_signal_event(sig_fd, rc_path, &running);
            else if (fd == ctl_fd)    handle_ctl(ctl_fd, &running);
            else if (fd == (int)mq)   handle_garbage(mq);
            else if (client_get(fd) != NULL) handle_client_data(fd);
            else if (pending_index(fd) >= 0) handle_pending_handshake(fd, &hs_budget);
            else {
                room_t *r = room_by_timerfd(fd);
                if (r != NULL) handle_room_tick(r);
                else epoll_ctl(g_ep, EPOLL_CTL_DEL, fd, NULL);   // stale fd safety
            }
        }

        if (g_npending_hs > 0) reap_stale_handshakes();
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

    // Connections that were accepted but never finished their handshake are
    // not clients yet, so phase 2 below will not find them. They own a
    // descriptor and nothing else, and there is no session to tear down.
    int parked = g_npending_hs;
    for (int i = 0; i < g_npending_hs; i++){
        epoll_ctl(g_ep, EPOLL_CTL_DEL, g_pending_hs[i].fd, NULL);
        close(g_pending_hs[i].fd);
    }
    g_npending_hs = 0;
    slog("info", "tetrisd: phase 1: stopped accepting new clients"
                 " (%d unadmitted connection%s dropped)",
         parked, parked == 1 ? "" : "s");

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
    // so the join below guarantees that everything which REACHED the ring is
    // sent before we close the socket.
    //
    // It does not guarantee the four records above are among them, and the
    // distinction is worth being precise about. slog() reaches the ring through
    // ring_push, which uses trylock and drops the record outright when the
    // shipper happens to hold the mutex. Phase 2 pushes one record per
    // disconnected client in a tight burst against a shipper that takes the
    // lock every millisecond, so under load a share of those, and possibly a
    // phase line with them, are dropped exactly like any other record. That is
    // the deal the trylock buys: the game never waits on logging, and no record
    // is ever guaranteed. The drop counters are what make the loss visible.
    atomic_store(&shipper_stop, 1);
    pthread_join(shipper, NULL);

    close(g_ep);
    close(sig_fd);
    close(ctl_fd);
    mq_close(mq);
    mq_unlink(bound_garbage_mq);
    if (log_fd >= 0) close(log_fd);
    unlink(bound_ctl_path);
    unlink(bound_pid_path);
    ring_destroy(&g_ring);
    return 0;
}
