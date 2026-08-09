// tetrish-view's link to tetrisd's control plane: plain HTTTP GET over a
// local AF_UNIX socket (see DECISIONS.md's repl design entry). This mirrors
// tetrisctl's own connect/request/read pattern (src/tetrisctl/main.c) rather
// than inventing a second way to talk to the same protocol.

#ifndef TETRISH_VIEW_CONTROL_H
#define TETRISH_VIEW_CONTROL_H

#include <stddef.h>
#include "rc.h"

// Connects to cfg->ctl_path, sends one GET request for `path`, and reads the
// response to EOF (the server closes after sending, same as it does for
// tetrisctl). The body is copied into `body` NUL-terminated, truncated to
// fit `cap` if needed. Returns the parsed HTTTP status code on success, or
// -1 if the socket couldn't be reached or the response didn't parse (body
// is left as an empty string in that case).
int control_get(const Config *cfg, const char *path, char *body, size_t cap);

#endif
