// The logging daemon tetrisd ships its records to. Listens on an AF_UNIX
// SOCK_DGRAM socket, timestamps every record it receives, appends it to the log
// file. Runs detached, so it lets go of the terminal that started it and stays
// up in the background.
#include <stdio.h>          // FILE, printf, fprintf
#include <sys/socket.h>     // socket, bind, recvfrom. No listen or accept: SOCK_DGRAM has no connections
#include <sys/time.h>       // struct timeval, for the receive timeout below
#include <sys/un.h>         // sockaddr_un, the Unix domain socket address
#include <sys/stat.h>       // umask
#include <unistd.h>         // fork, setsid, close, dup2, unlink
#include <stdlib.h>         // exit
#include <string.h>         // memset, strncpy, strlen, strerror
#include <errno.h>          // errno, checked after a failed syscall
#include <signal.h>         // sigaction, sig_atomic_t
#include <time.h>           // time, localtime_r, strftime, for the line timestamps
#include "rc.h"
#include "daemon.h"         // daemonize(), the shared double-fork helper

// How the signal handlers talk to the rest of the program. A handler may only
// call async-signal-safe functions, and fopen/fclose are not on that list. So
// the handler sets a flag and does nothing else; the main loop reads the flag
// and does the actual work.
static volatile sig_atomic_t running = 1;   // SIGTERM sets this to 0 and the main loop stops
static volatile sig_atomic_t reopen  = 0;   // SIGHUP sets this to 1 and the main loop reopens the log

static void on_signal(int sig){
    if (sig == SIGTERM) running = 0;        // start shutting down
    else if (sig == SIGHUP) reopen = 1;     // rotate the log file
}

// sigaction, deliberately without SA_RESTART (which is what the older signal()
// would have given us). Without it, a signal arriving while recvfrom() is
// blocked makes recvfrom() return straight away with errno EINTR, so the main
// loop sees SIGTERM or SIGHUP now instead of whenever the next datagram
// happens to turn up.
static void install(int sig, void (*handler)(int)){
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                        // 0 means no SA_RESTART
    sigaction(sig, &sa, NULL);
}

// Append mode, so new lines go on the end instead of over what is already
// there. Prints why and returns NULL if the open fails.
static FILE *open_log(const char *path){
    FILE *f = fopen(path, "a");
    if (f == NULL)
        fprintf(stderr, "tetrislogd: cannot open log '%s': %s\n", path, strerror(errno));
    return f;
}

int main(int argc, char **argv) {
    // Step 1: config. We need log_path (where the log goes) and log_ipc (the
    // socket path we listen on).
    const char *rc_path = (argc > 1) ? argv[1] : ".tetrishrc";
    Config config;
    if (rc_load(rc_path, &config) != 0){
        fprintf(stderr, "Failed to load configuration from %s\n", rc_path);
        return 1;
    }

    // Step 2: handlers. SIGTERM stops us, SIGHUP reopens the log file.
    install(SIGTERM, on_signal);
    install(SIGHUP,  on_signal);

    // Step 3: open the log file and the socket BEFORE daemonizing, so that
    // anything going wrong still prints to the terminal where somebody will
    // see it. Both survive fork(); a child inherits its parent's open fds.
    FILE *log = open_log(config.log_path);
    if (log == NULL) return 1;

    int sfd = socket(AF_UNIX, SOCK_DGRAM, 0);   // datagram: no connections, unlike a stream socket
    if (sfd < 0){ perror("socket"); fclose(log); return 1; }

    // Why this socket drops records under load, and why nothing here tries to
    // stop it.
    //
    // An AF_UNIX datagram socket's receive queue is bounded by the NUMBER OF
    // MESSAGES it holds, not their total size. The bound is
    // net.unix.max_dgram_qlen, 10 on this system.
    //
    // Measured, not assumed. Bind a socket, never read from it, send until
    // sendto() refuses:
    //
    //     payload    1 B, SO_RCVBUF  229376 -> 11 datagrams, then EAGAIN
    //     payload 4096 B, SO_RCVBUF  229376 -> 11 datagrams, then EAGAIN
    //     payload    1 B, SO_RCVBUF 2097152 -> 11 datagrams, then EAGAIN
    //     payload 4096 B, SO_RCVBUF 2097152 -> 11 datagrams, then EAGAIN
    //
    // One byte and four kilobytes stop at the same count, so the limit counts
    // messages. Eleven get through against a limit of ten, so the kernel's test
    // is "queue length greater than the limit", not greater or equal. And a
    // nine-fold SO_RCVBUF does nothing, so SO_RCVBUF (which caps bytes) is not
    // what governs this. Raising it here was tried, then reverted.
    //
    // Records retire with one fprintf plus one fflush each, so a write()
    // syscall per line. On purpose: a crash cannot then lose lines still
    // sitting in a stdio buffer. The cost is that when tetrisd bursts more than
    // ten records faster than we write them, the eleventh is refused and
    // tetrisd's non-blocking sendto fails.
    //
    // Which is the design, not a fault. tetrisd counts every such loss in its
    // "send" drop counter, readable through `tetrisctl dropped-logs`, and the
    // game never waits on logging. Measured: one client drops nothing, ten
    // rooms disconnecting at once drops a handful. Making the game block until
    // the logger caught up would trade a few log lines for a stalled tick.

    // Socket address, built from log_ipc in the config.
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(config.log_ipc) >= sizeof addr.sun_path){   // sun_path is small and fixed, so check the path fits before copying
        fprintf(stderr, "tetrislogd: log_ipc path too long: %s\n", config.log_ipc);
        fclose(log); close(sfd); return 1;
    }
    strncpy(addr.sun_path, config.log_ipc, sizeof addr.sun_path - 1);

    // Before removing whatever is at that path, check it is not a socket
    // another tetrislogd is still using.
    //
    // The unlink() below is what makes this necessary. Start this twice and the
    // second instance deletes the first one's socket file and binds its own in
    // place of it. The first daemon carries on quite happily on a socket that
    // no longer has a name, so it never receives anything again, while
    // tetrisd's records quietly start going to the second one. Then whichever
    // exits first unlinks at the end of main and deletes the OTHER's socket
    // node. Nothing in that whole sequence reports an error.
    //
    // The AF_UNIX way to ask "is anyone home?" is to try connecting: a live
    // socket accepts, a file left behind by a crashed run refuses with
    // ECONNREFUSED, an empty path gives ENOENT.
    int probe = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (probe >= 0){
        int live = (connect(probe, (struct sockaddr *)&addr, sizeof addr) == 0);
        close(probe);
        if (live){
            fprintf(stderr, "tetrislogd: another tetrislogd is already bound to %s\n",
                    config.log_ipc);
            fclose(log); close(sfd); return 1;
        }
    }

    unlink(config.log_ipc);   // only a stale node can be here now, so this is safe
    if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) < 0){   // no listen() or accept(): SOCK_DGRAM has no connections to accept
        perror("bind"); fclose(log); close(sfd); return 1;
    }

    // Cap the wait in recvfrom so the loop keeps re-testing its two flags.
    //
    // Without it the loop is "test the flags, then block", and a signal landing
    // in the gap between those two steps sets its flag and interrupts nothing:
    // recvfrom parks anyway and only wakes on the next datagram. At shutdown
    // there is no next datagram, since tetrisd is normally stopped first, so a
    // SIGTERM lost that way leaves this daemon up with the log file open and
    // nothing short of SIGKILL to end it. Same window quietly defers a SIGHUP
    // rotation.
    //
    // The EINTR handling below does not cover this. It only helps when the
    // signal arrives while recvfrom is ALREADY blocked. A timeout bounds the
    // window instead. The proper fix is signalfd, which is what tetrisd uses
    // and why tetrisd does not have this problem, but that means restructuring
    // the loop; a receive timeout buys the same guarantee for one setsockopt.
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    // Step 4: detach into the background, unless daemonize is no in the config.
    // Staying attached to the terminal is handy while debugging.
    if (config.daemonize){
        if (daemonize() < 0){ fclose(log); close(sfd); unlink(config.log_ipc); return 1; }
    }

    // Step 5: the main loop. Every datagram gets a timestamp, gets appended to
    // the log file, and gets flushed straight away.
    char buf[4096];
    while (running){
        if (reopen){                        // SIGHUP arrived: rotate the log
            reopen = 0;
            // Open the new file BEFORE closing the old one. The first version
            // did it the other way round, and when the open failed, log was
            // left NULL and the cleanup path below called fclose(NULL), which
            // is undefined behaviour and was crashing the daemon during
            // testing. Open-first means a failed rotation just keeps logging
            // to the file already open.
            FILE *nl = open_log(config.log_path);
            if (nl != NULL){
                fclose(log);
                log = nl;
            } else {
                fprintf(log, "tetrislogd: log rotation failed, keeping old file\n");
                fflush(log);
            }
        }

        ssize_t n = recvfrom(sfd, buf, sizeof buf - 1, 0, NULL, NULL);
        if (n < 0){
            if (errno == EINTR) continue;   // signal while waiting: go back up and re-test running and reopen
            // Receive timeout expired, nothing arrived. Not an error. Going
            // back to the top is the whole point: that is where the running
            // and reopen flags get re-tested.
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            perror("recvfrom");
            break;
        }
        buf[n] = '\0';                      // a datagram is raw bytes, not a C string: terminate it before treating it as text

        // Timestamp as "YYYY-MM-DD HH:MM:SS", so every line says when it landed.
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char stamp[64];
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);

        fprintf(log, "%s %s\n", stamp, buf);
        fflush(log);                        // flush now: a crash must not take buffered lines with it
    }

    // Step 6: clean up. Close the log, close the socket, remove the socket
    // node from disk.
    // The NULL check on log stays even though the open-before-close rotation
    // above means it should never be NULL here. Cleanup code is the wrong
    // place to assume nothing earlier went wrong.
    if (log) fclose(log);
    close(sfd);
    unlink(config.log_ipc);
    return 0;
}
