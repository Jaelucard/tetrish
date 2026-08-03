// test-only mock, not part of the real system

// A minimal stand-in for tetrisd's control-plane listener: one AF_UNIX
// stream socket that accepts exactly one connection, captures whatever
// request arrives, and replies with a canned HTTTP response. This is what
// lets control_get() (src/tetrish-view/control.c) be tested end to end
// without a real tetrisd.

#ifndef MOCK_CONTROL_H
#define MOCK_CONTROL_H

#include <stddef.h>
#include <pthread.h>
#include "rc.h"

typedef struct {
    char sock_path[108];     // sizeof sockaddr_un.sun_path on Linux
    char dir[64];             // temp dir sock_path lives in; removed on stop
    int  listen_fd;
    pthread_t thread;

    // Set by the caller before mock_control_start: what the mock replies
    // with. reply_body must outlive the mock (it's not copied).
    int         reply_status;
    const char *reply_body;

    // Set by the accept thread. Only valid to read after mock_control_stop
    // returns (it joins the thread first).
    char   got_request[4096];
    size_t got_request_len;
} mock_control_t;

// Starts listening on a fresh, uniquely-named socket and spawns a thread
// that accepts exactly one connection, records the request bytes, replies
// with reply_status/reply_body, and exits. Fills cfg->ctl_path so the
// caller can hand cfg straight to control_get. Returns 0 on success.
int mock_control_start(mock_control_t *m, Config *cfg, int reply_status, const char *reply_body);

// Joins the accept thread (blocks until the one expected connection has
// happened and been handled, or its 2-second receive timeout elapses) and
// removes the socket file and its temp dir.
void mock_control_stop(mock_control_t *m);

#endif
