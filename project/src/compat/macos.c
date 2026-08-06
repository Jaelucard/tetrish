// macOS implementations of the Linux kernel APIs this tree leans on:
// signalfd, epoll, timerfd, and POSIX message queues. Compiled and linked ON
// DARWIN ONLY (the Makefile's uname switch); on Linux this file is never
// built and the real system calls are used. Each shim's design rationale
// lives in its matching header under src/compat/include/ -- this file is
// just the mechanics.
//
// The guiding rule: emulate only the behaviour the callers in this tree
// actually rely on, and make anything else fail loudly (EINVAL / missing
// declaration) rather than approximately work.

#ifdef __APPLE__

#include <sys/epoll.h>       // resolves to our shim headers via -I
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <mqueue.h>

#include <sys/event.h>       // kqueue / kevent
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// --- small helpers ----------------------------------------------------------

static int set_nonblock(int fd){
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int set_cloexec(int fd){
    int fl = fcntl(fd, F_GETFD, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

// --- signalfd ---------------------------------------------------------------
// One instance per process, which is what all three callers do. The handler
// writes the full 128-byte struct in one call: write() on a pipe is atomic
// below PIPE_BUF, so the reader never sees a torn record. The write end is
// always non-blocking -- if the pipe is somehow full the signal is dropped,
// which mirrors the kernel's own coalescing of pending signals.

static int sig_pipe_wr = -1;

static void sigfd_handler(int signo){
    struct signalfd_siginfo si = {0};
    si.ssi_signo = (uint32_t)signo;
    // write() is async-signal-safe; the result is deliberately ignored (a
    // full pipe means the reader is hopelessly behind anyway).
    ssize_t n = write(sig_pipe_wr, &si, sizeof si);
    (void)n;
}

int signalfd(int fd, const sigset_t *mask, int flags){
    if (fd != -1 || mask == NULL){
        errno = EINVAL;           // mask-update form not emulated; nobody uses it
        return -1;
    }
    int p[2];
    if (pipe(p) < 0) return -1;
    set_nonblock(p[1]);
    set_cloexec(p[1]);
    if (flags & SFD_NONBLOCK) set_nonblock(p[0]);
    if (flags & SFD_CLOEXEC)  set_cloexec(p[0]);
    sig_pipe_wr = p[1];

    // SA_RESTART so unrelated slow syscalls (the blocking handshake sends,
    // accept) resume instead of failing with EINTR. select() still returns
    // EINTR on Darwin regardless, and every caller's loop handles that.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sigfd_handler;
    sa.sa_flags   = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    for (int s = 1; s < NSIG; s++)
        if (sigismember(mask, s))
            sigaction(s, &sa, NULL);

    // The callers blocked these signals expecting fd delivery; handlers only
    // run on unblocked signals, so undo that for the calling thread. Threads
    // created earlier (tetrisd's logshipper) keep them blocked, which is
    // exactly the Linux delivery pattern: only the event-loop thread reacts.
    pthread_sigmask(SIG_UNBLOCK, mask, NULL);
    return p[0];
}

// --- epoll over kqueue ------------------------------------------------------

int epoll_create1(int flags){
    int kq = kqueue();
    if (kq >= 0 && (flags & EPOLL_CLOEXEC)) set_cloexec(kq);
    return kq;
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event){
    struct kevent kev[2];
    int n = 0;

    if (op == EPOLL_CTL_DEL){
        // epoll forbids interest we never registered with ENOENT; kevent does
        // the same, so errors pass straight through. Callers here ignore DEL
        // failures anyway (the "stale fd safety" path).
        EV_SET(&kev[n++], fd, EVFILT_READ,  EV_DELETE, 0, 0, NULL);
        struct kevent kw;
        EV_SET(&kw, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        (void)kevent(epfd, &kw, 1, NULL, 0, NULL);   // best effort; usually absent
        return kevent(epfd, kev, n, NULL, 0, NULL) < 0 ? -1 : 0;
    }

    if (event == NULL || (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD)){
        errno = EINVAL;
        return -1;
    }
    if (event->events & ~(uint32_t)(EPOLLIN | EPOLLOUT)){
        errno = EINVAL;           // ET/ONESHOT/etc: refuse rather than fake
        return -1;
    }
    // kqueue's EV_ADD on an existing filter updates it, so ADD and MOD are
    // the same operation here. udata carries the caller's epoll_data.
    void *ud = (void *)(uintptr_t)event->data.u64;
    if (event->events & EPOLLIN)
        EV_SET(&kev[n++], fd, EVFILT_READ,  EV_ADD, 0, 0, ud);
    if (event->events & EPOLLOUT)
        EV_SET(&kev[n++], fd, EVFILT_WRITE, EV_ADD, 0, 0, ud);
    if (n == 0){ errno = EINVAL; return -1; }
    return kevent(epfd, kev, n, NULL, 0, NULL) < 0 ? -1 : 0;
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout){
    if (maxevents <= 0){ errno = EINVAL; return -1; }
    struct kevent kevs[64];
    if (maxevents > 64) maxevents = 64;

    struct timespec ts, *tsp = NULL;
    if (timeout >= 0){
        ts.tv_sec  = timeout / 1000;
        ts.tv_nsec = (long)(timeout % 1000) * 1000000L;
        tsp = &ts;
    }
    int n = kevent(epfd, NULL, 0, kevs, maxevents, tsp);
    if (n < 0) return -1;         // EINTR passes through, as with epoll

    for (int i = 0; i < n; i++){
        uint32_t ev = 0;
        if (kevs[i].filter == EVFILT_READ)  ev |= EPOLLIN;
        if (kevs[i].filter == EVFILT_WRITE) ev |= EPOLLOUT;
        if (kevs[i].flags & EV_EOF)         ev |= EPOLLHUP;
        if (kevs[i].flags & EV_ERROR)       ev  = EPOLLERR;
        events[i].events   = ev;
        events[i].data.u64 = (uint64_t)(uintptr_t)kevs[i].udata;
    }
    return n;
}

// --- timerfd ----------------------------------------------------------------

#define CTIMER_MAX 64

typedef struct {
    int       in_use;
    int       rfd, wfd;          // pipe: caller reads rfd, ticker writes wfd
    long long period_ns;
    int       armed;             // ticker thread running
} ctimer_t;

static ctimer_t       ctimers[CTIMER_MAX];
static pthread_mutex_t ctimer_mu = PTHREAD_MUTEX_INITIALIZER;

static void *ctimer_thread(void *arg){
    ctimer_t *t = (ctimer_t *)arg;
    long long period = t->period_ns;

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;){
        // Pace against the absolute clock so each iteration's scheduling
        // error does not accumulate into drift (nanosleep is relative;
        // Darwin has no clock_nanosleep).
        next.tv_nsec += period % 1000000000LL;
        next.tv_sec  += period / 1000000000LL;
        if (next.tv_nsec >= 1000000000L){ next.tv_nsec -= 1000000000L; next.tv_sec++; }

        struct timespec now, rem;
        clock_gettime(CLOCK_MONOTONIC, &now);
        rem.tv_sec  = next.tv_sec  - now.tv_sec;
        rem.tv_nsec = next.tv_nsec - now.tv_nsec;
        if (rem.tv_nsec < 0){ rem.tv_nsec += 1000000000L; rem.tv_sec--; }
        if (rem.tv_sec >= 0)
            nanosleep(&rem, NULL);
        // else: we are behind; fire immediately (the catch-up tick)

        uint64_t one = 1;
        if (write(t->wfd, &one, sizeof one) < 0){
            if (errno == EAGAIN) continue;   // pipe full: reader behind, coalesce
            break;                           // EPIPE/EBADF: owner closed the read end
        }
    }

    pthread_mutex_lock(&ctimer_mu);
    close(t->wfd);
    t->in_use = 0;
    pthread_mutex_unlock(&ctimer_mu);
    return NULL;
}

int timerfd_create(int clockid, int flags){
    (void)clockid;                          // CLOCK_MONOTONIC assumed; it is
    int p[2];
    if (pipe(p) < 0) return -1;
    set_nonblock(p[1]);                     // ticker must never block on a full pipe
    set_cloexec(p[1]);
    if (flags & TFD_NONBLOCK) set_nonblock(p[0]);
    if (flags & TFD_CLOEXEC)  set_cloexec(p[0]);

    pthread_mutex_lock(&ctimer_mu);
    ctimer_t *slot = NULL;
    for (int i = 0; i < CTIMER_MAX; i++){
        // The OS can hand back an fd number whose previous ticker is still
        // winding down (it clears its slot only after noticing EPIPE, up to
        // one period after the close). Orphan such a slot so lookups can
        // never bind the new timer to the dying thread.
        if (ctimers[i].in_use && ctimers[i].rfd == p[0])
            ctimers[i].rfd = -1;
        if (slot == NULL && !ctimers[i].in_use)
            slot = &ctimers[i];
    }
    if (slot == NULL){
        pthread_mutex_unlock(&ctimer_mu);
        close(p[0]); close(p[1]);
        errno = EMFILE;
        return -1;
    }
    slot->in_use    = 1;
    slot->rfd       = p[0];
    slot->wfd       = p[1];
    slot->period_ns = 0;
    slot->armed     = 0;
    pthread_mutex_unlock(&ctimer_mu);
    return p[0];
}

int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
                    struct itimerspec *old_value){
    (void)flags;                            // relative arming only; that's what's used
    if (old_value) memset(old_value, 0, sizeof *old_value);
    if (new_value == NULL){ errno = EINVAL; return -1; }

    long long period = (long long)new_value->it_interval.tv_sec * 1000000000LL
                     + new_value->it_interval.tv_nsec;
    if (period <= 0){ errno = EINVAL; return -1; }   // disarm/one-shot: unused here

    pthread_mutex_lock(&ctimer_mu);
    ctimer_t *t = NULL;
    for (int i = 0; i < CTIMER_MAX; i++)
        if (ctimers[i].in_use && ctimers[i].rfd == fd){ t = &ctimers[i]; break; }
    if (t == NULL){
        pthread_mutex_unlock(&ctimer_mu);
        errno = EINVAL;
        return -1;
    }
    t->period_ns = period;
    int need_thread = !t->armed;
    t->armed = 1;
    pthread_mutex_unlock(&ctimer_mu);

    if (need_thread){
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&th, &at, ctimer_thread, t);
        pthread_attr_destroy(&at);
        if (rc != 0){
            pthread_mutex_lock(&ctimer_mu);
            t->armed = 0;
            pthread_mutex_unlock(&ctimer_mu);
            errno = rc;
            return -1;
        }
    }
    return 0;
}

// --- POSIX message queues over AF_UNIX datagrams ----------------------------

#define CMQ_MAX       8
#define CMQ_PATH_MAX  92          // fits sockaddr_un.sun_path (104 on Darwin)

typedef struct {
    int  in_use;
    int  fd;                      // the mqd_t we hand out
    int  owner;                   // bound the path (O_CREAT side)
    char path[CMQ_PATH_MAX];
} cmq_t;

static cmq_t          cmqs[CMQ_MAX];
static pthread_mutex_t cmq_mu = PTHREAD_MUTEX_INITIALIZER;

// "/tetris-garbage" -> "/tmp/mq.tetris-garbage". Slashes inside the name
// (mq names are single-component by spec, but be safe) become dots.
static int cmq_path(const char *name, char *out, size_t cap){
    if (name == NULL || name[0] != '/') { errno = EINVAL; return -1; }
    int n = snprintf(out, cap, "/tmp/mq.%s", name + 1);
    if (n < 0 || (size_t)n >= cap){ errno = ENAMETOOLONG; return -1; }
    for (char *p = out + 8; *p; p++)
        if (*p == '/') *p = '.';
    return 0;
}

static cmq_t *cmq_find(int fd){
    for (int i = 0; i < CMQ_MAX; i++)
        if (cmqs[i].in_use && cmqs[i].fd == fd) return &cmqs[i];
    return NULL;
}

mqd_t mq_open(const char *name, int oflag, ...){
    char path[CMQ_PATH_MAX];
    if (cmq_path(name, path, sizeof path) < 0) return (mqd_t)-1;

    long maxmsg = 10, msgsize = 512;        // POSIX-ish fallbacks
    if (oflag & O_CREAT){
        va_list ap;
        va_start(ap, oflag);
        (void)va_arg(ap, int);              // mode_t promotes to int; unused
        struct mq_attr *attr = va_arg(ap, struct mq_attr *);
        va_end(ap);
        if (attr != NULL){
            if (attr->mq_maxmsg  > 0) maxmsg  = attr->mq_maxmsg;
            if (attr->mq_msgsize > 0) msgsize = attr->mq_msgsize;
        }
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return (mqd_t)-1;
    set_cloexec(fd);
    if (oflag & O_NONBLOCK) set_nonblock(fd);

    if (oflag & O_CREAT){
        // Rebinding is the create: a stale socket file from a dead daemon
        // would fail the bind, so clear it first (see the header note on the
        // persistence difference vs a real mq).
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
        unlink(path);
        if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0){
            close(fd);
            return (mqd_t)-1;
        }
        // The depth bound. A real mq refuses the (maxmsg+1)th message; here
        // the receive buffer refuses a send once roughly maxmsg messages
        // (plus per-datagram overhead) are queued. Same failure mode, fuzzy
        // threshold.
        int rcvbuf = (int)(maxmsg * (msgsize + 32));
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    }

    pthread_mutex_lock(&cmq_mu);
    cmq_t *slot = NULL;
    for (int i = 0; i < CMQ_MAX; i++)
        if (!cmqs[i].in_use){ slot = &cmqs[i]; break; }
    if (slot == NULL){
        pthread_mutex_unlock(&cmq_mu);
        close(fd);
        errno = EMFILE;
        return (mqd_t)-1;
    }
    slot->in_use = 1;
    slot->fd     = fd;
    slot->owner  = (oflag & O_CREAT) ? 1 : 0;
    snprintf(slot->path, sizeof slot->path, "%s", path);
    pthread_mutex_unlock(&cmq_mu);
    return (mqd_t)fd;
}

int mq_send(mqd_t mqdes, const char *msg_ptr, size_t msg_len, unsigned msg_prio){
    (void)msg_prio;
    pthread_mutex_lock(&cmq_mu);
    cmq_t *q = cmq_find(mqdes);
    struct sockaddr_un sa;
    if (q != NULL){
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        strncpy(sa.sun_path, q->path, sizeof sa.sun_path - 1);
    }
    pthread_mutex_unlock(&cmq_mu);
    if (q == NULL){ errno = EBADF; return -1; }
    // sendto its own bound path works too: the datagram loops back into the
    // socket's receive queue, which is precisely tetrisd's O_RDWR usage.
    ssize_t n = sendto(mqdes, msg_ptr, msg_len, 0,
                       (struct sockaddr *)&sa, sizeof sa);
    return n < 0 ? -1 : 0;
}

ssize_t mq_receive(mqd_t mqdes, char *msg_ptr, size_t msg_len, unsigned *msg_prio){
    if (msg_prio) *msg_prio = 0;
    return recv(mqdes, msg_ptr, msg_len, 0);
}

int mq_close(mqd_t mqdes){
    pthread_mutex_lock(&cmq_mu);
    cmq_t *q = cmq_find(mqdes);
    if (q != NULL) q->in_use = 0;
    pthread_mutex_unlock(&cmq_mu);
    return close(mqdes);
}

int mq_unlink(const char *name){
    char path[CMQ_PATH_MAX];
    if (cmq_path(name, path, sizeof path) < 0) return -1;
    return unlink(path);
}

#endif // __APPLE__
