// tetrisctl is the admin command line tool. It connects to tetrisd's control
// socket (the ctl_path from the config file), sends a single HTTTP request,
// prints the response body, and then exits.
//
// As of week 6, the control plane speaks real HTTTP through libhtttp instead
// of the small placeholder line protocol we used back in weeks 4 and 5. This
// channel carries plain HTTTP text over a local AF_UNIX socket. Only
// processes on this same machine can reach that socket, so normal filesystem
// permissions are enough to control who can connect. That is why we do not
// bother adding the session encryption layer here, since it would not
// protect anything extra.
//
// Each command below turns into a single GET request to a fixed path on
// tetrisd. Here is what each one does.
//   status              -> GET /status     returns counters for rooms, players, and
//                                 dropped log records
//   rooms               -> GET /rooms      lists every active room along with how many
//                                 players are in it
//   players             -> GET /players    lists every player currently connected
//   dropped-logs        -> GET /dropped-logs  reports log records lost inside
//                                 tetrisd, split by cause. "ring" means the
//                                 game path could not hand the record over
//                                 (trylock failed, or the ring was full), so
//                                 tetrisd is busier than the shipper. "send"
//                                 means the shipper's sendto failed, so
//                                 tetrislogd is dead, slow, or backed up.
//   shutdown            -> GET /shutdown   asks the daemon to shut down gracefully
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>         // We ignore SIGPIPE here, because the server might close first
#include <sys/socket.h>
#include <sys/un.h>
#include "rc.h"
#include "htttp.h"

static const char *command_to_path(const char *cmd){
    if (strcasecmp(cmd, "status")       == 0) return "/status";
    if (strcasecmp(cmd, "rooms")        == 0) return "/rooms";
    if (strcasecmp(cmd, "players")      == 0) return "/players";
    if (strcasecmp(cmd, "dropped-logs") == 0) return "/dropped-logs";
    if (strcasecmp(cmd, "shutdown")     == 0) return "/shutdown";
    return NULL;
}

int main(int argc, char **argv){
    // The first command line argument is the path to the config file, and it
    // defaults to .tetrishrc if we are not given one. The second argument is
    // the command to run, and it defaults to status if we are not given one.
    const char *rc_path = (argc > 1) ? argv[1] : ".tetrishrc";
    const char *command = (argc > 2) ? argv[2] : "status";

    // If the daemon closes the socket while we are still writing to it, we
    // want write() to just return -1 with errno set to EPIPE. Without this
    // line, the operating system would instead send us a signal that kills
    // the program outright.
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

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0){
        fprintf(stderr, "tetrisctl: cannot connect to %s: %s\n"
                        "  (is tetrisd running?)\n", cfg.ctl_path, strerror(errno));
        close(fd); return 1;
    }

    // Step 2: build the request and send it, using libhtttp to do the work.
    // We never build the raw HTTTP text by hand, since the library already
    // knows how to format it correctly.
    htttp_builder_t b;
    htttp_builder_init_request(&b, HTTTP_METHOD_GET, path);
    size_t req_len = 0;
    char *req = htttp_serialise(&b, &req_len);
    if (req == NULL){ fprintf(stderr, "tetrisctl: out of memory\n"); close(fd); return 1; }
    if (write(fd, req, req_len) < 0){ perror("write"); free(req); close(fd); return 1; }
    free(req);

    // Step 3: read the whole response. The server closes the connection once
    // it has sent everything, so we know we are done reading when read()
    // returns zero bytes (EOF). After that we parse the response and print it.
    char buf[16384];
    size_t got = 0;
    ssize_t n;
    while ((n = read(fd, buf + got, sizeof buf - 1 - got)) > 0){
        got += (size_t)n;
        if (got >= sizeof buf - 1) break;
    }
    close(fd);
    if (n < 0){ perror("read"); return 1; }

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
        return 1;                     // A status code outside the 2xx range means the command
                                       // failed, so we return a non-zero exit code here. That way
                                       // scripts calling tetrisctl can check whether it succeeded.
    }
    return 0;
}
