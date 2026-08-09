// Attaches the ops console to a live room and streams its boards into a
// plain-text view until any input arrives on stdin, then leaves cleanly.
//
// tetrisd has no true spectate mode (see REQUIRED_FILES.md): the only way
// to receive STATE broadcasts is an ordinary JOIN, which deals a real board
// and counts toward the room's player limit. So `attach` is implemented as
// an ordinary late-join participant over the game plane (src/tetrisu/net.c,
// the same adapter tetrisu itself uses) rather than blocking on a server
// feature that does not exist -- see DECISIONS.md's attach-model entry for
// the caveat this implies.

#ifndef TETRISH_VIEW_ATTACH_H
#define TETRISH_VIEW_ATTACH_H

#include "rc.h"

// Connects to tetrisd (cfg->bind_addr:listen_port), JOINs `room` (creating
// it if it doesn't exist), starts it if nobody has yet, and redraws its
// boards each time a STATE frame arrives. select() watches stdin alongside
// the session socket; any byte on stdin detaches (LEAVEs the room) and
// returns to the caller, which is always the repl's own prompt regardless
// of this function's return value. Returns 0 on a normal detach, 1 if the
// connection/handshake/JOIN never succeeded in the first place.
int attach_run(const Config *cfg, const char *room);

#endif
