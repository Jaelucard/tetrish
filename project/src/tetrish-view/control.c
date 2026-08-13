#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "control.h"
#include "htttp.h"

// one complete control-plane exchange: connect to tetrisd's local socket,
// send a single GET, read the whole reply, hand the body and status back.
// each call is its own connection because that is the server's model too
// (handle_ctl accepts, answers once, closes). full contract in control.h.
int control_get(const Config *cfg, const char *path, char *body, size_t cap){
    // start every call with a defined (empty) body, so a failure can never
    // leave the caller printing stale bytes from a previous command
    if (cap > 0) body[0] = '\0';

    // connect to the AF_UNIX control socket named in the rc file. this is
    // a plain local socket -- authorization is its filesystem permissions
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(cfg->ctl_path) >= sizeof addr.sun_path){ close(fd); return -1; }
    strncpy(addr.sun_path, cfg->ctl_path, sizeof addr.sun_path - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0){ close(fd); return -1; }

    // build and send the GET request through libhtttp's builder, the same
    // wire format tetrisctl uses (repl_run ignores SIGPIPE, so a server
    // that closed early makes this write fail with EPIPE instead of
    // killing the process)
    htttp_builder_t b;
    htttp_builder_init_request(&b, HTTTP_METHOD_GET, path);
    size_t req_len = 0;
    char *req = htttp_serialise(&b, &req_len);
    if (req == NULL){ close(fd); return -1; }
    ssize_t wrote = write(fd, req, req_len);
    free(req);
    if (wrote < 0){ close(fd); return -1; }

    // read until the server closes (it sends one response then hangs up),
    // bounded by the local buffer
    char buf[16384];
    size_t got = 0;
    ssize_t n;
    while ((n = read(fd, buf + got, sizeof buf - 1 - got)) > 0){
        got += (size_t)n;
        if (got >= sizeof buf - 1) break;
    }
    close(fd);
    if (n < 0) return -1;

    // parse the response and copy its body out for the caller, truncated
    // to fit their buffer, always NUL-terminated
    htttp_msg_t msg;
    if (htttp_parse_response(buf, got, &msg) != HTTTP_OK) return -1;

    if (msg.body != NULL && msg.body_len > 0 && cap > 0){
        size_t cp = msg.body_len < cap - 1 ? msg.body_len : cap - 1;
        memcpy(body, msg.body, cp);
        body[cp] = '\0';
    }
    return (int)msg.status;
}
