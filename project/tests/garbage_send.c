// garbage_send posts one Battle Royale garbage event into tetrisd's message
// queue, so the receive side can be tested on demand.
//
// This exists because no automated client in this repo plays well enough to
// clear two lines at once, and a clear of two or more is what makes tetrisd
// send garbage of its own accord. Waiting for that to happen by luck is not a
// test. This tool opens the same POSIX message queue tetrisd is watching and
// posts a well-formed event, so the daemon's epoll wakeup, magic check, room
// lookup and injection path all run when we want them to.
//
// It also doubles as a demo prop. Run it while two clients are playing and the
// target room's boards visibly jump up.
//
// usage: garbage_send <rc-file> <src-room> <dst-room> <rows> [hole-column]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <mqueue.h>
#include "rc.h"
#include "garbage.h"

int main(int argc, char **argv){
    if (argc < 5){
        fprintf(stderr,
                "usage: %s <rc-file> <src-room> <dst-room> <rows> [hole-column]\n"
                "  rows        1-20, how many garbage rows to insert\n"
                "  hole-column 0-9, which column stays empty (default 4)\n",
                argv[0]);
        return 2;
    }

    Config cfg;
    if (rc_load(argv[1], &cfg) != 0){
        fprintf(stderr, "garbage_send: cannot load config %s\n", argv[1]);
        return 1;
    }

    int rows = atoi(argv[4]);
    int hole = (argc > 5) ? atoi(argv[5]) : 4;
    if (rows < 1 || rows > 20){ fprintf(stderr, "rows must be 1-20\n"); return 2; }
    if (hole < 0 || hole > 9){  fprintf(stderr, "hole must be 0-9\n");  return 2; }

    // O_WRONLY here: this tool only produces. tetrisd itself needs O_RDWR
    // because it is both ends of the channel.
    mqd_t mq = mq_open(cfg.garbage_mq, O_WRONLY | O_NONBLOCK);
    if (mq == (mqd_t)-1){
        fprintf(stderr, "garbage_send: mq_open(%s): %s\n",
                cfg.garbage_mq, strerror(errno));
        fprintf(stderr, "  (is tetrisd running? it creates the queue)\n");
        return 1;
    }

    garbage_msg_t gm;
    memset(&gm, 0, sizeof gm);
    gm.magic        = GARBAGE_MAGIC;
    gm.rows         = (uint8_t)rows;
    gm.hole_pattern = (uint16_t)(1u << hole);
    snprintf(gm.src_room, sizeof gm.src_room, "%s", argv[2]);
    snprintf(gm.dst_room, sizeof gm.dst_room, "%s", argv[3]);

    if (mq_send(mq, (const char *)&gm, sizeof gm, 0) < 0){
        // EAGAIN here means the queue is full, which is the same condition
        // tetrisd's own send site handles by dropping.
        fprintf(stderr, "garbage_send: mq_send: %s\n", strerror(errno));
        mq_close(mq);
        return 1;
    }
    printf("sent: %s -> %s rows=%d hole=col%d\n", gm.src_room, gm.dst_room, rows, hole);
    mq_close(mq);
    return 0;
}
