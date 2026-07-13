// A stress test for the ring buffer and the logshipper working together

// How to run it:
//   1. Start tetrislogd first, because it is the one that binds the log_ipc socket.
//   2. Run ./stress_ring <log_ipc path> [n_records]. If you leave out the
//      record count, it defaults to 100000.
//   3. Compare the "delivered" number it prints against the count you get from
//      running: grep -c STRESS <log_path>. They should match.

// This copies tetrisd's real logging pipeline as closely as possible. One
// thread (the producer) calls ring_push() as fast as it can, which drops
// records whenever the lock is busy or the ring is full. A second thread (the
// shipper) drains the ring and sends each record to tetrislogd. Every record is
// numbered, and at the end we print numbers that should all add up:
//
//   produced should equal pushed plus ring drops. This proves that no record
//   went missing without being counted somewhere.
//   delivered should equal pushed minus send drops. It should also equal what
//   tetrislogd actually received. The reason it lines up is that an AF_UNIX
//   datagram socket never quietly loses a message. If its buffer is full the
//   send fails with EAGAIN, and we count that as a send drop.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "ring.h"

#define BATCH 64

static Ring ring;
static int sock_fd = -1;
static struct sockaddr_un dst;
static atomic_ulong send_drops;     // how many times sendto() failed, usually because the receive buffer was full (EAGAIN)
static atomic_int   producer_done;  // set to 1 when the producer is finished, which tells the shipper to drain the rest and stop

// This shipper has the same shape as the real one in tetrisd. It copies a batch
// of records out while holding the lock (that happens inside ring_pop_batch),
// and then it sends them over the socket after the lock has been released.
static void *shipper(void *arg){
    (void)arg;
    static char batch[BATCH][RING_REC_MAX];
    size_t lens[BATCH];
    for (;;){
        size_t n = ring_pop_batch(&ring, batch, lens, BATCH);
        if (n == 0){
            if (atomic_load(&producer_done))
                break;              // the producer is done and the ring is now empty, so we can stop
            usleep(1000);
            continue;
        }
        for (size_t i = 0; i < n; i++){
            if (sendto(sock_fd, batch[i], lens[i], 0,
                       (struct sockaddr *)&dst, sizeof dst) < 0)
                atomic_fetch_add(&send_drops, 1);
        }
    }
    return NULL;
}

int main(int argc, char **argv){
    if (argc < 2){
        fprintf(stderr, "usage: %s <log_ipc path> [n_records]\n", argv[0]);
        return 2;
    }
    long n_records = (argc > 2) ? strtol(argv[2], NULL, 10) : 100000;

    // Set the socket up the same way tetrisd's log_init() does: a datagram
    // socket in non-blocking mode.
    sock_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock_fd < 0){ perror("socket"); return 1; }
    memset(&dst, 0, sizeof dst);
    dst.sun_family = AF_UNIX;
    strncpy(dst.sun_path, argv[1], sizeof dst.sun_path - 1);

    if (ring_init(&ring) != 0){ fprintf(stderr, "ring_init failed\n"); return 1; }
    atomic_init(&send_drops, 0);
    atomic_init(&producer_done, 0);

    pthread_t sh;
    if (pthread_create(&sh, NULL, shipper, NULL) != 0){ perror("pthread_create"); return 1; }

    // The producer runs on this main thread. It pushes numbered records into
    // the ring as fast as it can.
    unsigned long pushed = 0, ring_drops = 0;
    char rec[64];
    for (long i = 0; i < n_records; i++){
        int len = snprintf(rec, sizeof rec, "STRESS %ld", i);
        if (ring_push(&ring, rec, (size_t)len) == 0) pushed++;
        else                                         ring_drops++;
    }
    atomic_store(&producer_done, 1);
    pthread_join(sh, NULL);            // wait here until the shipper has drained the last records and stopped

    unsigned long sdrops = atomic_load(&send_drops);
    printf("produced        %ld\n",  n_records);
    printf("pushed to ring  %lu\n",  pushed);
    printf("ring drops      %lu   (trylock busy or ring full)\n", ring_drops);
    printf("send drops      %lu   (sendto EAGAIN: logd's receive buffer full)\n", sdrops);
    printf("delivered       %lu   <- must equal `grep -c STRESS <log_path>`\n", pushed - sdrops);
    printf("accounting      %s   (pushed + ring drops == produced)\n",
           (pushed + ring_drops == (unsigned long)n_records) ? "EXACT" : "BROKEN");
    printf("ring_dropped()  %lu   (the ring's own atomic counter; == ring drops)\n",
           ring_dropped(&ring));

    close(sock_fd);
    ring_destroy(&ring);
    return 0;
}
