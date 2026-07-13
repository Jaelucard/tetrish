// tetrisd is the game server daemon.
//
// The Week-5 shape of this file is a real epoll event loop. It replaces the
// Week-4 version, which could only block on a single signalfd read. Now one
// loop on one thread handles every kind of event. This is the same idea as the
// event loop in Redis (see systems/redis/src/ae.c). The epoll set watches four
// file descriptors:
//
//   listen_fd  TCP            new game clients. For now we accept them and then
//                             politely close them, because the game protocol is
//                             added in a later week.
//   sig_fd     signalfd       SIGTERM, SIGHUP and SIGUSR1, arriving as readable events
//   ctl_fd     AF_UNIX STREAM the control plane: tetrisctl STATUS and SHUTDOWN
//   mq         POSIX mqueue   the Battle Royale garbage channel (a stub for now)
//
// The only other thread in the daemon is the logshipper. The game code pushes
// log records into a ring buffer, using a trylock so it drops the record
// instead of waiting (see ring.h). The logshipper thread then drains the ring
// and sends the records to tetrislogd. No game state is ever locked.
#include <stdio.h>           // snprintf, perror, fopen (pidfile)
#include <stdlib.h>          // exit codes
#include <string.h>          // memset, strlen, strcmp, strcspn
#include <stdint.h>          // uint16_t for the port
#include <unistd.h>          // read, write, close, unlink, getpid, usleep
#include <errno.h>           // errno, EINTR
#include <signal.h>          // sigprocmask, SIG_IGN (SIGPIPE)
#include <fcntl.h>           // O_CREAT, O_RDONLY, O_NONBLOCK for mq_open
#include <pthread.h>         // the logshipper thread
#include <stdatomic.h>       // drop counters shared between threads
#include <sys/signalfd.h>    // signals as file descriptors
#include <sys/epoll.h>       // THE event loop
#include <sys/socket.h>      // socket, bind, listen, accept, setsockopt
#include <sys/time.h>        // struct timeval for SO_RCVTIMEO
#include <netinet/in.h>      // sockaddr_in
#include <arpa/inet.h>       // inet_pton
#include <sys/un.h>          // sockaddr_un
#include <mqueue.h>          // POSIX message queue (mqd_t IS an fd on Linux)
#include "rc.h"              // config parser
#include "daemon.h"          // shared double-fork daemonize()
#include "ring.h"            // the log ring buffer (the one mutex)

#define GARBAGE_MSG_MAX 128  // must match the mq_msgsize attribute below

// --- logging: ring buffer + logshipper thread ------------------------------
// The game code calls logmsg(), which pushes into the ring buffer. That call
// never blocks and may drop the record. The logshipper thread is the only code
// that drains the ring and sends over the socket.
static int log_fd = -1;
static struct sockaddr_un log_addr;
static Ring g_ring;
static atomic_ulong dropped_send;    // how many times sendto() failed in the shipper
static atomic_int   shipper_stop;    // main sets this to 1, then the shipper drains the ring and exits

// Set up the non-blocking datagram socket that points at tetrislogd's log_ipc
// path, so the shipper can send log records to it.
static void log_init(const char *path){
    log_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    memset(&log_addr, 0, sizeof log_addr);
    log_addr.sun_family = AF_UNIX;
    strncpy(log_addr.sun_path, path, sizeof log_addr.sun_path - 1);
}

// Add one log line to the queue. The game code never touches the socket
// itself. It just pushes the record into the ring and returns right away. If
// the ring is busy or full the record is dropped and counted. We accept that
// logging can lose records, because the game tick must never be held up.
static void logmsg(const char *msg){
    ring_push(&g_ring, msg, strlen(msg));
}

// The logshipper thread. It is the only consumer of the ring, and the only
// code that ever sends on log_fd. It copies a batch of records out while the
// lock is held (that happens inside ring_pop_batch), and then sends them after
// the lock is released. We never hold the mutex across a slow system call.
#define SHIP_BATCH 64
static void *logshipper(void *arg){
    (void)arg;
    static char batch[SHIP_BATCH][RING_REC_MAX];   // static because only one shipper thread ever runs
    size_t lens[SHIP_BATCH];
    for (;;){
        size_t n = ring_pop_batch(&g_ring, batch, lens, SHIP_BATCH);
        if (n == 0){
            if (atomic_load(&shipper_stop))
                break;                   // the ring is empty and we have been told to stop
            usleep(1000);                // nothing to do, so sleep 1ms and look again
            continue;
        }
        for (size_t i = 0; i < n; i++){
            // We send and forget. If sendto fails, for example because
            // tetrislogd is slow (EAGAIN) or not running (ECONNREFUSED), the
            // record is lost. We just count it, and we never retry or block.
            if (sendto(log_fd, batch[i], lens[i], 0,
                       (struct sockaddr *)&log_addr, sizeof log_addr) < 0)
                atomic_fetch_add(&dropped_send, 1);
        }
    }
    return NULL;
}

// The total number of records lost so far, adding up both places we can lose
// them: records dropped at the ring, and records the shipper failed to send.
// tetrisd owns both counters.
static unsigned long total_dropped(void){
    return ring_dropped(&g_ring) + atomic_load(&dropped_send);
}

// --- listeners --------------------------------------------------------------

// Build the TCP listening socket. It binds to bind_addr and listen_port from
// the config, and then starts listening for connections.
static int tcp_listen(const Config *cfg){
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0){ perror("socket"); return -1; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);  // lets us bind again quickly after a restart

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)cfg->listen_port);          // convert the port from host to network byte order
    if (inet_pton(AF_INET, cfg->bind_addr, &sa.sin_addr) != 1){ // convert the text address like "127.0.0.1" into binary form
        fprintf(stderr, "tetrisd: bad bind address '%s'\n", cfg->bind_addr);
        close(fd); return -1;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0){ perror("bind"); close(fd); return -1; }
    if (listen(fd, cfg->tcp_backlog) < 0){ perror("listen"); close(fd); return -1; }
    return fd;
}

// Build the control-plane listener. This is an AF_UNIX stream socket at
// ctl_path. It uses the same server steps as TCP (socket, bind, listen), but
// its address is a path on the filesystem, so we unlink any leftover socket
// file from a previous run first, the same way tetrislogd does.
// Making this a separate socket from the TCP listener is the whole point. If a
// flood of game clients fills up the TCP backlog, this socket is not affected,
// so admin commands still get through.
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

    unlink(cfg->ctl_path);   // remove a leftover socket file from a previous run
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0){
        perror("bind(ctl)"); close(fd); return -1;
    }
    if (listen(fd, 8) < 0){ perror("listen(ctl)"); close(fd); return -1; }
    return fd;
}

// Write our process id to pid_path. We call this after daemonizing, so the id
// we write is the daemon's own id.
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

// --- epoll event handlers ----------------------------------------------------
// There is one small function per kind of file descriptor. The main loop only
// works out which fd became ready and then calls the matching handler here.
// This is the same pattern as the Redis event loop, in miniature.

// A new TCP game client has connected. There is no game protocol yet, but we
// still have to call accept(). epoll is level-triggered, which means that as
// long as a waiting connection is not accepted, listen_fd stays readable and
// the loop would spin at 100% CPU. So we accept it, write a short message, and
// close it.
static void handle_new_client(int listen_fd){
    int cfd = accept(listen_fd, NULL, NULL);
    if (cfd < 0) return;
    const char *msg = "tetrisd: game protocol not implemented yet\n";
    ssize_t wr = write(cfd, msg, strlen(msg));
    (void)wr;                       // the client may already be gone, which is fine
    close(cfd);
    logmsg("tetrisd: game client connected -> closed (no protocol yet)");
}

// One of the signals we blocked has arrived on the signalfd. We read it to find
// out which signal it was, and then act on it.
static void handle_signal_event(int sig_fd, const char *rc_path, Config *cfg, int *running){
    struct signalfd_siginfo si;
    ssize_t r = read(sig_fd, &si, sizeof si);
    if (r != sizeof si) return;

    switch (si.ssi_signo){
    case SIGTERM:
        // SIGTERM means "please shut down". We set running to 0 so the event
        // loop ends after this pass.
        logmsg("tetrisd: SIGTERM -> shutting down");
        *running = 0;
        break;
    case SIGHUP:
        // SIGHUP means "re-read the config file", which is how you reload
        // settings without restarting the daemon.
        if (rc_load(rc_path, cfg) == 0) logmsg("tetrisd: SIGHUP -> config reloaded");
        else                            logmsg("tetrisd: SIGHUP -> reload FAILED, keeping old config");
        break;
    case SIGUSR1: {
        // SIGUSR1 asks the daemon to dump its current state to the log. The
        // room and player counts are still zero because rooms come later. The
        // two drop counters are already real.
        char buf[160];
        snprintf(buf, sizeof buf,
                 "tetrisd: SIGUSR1 -> STATE DUMP: rooms=0 players=0 dropped_ring=%lu dropped_send=%lu",
                 ring_dropped(&g_ring), atomic_load(&dropped_send));
        logmsg(buf);
        break;
    }
    }
}

// A tetrisctl client has connected on the control socket. For now the control
// plane uses a very small line protocol: the client sends "STATUS\n" or
// "SHUTDOWN\n". It moves to the HTTTP format once libhtttp exists. We handle it
// right here in the loop, one step at a time, which is a deliberate
// simplification. The client is a trusted local admin tool that sends its one
// line straight away, and the receive timeout below means a stuck client can
// only hold us up for 2 seconds at most.
static void handle_ctl(int ctl_fd, int *running){
    int cfd = accept(ctl_fd, NULL, NULL);
    if (cfd < 0) return;

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    char req[256];
    ssize_t n = read(cfd, req, sizeof req - 1);
    if (n <= 0){ close(cfd); return; }
    req[n] = '\0';
    req[strcspn(req, "\r\n")] = '\0';       // cut the string at the first line ending, so we keep only the first line

    char reply[256];
    if (strcmp(req, "STATUS") == 0){
        // The room and player numbers are stubs for now, because rooms do not
        // exist yet. dropped_logs is already real. This is how a STATUS command
        // lets an admin read the drop counter from outside the daemon.
        snprintf(reply, sizeof reply,
                 "{\"rooms\": 0, \"players\": 0, \"dropped_logs\": %lu}\n",
                 total_dropped());
    } else if (strcmp(req, "SHUTDOWN") == 0){
        snprintf(reply, sizeof reply, "OK: shutting down\n");
        logmsg("tetrisd: SHUTDOWN via tetrisctl");
        *running = 0;
    } else {
        // The %.64s limits how much of the command we echo back. The command
        // text comes from outside, so we do not want to copy an unbounded
        // amount of it, and this also keeps the reply inside the buffer, which
        // the compiler checks for us.
        snprintf(reply, sizeof reply, "ERR: unknown command '%.64s'\n", req);
    }
    ssize_t wr = write(cfd, reply, strlen(reply));
    (void)wr;                               // a broken pipe here is fine, because we ignore SIGPIPE
    close(cfd);
}

// A Battle Royale garbage message has arrived. This is only a stub for now.
// The real garbage handling is added in Week 9.
static void handle_garbage(mqd_t mq){
    char gbuf[GARBAGE_MSG_MAX];
    ssize_t n = mq_receive(mq, gbuf, sizeof gbuf, NULL);
    if (n < 0) return;                      // nothing there to read right now
    char msg[64];
    snprintf(msg, sizeof msg, "tetrisd: got garbage event (%zd bytes)", n);
    logmsg(msg);
}

// Add one file descriptor to the epoll set so the loop is told when it becomes
// readable.
static int ep_add(int ep, int fd){
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events  = EPOLLIN;                   // wake us when there is data to read (level-triggered, the default)
    ev.data.fd = fd;
    return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

int main(int argc, char **argv){
    // 1. Load the config file (.tetrishrc).
    const char *rc_path = (argc > 1) ? argv[1] : ".tetrishrc";
    Config cfg;
    if (rc_load(rc_path, &cfg) != 0){
        fprintf(stderr, "tetrisd: failed to load configuration from %s\n", rc_path);
        return 1;
    }

    // 2. Deal with signals. First we ignore SIGPIPE. We now write to control
    //    clients (and later, game clients), and a client that disconnects
    //    before we reply must not be allowed to kill the daemon. With SIGPIPE
    //    ignored, that write simply fails with an error instead. Then we block
    //    the three signals we handle, so that they arrive as signalfd events.
    signal(SIGPIPE, SIG_IGN);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0){ perror("sigprocmask"); return 1; }

    // 3 and 4. Create both listening sockets before we daemonize. That way, if
    //    a socket fails to bind, the error is still visible on the terminal,
    //    and the open sockets carry over into the daemon after the fork.
    int listen_fd = tcp_listen(&cfg);
    if (listen_fd < 0) return 1;
    int ctl_fd = ctl_listen(&cfg);
    if (ctl_fd < 0){ close(listen_fd); return 1; }

    // 5. Open the Battle Royale garbage queue. The handler is still a stub. On
    //    Linux the queue handle is a real file descriptor, so we can add it
    //    straight into the epoll set. We open it non-blocking, so that an
    //    unexpected wakeup can never leave the loop stuck inside mq_receive.
    struct mq_attr attr = { .mq_flags = 0, .mq_maxmsg = 10,
                            .mq_msgsize = GARBAGE_MSG_MAX, .mq_curmsgs = 0 };
    mqd_t mq = mq_open(cfg.garbage_mq, O_CREAT | O_RDONLY | O_NONBLOCK, 0600, &attr);
    if (mq == (mqd_t)-1){
        perror("mq_open");
        close(listen_fd); close(ctl_fd); return 1;
    }

    // 6. Set up the log socket. From here on, only the shipper thread uses it.
    log_init(cfg.log_ipc);

    // 7. Turn the process into a background daemon, unless the config set
    //    daemonize=no, in which case we stay in the foreground.
    if (cfg.daemonize){
        if (daemonize() < 0){ close(listen_fd); close(ctl_fd); return 1; }
    }

    // 8. Write the process id file now, after daemonizing, so it holds the
    //    daemon's real process id.
    write_pidfile(cfg.pid_path);

    // 9. Set up the ring buffer and start the logshipper thread. This must
    //    happen after daemonizing, because fork() only keeps the thread that
    //    called it. A thread started before the fork would quietly not exist
    //    inside the daemon.
    ring_init(&g_ring);
    atomic_init(&dropped_send, 0);
    atomic_init(&shipper_stop, 0);
    pthread_t shipper;
    if (pthread_create(&shipper, NULL, logshipper, NULL) != 0){
        perror("pthread_create");
        close(listen_fd); close(ctl_fd); mq_close(mq); return 1;
    }

    // 10. Create the signalfd. The three signals we blocked now arrive here as
    //     bytes we can read, so the event loop can wait on them like any fd.
    int sig_fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sig_fd < 0){ perror("signalfd"); close(listen_fd); close(ctl_fd); return 1; }

    // 11. Build the epoll set, adding each fd we want to watch with one call.
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0){ perror("epoll_create1"); return 1; }
    if (ep_add(ep, listen_fd) < 0 || ep_add(ep, sig_fd) < 0 ||
        ep_add(ep, ctl_fd)    < 0 || ep_add(ep, (int)mq) < 0){
        perror("epoll_ctl"); return 1;
    }

    logmsg("tetrisd: started (epoll loop: listen + signalfd + ctl + mq)");

    // 12. The main event loop. epoll_wait sleeps until at least one fd is
    //     ready, and then we send each ready fd to its handler. The timeout of
    //     -1 means "wait forever", which is correct because everything that can
    //     happen to us arrives as an fd event. Later on, room tickers will join
    //     this same set as timer fds, rather than as separate threads.
    int running = 1;
    while (running){
        struct epoll_event events[16];
        int n = epoll_wait(ep, events, 16, -1);
        if (n < 0){
            if (errno == EINTR) continue;   // interrupted early: just re-wait
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n && running; i++){
            int fd = events[i].data.fd;
            if      (fd == listen_fd) handle_new_client(listen_fd);
            else if (fd == sig_fd)    handle_signal_event(sig_fd, rc_path, &cfg, &running);
            else if (fd == ctl_fd)    handle_ctl(ctl_fd, &running);
            else if (fd == (int)mq)   handle_garbage(mq);
        }
    }

    // 13. Shut down cleanly. We push the final log record first, and only then
    //     tell the shipper to stop. The shipper drains everything left in the
    //     ring before it exits, so the "stopped" record still reaches
    //     tetrislogd. The join below waits for that drain to finish.
    logmsg("tetrisd: stopped");
    atomic_store(&shipper_stop, 1);
    pthread_join(shipper, NULL);

    close(ep);
    close(sig_fd);
    close(listen_fd);
    close(ctl_fd);
    mq_close(mq);
    mq_unlink(cfg.garbage_mq);      // remove the queue's name from the system
    if (log_fd >= 0) close(log_fd);
    unlink(cfg.ctl_path);           // remove our control socket file
    unlink(cfg.pid_path);
    ring_destroy(&g_ring);
    return 0;
}
