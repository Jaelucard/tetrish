// This file is the logging service (daemon) that tetrisd sends its log
// records to. It listens on an AF_UNIX SOCK_DGRAM socket, and for every log
// record it receives, it adds a timestamp and appends it to the log file.
// It runs as a detached daemon, which means it disconnects from the
// terminal that started it and keeps running quietly in the background.
#include <stdio.h>          // This gives us FILE, printf, and fprintf, for printing and for writing to files.
#include <sys/socket.h>     // This gives us socket(), bind(), and recvfrom(). We do not need listen() or accept() because SOCK_DGRAM has no connections.
#include <sys/un.h>         // This gives us the sockaddr_un structure, which we use to describe our Unix domain socket address.
#include <sys/stat.h>       // This gives us umask(), which controls the default permissions on files we create.
#include <unistd.h>         // This gives us fork(), setsid(), close(), dup2(), and unlink().
#include <stdlib.h>         // This gives us exit().
#include <string.h>         // This gives us memset, strncpy, strlen, and strerror.
#include <errno.h>          // This gives us errno, which we check after a system call fails to see what went wrong.
#include <signal.h>         // This gives us sigaction and sig_atomic_t, which we use to handle signals safely.
#include <time.h>           // This gives us time, localtime_r, and strftime, which we use to build the timestamp we put on each log line.
#include "rc.h"
#include "daemon.h"         // This gives us daemonize(), the shared double-fork helper that turns us into a background daemon.

// These two flags are how our signal handlers talk to the rest of the
// program. A signal handler has to be very small and can only safely call a
// small set of functions, a property called being async-signal-safe. Things
// like fopen() and fclose() are not safe to call from inside a handler. So
// instead of doing real work in the handler, we just set a flag here, and
// the main loop checks the flag and does the real work itself.
static volatile sig_atomic_t running = 1;   // When SIGTERM arrives, the handler sets this to 0, and the main loop stops looping.
static volatile sig_atomic_t reopen  = 0;   // When SIGHUP arrives, the handler sets this to 1, and the main loop reopens the log file.

static void on_signal(int sig){
    if (sig == SIGTERM) running = 0;        // This starts a graceful shutdown.
    else if (sig == SIGHUP) reopen = 1;     // This asks the main loop to rotate the log file.
}

// This function installs a signal handler using sigaction(), and it
// deliberately leaves out the SA_RESTART flag. This makes the program more
// robust than the older signal() function would. Without SA_RESTART, if a
// signal arrives while recvfrom() is blocked waiting for data, recvfrom()
// stops waiting and returns immediately with errno set to EINTR. That lets
// our main loop notice that SIGTERM or SIGHUP has arrived right away,
// instead of only finding out after the next datagram happens to arrive.
static void install(int sig, void (*handler)(int)){
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                        // Setting this to 0 means SA_RESTART is turned off.
    sigaction(sig, &sa, NULL);
}

// This function opens the log file in append mode, so new log lines get
// added to the end of the file instead of overwriting what is already
// there. If opening the file fails, it prints an error message and returns
// NULL.
static FILE *open_log(const char *path){
    FILE *f = fopen(path, "a");
    if (f == NULL)
        fprintf(stderr, "tetrislogd: cannot open log '%s': %s\n", path, strerror(errno));
    return f;
}

int main(int argc, char **argv) {
    // Step 1: load the configuration file. We need log_path, which tells us
    // where to write the log file, and log_ipc, which tells us the path of
    // the socket we will listen on.
    const char *rc_path = (argc > 1) ? argv[1] : ".tetrishrc";
    Config config;
    if (rc_load(rc_path, &config) != 0){
        fprintf(stderr, "Failed to load configuration from %s\n", rc_path);
        return 1;
    }

    // Step 2: install our signal handlers. SIGTERM tells us to stop, and
    // SIGHUP tells us to reopen the log file.
    install(SIGTERM, on_signal);
    install(SIGHUP,  on_signal);

    // Step 3: open the log file and the socket before we daemonize. We do
    // this first so that if something goes wrong, we can still print an
    // error message to the terminal where the user will see it. Both the
    // file and the socket keep working after fork(), because a child
    // process inherits its parent's open file descriptors.
    FILE *log = open_log(config.log_path);
    if (log == NULL) return 1;

    int sfd = socket(AF_UNIX, SOCK_DGRAM, 0);   // We use SOCK_DGRAM because this socket has no connections, unlike a stream socket.
    if (sfd < 0){ perror("socket"); fclose(log); return 1; }

    // A note on why this socket drops records under load, and why we do not
    // try to stop it here.
    //
    // An AF_UNIX datagram socket's receive queue is bounded by the NUMBER OF
    // MESSAGES it holds, not by their total size. The bound comes from
    // net.unix.max_dgram_qlen, which is 10 on this system.
    //
    // Measured rather than assumed. Bind a socket, never read from it, and
    // send until sendto() refuses:
    //
    //     payload    1 B, SO_RCVBUF  229376 -> 11 datagrams, then EAGAIN
    //     payload 4096 B, SO_RCVBUF  229376 -> 11 datagrams, then EAGAIN
    //     payload    1 B, SO_RCVBUF 2097152 -> 11 datagrams, then EAGAIN
    //     payload 4096 B, SO_RCVBUF 2097152 -> 11 datagrams, then EAGAIN
    //
    // A one-byte and a four-kilobyte payload stop at the same count, so the
    // limit is messages. Eleven get through against a limit of ten, so the
    // kernel's test is "queue length greater than the limit", not "greater or
    // equal". And raising SO_RCVBUF nine-fold changes nothing, so SO_RCVBUF
    // (which caps bytes) does not govern this at all. Raising it here was
    // tried and reverted for exactly that reason.
    //
    // We retire records with one fprintf plus one fflush each, which is a
    // write() syscall per line. That is deliberate, so a crash cannot lose
    // lines still sitting in a stdio buffer. It does mean that when tetrisd
    // bursts more than ten records faster than we can write them, the eleventh
    // is refused and tetrisd's non-blocking sendto fails.
    //
    // That is the designed behaviour, not a fault. tetrisd counts every such
    // loss in its "send" drop counter, visible through `tetrisctl
    // dropped-logs`, and gameplay never waits on logging. Measured: a single
    // client produces zero drops; ten rooms all disconnecting at once produces
    // a handful. The alternative, making the game block until the logger
    // catches up, would trade a few lost log lines for a stalled tick.

    // Here we build the socket address from the log_ipc path in the config.
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(config.log_ipc) >= sizeof addr.sun_path){   // sun_path is a small, fixed-size buffer, so we must check that the path actually fits before we copy it in.
        fprintf(stderr, "tetrislogd: log_ipc path too long: %s\n", config.log_ipc);
        fclose(log); close(sfd); return 1;
    }
    strncpy(addr.sun_path, config.log_ipc, sizeof addr.sun_path - 1);

    unlink(config.log_ipc);   // If a socket file from an earlier run is still sitting here, we remove it first.
    if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) < 0){   // We do not call listen() or accept() here, because a SOCK_DGRAM socket has no connections to accept.
        perror("bind"); fclose(log); close(sfd); return 1;
    }

    // Step 4: detach into the background and become a daemon, unless
    // daemonize is set to no in the config file. Keeping it attached to the
    // terminal like that is handy while we are debugging.
    if (config.daemonize){
        if (daemonize() < 0){ fclose(log); close(sfd); unlink(config.log_ipc); return 1; }
    }

    // Step 5: this is the main loop. For every datagram we receive, we add
    // a timestamp, append the record to the log file, and flush it right
    // away.
    char buf[4096];
    while (running){
        if (reopen){                        // SIGHUP has arrived, so it is time to rotate the log file.
            reopen = 0;
            // When SIGHUP asks us to rotate the log, we open the new file
            // before we close the old one. That way, if opening the new
            // file fails, we keep using the old file instead of ending up
            // with no file at all, which is what used to crash the program.
            // The earlier version of this code closed the old file first
            // and only then tried to open the new one. If that open failed,
            // log was left set to NULL, and later on the cleanup code would
            // call fclose(NULL), which is undefined behavior and was
            // actually crashing the daemon during testing. Opening the new
            // file first avoids that problem completely: if the reopen
            // fails, we simply keep logging to the file we already have
            // open.
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
            if (errno == EINTR) continue;   // A signal interrupted us while we were waiting, so we go back to the top of the loop to check the running and reopen flags.
            perror("recvfrom");
            break;
        }
        buf[n] = '\0';                      // A datagram is just raw bytes, not a C string, so we add the null terminator ourselves before treating it as text.

        // Here we build a timestamp in the form "YYYY-MM-DD HH:MM:SS", so every log line shows exactly when it was written.
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char stamp[64];
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);

        fprintf(log, "%s %s\n", stamp, buf);
        fflush(log);                        // We flush right away, so that if the program crashes, we do not lose log lines that were still sitting in a buffer.
    }

    // Step 6: clean up before we exit. We close the log file, close the
    // socket, and then remove the socket file from disk.
    // We still check that log is not NULL before calling fclose() on it,
    // even though the open-before-close rotation logic above means log
    // should never actually be NULL by the time we get here. We keep the
    // check anyway as a safety net, because cleanup code should never
    // assume that something earlier could not have gone wrong.
    if (log) fclose(log);
    close(sfd);
    unlink(config.log_ipc);
    return 0;
}
