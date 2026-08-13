// tetrisctl is the admin command line tool. Connect to tetrisd's control
// socket (ctl_path from the config file), send one HTTTP request, print the
// response body, exit.
//
// The control plane speaks real HTTTP through libhtttp now, in place of the
// placeholder line protocol it started with. Plain text over a local AF_UNIX
// socket: only processes on this machine can reach it, so filesystem
// permissions already decide who gets to connect. Wrapping it in the session
// encryption layer would protect nothing extra, so it does not.
//
// Every command is one GET to a fixed path on tetrisd:
//   status              -> GET /status     counters for rooms, players, and
//                                 dropped log records
//   rooms               -> GET /rooms      every active room and how many
//                                 players are in it
//   players             -> GET /players    every player currently connected
//   dropped-logs        -> GET /dropped-logs  log records lost inside tetrisd,
//                                 split by cause. "ring" means the game path
//                                 could not hand the record over (trylock
//                                 failed, or the ring was full), so tetrisd is
//                                 busier than the shipper. "send" means the
//                                 shipper's sendto failed, so tetrislogd is
//                                 dead, slow, or backed up.
//   shutdown            -> GET /shutdown   ask the daemon to stop cleanly
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>         // We ignore SIGPIPE here, because the server might close first
#include <sys/socket.h>
#include <sys/time.h>       // struct timeval, for the socket timeouts below
#include <sys/un.h>
#include "rc.h"
#include "htttp.h"

// How long to wait on the daemon before giving up.
//
// An admin tool must not be able to hang forever, and without this it can.
// tetrisd puts a 2 second timeout on its side of an accepted control
// connection, but that governs the daemon's reads, not ours. If the event loop
// is stuck (blocked mid-broadcast to a client that stopped reading, say) it
// never reaches accept() at all, and yet connect() still SUCCEEDS, because the
// kernel completes it into the listen backlog. So we sit in read() with nothing
// coming back and no timeout, and `tetrisctl shutdown` hangs quietly at the one
// moment an operator really needs it to work.
#define CTL_TIMEOUT_SEC 5

static const char *command_to_path(const char *cmd){
    if (strcasecmp(cmd, "status")       == 0) return "/status";
    if (strcasecmp(cmd, "rooms")        == 0) return "/rooms";
    if (strcasecmp(cmd, "players")      == 0) return "/players";
    if (strcasecmp(cmd, "dropped-logs") == 0) return "/dropped-logs";
    if (strcasecmp(cmd, "shutdown")     == 0) return "/shutdown";
    return NULL;
}

int main(int argc, char **argv){
    // argv[1] is the config file, .tetrishrc if not given.
    // argv[2] is the command, status if not given.
    const char *rc_path = (argc > 1) ? argv[1] : ".tetrishrc";
    const char *command = (argc > 2) ? argv[2] : "status";

    // If the daemon closes the socket while we are still writing, we want
    // write() to return -1 with errno EPIPE. Without this line the kernel
    // sends a signal that kills us outright instead.
    signal(SIGPIPE, SIG_IGN);

    const char *path = command_to_path(command);
    if (path == NULL){
        fprintf(stderr, "tetrisctl: unknown command '%s'\n"
                        "  commands: status | rooms | players | dropped-logs | shutdown\n",
                command);
        return 2;
    }

    Config cfg;
    if (rc_load(rc_path, &cfg) != 0){
        fprintf(stderr, "tetrisctl: failed to load configuration from %s\n", rc_path);
        return 1;
    }

    // Step 1: connect to tetrisd's control socket.
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0){ perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(cfg.ctl_path) >= sizeof addr.sun_path){
        fprintf(stderr, "tetrisctl: ctl_path too long: %s\n", cfg.ctl_path);
        close(fd); return 1;
    }
    strncpy(addr.sun_path, cfg.ctl_path, sizeof addr.sun_path - 1);

    // Both directions, before connect. The read side is the one that matters
    // (see CTL_TIMEOUT_SEC); the write side is there so a daemon that stopped
    // draining its receive buffer cannot wedge us either.
    struct timeval tv = { .tv_sec = CTL_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0){
        fprintf(stderr, "tetrisctl: cannot connect to %s: %s\n"
                        "  (is tetrisd running?)\n", cfg.ctl_path, strerror(errno));
        close(fd); return 1;
    }

    // Step 2: build the request and send it. libhtttp formats the wire text;
    // we never hand-roll it here.
    htttp_builder_t b;
    htttp_builder_init_request(&b, HTTTP_METHOD_GET, path);
    size_t req_len = 0;
    char *req = htttp_serialise(&b, &req_len);
    if (req == NULL){ fprintf(stderr, "tetrisctl: out of memory\n"); close(fd); return 1; }
    if (write(fd, req, req_len) < 0){ perror("write"); free(req); close(fd); return 1; }
    free(req);

    // Step 3: read the whole response. The server closes once it has sent
    // everything, so read() returning 0 (EOF) is how we know we are done.
    // Then parse and print.
    char buf[16384];
    size_t got = 0;
    ssize_t n;
    while ((n = read(fd, buf + got, sizeof buf - 1 - got)) > 0){
        got += (size_t)n;
        if (got >= sizeof buf - 1) break;
    }
    close(fd);
    if (n < 0){
        // A timeout gets its own message. The daemon is there (connect
        // succeeded) but is not answering, which is a different problem from
        // a socket nobody was ever listening on.
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            fprintf(stderr, "tetrisctl: no response from tetrisd within %d seconds\n"
                            "  (it accepted the connection but is not servicing it;"
                            " the event loop may be blocked)\n", CTL_TIMEOUT_SEC);
        else
            perror("read");
        return 1;
    }

    htttp_msg_t msg;
    htttp_err_t err = htttp_parse_response(buf, got, &msg);
    if (err != HTTTP_OK){
        fprintf(stderr, "tetrisctl: bad response (%s)\n", htttp_strerror(err));
        return 1;
    }

    if (msg.body != NULL && msg.body_len > 0){
        fwrite(msg.body, 1, msg.body_len, stdout);
        fputc('\n', stdout);
    }
    if (msg.status != HTTTP_200_OK){
        fprintf(stderr, "tetrisctl: %d %s\n", (int)msg.status, msg.reason);
        return 1;                     // Anything outside 2xx means the command failed, so exit
                                       // non-zero and a calling script can just test $?.
    }
    return 0;
}
