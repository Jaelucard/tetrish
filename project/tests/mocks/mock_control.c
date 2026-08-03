// test-only mock, not part of the real system

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "mock_control.h"
#include "htttp.h"

static void *accept_thread(void *arg){
    mock_control_t *m = (mock_control_t *)arg;

    struct sockaddr_un peer;
    socklen_t plen = sizeof peer;
    int cfd = accept(m->listen_fd, (struct sockaddr *)&peer, &plen);
    if (cfd < 0) return NULL;

    // If control_get never connects (a bug in the caller under test), this
    // thread must not hang the test process forever.
    struct timeval to = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);

    ssize_t n = read(cfd, m->got_request, sizeof m->got_request - 1);
    m->got_request_len = (n > 0) ? (size_t)n : 0;
    m->got_request[m->got_request_len] = '\0';

    htttp_builder_t b;
    htttp_builder_init_response(&b, (htttp_status_t)m->reply_status);
    if (m->reply_body)
        htttp_builder_set_body(&b, (const unsigned char *)m->reply_body, strlen(m->reply_body));
    size_t wlen = 0;
    char *wire = htttp_serialise(&b, &wlen);
    if (wire){
        ssize_t written = write(cfd, wire, wlen);
        (void)written;   // best-effort; a short write just fails the test's assertions
        free(wire);
    }
    close(cfd);
    return NULL;
}

int mock_control_start(mock_control_t *m, Config *cfg, int reply_status, const char *reply_body){
    memset(m, 0, sizeof *m);
    m->reply_status = reply_status;
    m->reply_body   = reply_body;
    m->listen_fd    = -1;

    snprintf(m->dir, sizeof m->dir, "/tmp/tv-mock-XXXXXX");
    if (mkdtemp(m->dir) == NULL) return -1;
    snprintf(m->sock_path, sizeof m->sock_path, "%s/ctl.sock", m->dir);

    m->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m->listen_fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m->sock_path, sizeof addr.sun_path - 1);
    if (bind(m->listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) return -1;
    if (listen(m->listen_fd, 1) < 0) return -1;

    if (pthread_create(&m->thread, NULL, accept_thread, m) != 0) return -1;

    memset(cfg, 0, sizeof *cfg);
    snprintf(cfg->ctl_path, sizeof cfg->ctl_path, "%s", m->sock_path);
    return 0;
}

void mock_control_stop(mock_control_t *m){
    pthread_join(m->thread, NULL);
    if (m->listen_fd >= 0) close(m->listen_fd);
    unlink(m->sock_path);
    rmdir(m->dir);
}
