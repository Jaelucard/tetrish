// macOS shim for POSIX message queues (<mqueue.h>), which Darwin has never
// implemented. Darwin-only include path; see the note in sys/signalfd.h.
//
// Emulation: an AF_UNIX SOCK_DGRAM socket bound to a filesystem path derived
// from the queue name ("/tetris-garbage" -> /tmp/mq.tetris-garbage). A
// datagram socket preserves message boundaries the same way an mq does --
// one recv returns exactly what one send wrote, never split, never merged --
// which is the property garbage.h's design note says the queue was chosen
// for. Because the rendezvous is a filesystem path, a SEPARATE process
// (tests/garbage_send.c) can still open the queue by name and post into the
// daemon, just like on Linux.
//
// Semantic differences, all harmless to this tree:
//  - The queue does not survive the owner: mq_open(O_CREAT) rebinds the
//    path, so a stale message from a previous run cannot be read back.
//    (tetrisd guards against stale messages with a magic number anyway.)
//  - The depth bound is enforced through the socket's receive buffer
//    (mq_maxmsg * mq_msgsize), so a full queue fails a send with
//    ENOBUFS/EAGAIN rather than precisely at mq_maxmsg messages. tetrisd's
//    policy only needs "a full queue fails fast instead of blocking".
//  - mq_getattr/mq_notify and message priorities are not provided; the
//    priority arguments are accepted and ignored (all callers pass 0/NULL).
#ifndef COMPAT_MQUEUE_H
#define COMPAT_MQUEUE_H

#include <sys/types.h>
#include <fcntl.h>

typedef int mqd_t;

struct mq_attr {
    long mq_flags;
    long mq_maxmsg;
    long mq_msgsize;
    long mq_curmsgs;
};

mqd_t   mq_open(const char *name, int oflag, ...);   // [, mode_t mode, struct mq_attr *attr]
int     mq_close(mqd_t mqdes);
int     mq_unlink(const char *name);
int     mq_send(mqd_t mqdes, const char *msg_ptr, size_t msg_len, unsigned msg_prio);
ssize_t mq_receive(mqd_t mqdes, char *msg_ptr, size_t msg_len, unsigned *msg_prio);

#endif
