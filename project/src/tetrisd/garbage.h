// The Battle Royale garbage event, carried over the POSIX message queue.
//
// It lives in its own header so test tools can send a well-formed event without
// duplicating the struct layout. Both ends are the same process on the same
// machine, so the struct goes on the wire raw, with no serialisation: there is
// no endianness or padding question to answer.
//
// Why the payload travels inside the queue rather than the queue being a bare
// wakeup: a POSIX message queue preserves message boundaries, so one mq_receive
// returns exactly what one mq_send wrote, never split and never merged. Redis
// cannot do this with its module pipe (server.h:2032) because a pipe is a byte
// stream, so it writes a single "A" byte as a doorbell and keeps the real
// payload in a separate list. Message queues let us skip that second structure.
#ifndef GARBAGE_H
#define GARBAGE_H

#include <stdint.h>
#include "rooms.h"           // ROOM_ID_MAX

#define GARBAGE_MSG_MAX 128  // must match the mq_msgsize attribute in tetrisd
#define GARBAGE_MAGIC 0x47425231u   // "GBR1". A POSIX message queue outlives the
                                    // process that created it, so this catches a
                                    // stale message left by an older build.

typedef struct {
    uint32_t magic;
    char     src_room[ROOM_ID_MAX];   // who cleared the lines (for the log)
    char     dst_room[ROOM_ID_MAX];   // who takes the hit
    uint16_t hole_pattern;            // bitmask: set bit = that column stays empty
    uint8_t  rows;                    // how many garbage rows to insert
} garbage_msg_t;

// The attack table: how many garbage rows a clear of `lines_cleared` sends.
// This is the standard Tetris Guideline mapping. A single sends nothing, which
// is what makes clearing several rows at once worth the risk.
//
//   1 line  -> 0 rows      3 lines -> 2 rows
//   2 lines -> 1 row       4 lines -> 4 rows (a "tetris" is rewarded twice over)
//
// It lives here, next to the message it fills in, rather than inline in the
// tick handler, so a unit test can reach it. Otherwise the only way to exercise
// it is a player who actually clears two lines, and no automated client in this
// repo can do that yet.
static inline int garbage_rows_for(int lines_cleared){
    if (lines_cleared < 2) return 0;
    return (lines_cleared >= 4) ? 4 : lines_cleared - 1;
}

#endif
