// tetrisd: the game server daemon.
// Week 4 scope: become a daemon, bind a TCP listen socket, write a PID file,
// and react to SIGTERM/SIGHUP/SIGUSR1 through a signalfd. No accept() yet.
#include <stdio.h>           // standard input/output
#include <stdlib.h>          // standard library
#include <string.h>          // string helpers
#include <stdint.h>          // fixed width integer types
#include <unistd.h>          // POSIX system calls
#include <errno.h>           // errno / error reporting
#include <signal.h>          // signal handling
#include <sys/signalfd.h>    // signalfd
#include <sys/socket.h>      // sockets
#include <netinet/in.h>      // internet address structs
#include <arpa/inet.h>       // inet_pton and friends
#include <sys/un.h>          // unix domain socket structs
#include "rc.h"              // config parser
#include "daemon.h"          // shared daemonize() helper

// logging to tetrislogd, fire-and-forget over the DGRAM socket
static int log_fd = -1;
static struct sockaddr_un log_addr;

// set up a non-blocking datagram socket aimed at tetrislogd's log_ipc path
static void log_init(const char *path){
    log_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    memset(&log_addr, 0, sizeof log_addr);
    log_addr.sun_family = AF_UNIX;
    strncpy(log_addr.sun_path, path, sizeof log_addr.sun_path - 1);
}

// send one log line. fire-and-forget: if tetrislogd is down or its buffer is
// full the sendto just fails and we drop the record. logging must never block the game.
static void logmsg(const char *msg){
    if (log_fd < 0) return;
    sendto(log_fd, msg, strlen(msg), 0, (struct sockaddr *)&log_addr, sizeof log_addr);
}

// build the TCP listening socket: bind bind_addr:listen_port, then listen
static int tcp_listen(const Config *cfg){
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0){ perror("socket"); return -1; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);  // lets us rebind right after a restart

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)cfg->listen_port);          // host -> network byte order
    if (inet_pton(AF_INET, cfg->bind_addr, &sa.sin_addr) != 1){ // "127.0.0.1" text -> binary
        fprintf(stderr, "tetrisd: bad bind address '%s'\n", cfg->bind_addr);
        close(fd); return -1;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0){ perror("bind"); close(fd); return -1; }
    if (listen(fd, cfg->tcp_backlog) < 0){ perror("listen"); close(fd); return -1; }
    return fd;
}

// write our PID to pid_path. done AFTER daemonizing so we record the daemon's PID, not the parent's.
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

int main(int argc, char **argv){
    // 1. load config. we need listen_port, bind_addr, pid_path, log_ipc.
    const char *rc_path = (argc > 1) ? argv[1] : ".tetrishrc";
    Config cfg;
    if (rc_load(rc_path, &cfg) != 0){
        fprintf(stderr, "tetrisd: failed to load configuration from %s\n", rc_path);
        return 1;
    }

    // 2. block SIGTERM/SIGHUP/SIGUSR1 so they don't get handled the default way.
    //    once blocked they show up as readable events on the signalfd. the mask survives fork().
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0){ perror("sigprocmask"); return 1; }

    // 3. build the TCP listener BEFORE daemonizing, so errors still print to the
    //    terminal and the fd is inherited across the fork.
    int listen_fd = tcp_listen(&cfg);
    if (listen_fd < 0) return 1;

    // 4. set up the log channel to tetrislogd.
    log_init(cfg.log_ipc);

    // 5. detach into the background (skip if daemonize=no).
    if (cfg.daemonize){
        if (daemonize() < 0){ close(listen_fd); return 1; }
    }

    // 6. write the PID file now, so it holds the daemon's real PID.
    write_pidfile(cfg.pid_path);

    // 7. create the signalfd. the three blocked signals now arrive as bytes we read here.
    int sig_fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sig_fd < 0){ perror("signalfd"); close(listen_fd); return 1; }

    logmsg("tetrisd: started (listening, no accept yet)");

    // 8. temporary main loop: block until a signal arrives, then handle it.
    //    a later week swaps this for an epoll loop that also accepts clients.
    int running = 1;
    while (running){
        struct signalfd_siginfo si;
        ssize_t r = read(sig_fd, &si, sizeof si);
        if (r != sizeof si){
            if (errno == EINTR) continue;
            perror("read(signalfd)");
            break;
        }
        switch (si.ssi_signo){
        case SIGTERM:
            logmsg("tetrisd: SIGTERM -> shutting down");
            running = 0;
            break;
        case SIGHUP:
            if (rc_load(rc_path, &cfg) == 0) logmsg("tetrisd: SIGHUP -> config reloaded");
            else                             logmsg("tetrisd: SIGHUP -> reload FAILED, keeping old config");
            break;
        case SIGUSR1:
            logmsg("tetrisd: SIGUSR1 -> STATE DUMP: rooms=0 players=0 (stub)");
            break;
        }
    }

    // 9. cleanup.
    close(sig_fd);
    close(listen_fd);
    if (log_fd >= 0) close(log_fd);
    unlink(cfg.pid_path);
    return 0;
}
