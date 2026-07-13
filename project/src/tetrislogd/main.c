// This is the logging service for tetrisd.
// It receives log records from tetrisd over an AF_UNIX SOCK_DGRAM socket and
// writes them, timestamped, to the log file. It is a detached daemon.
#include <stdio.h>          // for printf, fprintf, FILE operations
#include <sys/socket.h>     // for socket(), bind(), recvfrom() (NO listen/accept: DGRAM)
#include <sys/un.h>         // for Unix domain socket structures (sockaddr_un)
#include <sys/stat.h>       // for umask()
#include <unistd.h>         // for fork(), setsid(), close(), dup2(), unlink()
#include <stdlib.h>         // for exit()
#include <string.h>         // for memset, strncpy, strlen, strerror
#include <errno.h>          // for error checking
#include <signal.h>         // for sigaction, sig_atomic_t
#include <time.h>           // for time, localtime_r, strftime (the timestamp)
#include "rc.h"
#include "daemon.h"         // shared daemonize() (double-fork)

// --- signal flags ----
// Handlers must be tiny and async-signal-safe
// The main loop reads them and does the real work (fopen/fclose etc).
static volatile sig_atomic_t running = 1;   // SIGTERM clears this -> loop ends
static volatile sig_atomic_t reopen  = 0;   // SIGHUP sets this -> reopen the log

static void on_signal(int sig){
    if (sig == SIGTERM) running = 0;        // graceful shutdown
    else if (sig == SIGHUP) reopen = 1;     // log rotation: reopen the file
}

// Install a handler with sigaction and NO SA_RESTART. This is the robustness
// upgrade over signal(): without SA_RESTART, a signal makes the blocked
// recvfrom() return with errno==EINTR, so the loop notices SIGTERM/SIGHUP
// immediately instead of only after the next datagram arrives.
static void install(int sig, void (*handler)(int)){
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                        // 0 == no SA_RESTART
    sigaction(sig, &sa, NULL);
}

// Open the log file in append mode. Returns NULL (and reports) on failure.
static FILE *open_log(const char *path){
    FILE *f = fopen(path, "a");
    if (f == NULL)
        fprintf(stderr, "tetrislogd: cannot open log '%s': %s\n", path, strerror(errno));
    return f;
}

int main(int argc, char **argv) {
    // 1. Load the config (need log_path = the file, log_ipc = the socket path).
    const char *rc_path = (argc > 1) ? argv[1] : ".tetrishrc";
    Config config;
    if (rc_load(rc_path, &config) != 0){
        fprintf(stderr, "Failed to load configuration from %s\n", rc_path);
        return 1;
    }

    // 2. Install signal handlers (SIGTERM = stop, SIGHUP = reopen log).
    install(SIGTERM, on_signal);
    install(SIGHUP,  on_signal);

    // 3. Open the log file and the socket BEFORE daemonizing, so any error is
    //    still visible on the terminal. Both survive fork() (fds are inherited).
    FILE *log = open_log(config.log_path);
    if (log == NULL) return 1;

    int sfd = socket(AF_UNIX, SOCK_DGRAM, 0);   // DGRAM = connectionless
    if (sfd < 0){ perror("socket"); fclose(log); return 1; }

    // Build the socket address from log_ipc.
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(config.log_ipc) >= sizeof addr.sun_path){   // sun_path is a small fixed buffer
        fprintf(stderr, "tetrislogd: log_ipc path too long: %s\n", config.log_ipc);
        fclose(log); close(sfd); return 1;
    }
    strncpy(addr.sun_path, config.log_ipc, sizeof addr.sun_path - 1);

    unlink(config.log_ipc);   // remove a stale socket file from a previous run
    if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) < 0){   // NO listen/accept for DGRAM
        perror("bind"); fclose(log); close(sfd); return 1;
    }

    // 4. Detach into the background (unless daemonize=no, handy for debugging).
    if (config.daemonize){
        if (daemonize() < 0){ fclose(log); close(sfd); unlink(config.log_ipc); return 1; }
    }

    // 5. Receive loop: get a datagram, timestamp it, append it, flush.
    char buf[4096];
    while (running){
        if (reopen){                        // SIGHUP arrived: rotate the log
            reopen = 0;
            fclose(log);
            log = open_log(config.log_path);
            if (log == NULL) break;
        }

        ssize_t n = recvfrom(sfd, buf, sizeof buf - 1, 0, NULL, NULL);
        if (n < 0){
            if (errno == EINTR) continue;   // a signal woke us -> re-check flags
            perror("recvfrom");
            break;
        }
        buf[n] = '\0';                      // datagrams are raw bytes, not C strings

        // Format a timestamp: "YYYY-MM-DD HH:MM:SS".
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char stamp[64];
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);

        fprintf(log, "%s %s\n", stamp, buf);
        fflush(log);                        // flush now so a crash won't lose logs
    }

    // 6. Cleanup: flush+close the file, close+remove the socket.
    fclose(log);
    close(sfd);
    unlink(config.log_ipc);
    return 0;
}
