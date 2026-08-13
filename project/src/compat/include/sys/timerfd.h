// macOS shim for Linux's <sys/timerfd.h>. Darwin-only include path; see the
// note at the top of sys/signalfd.h for how src/compat/ is wired in.
//
// Emulation: timerfd_create() returns the read end of a pipe, and
// timerfd_settime() starts a small ticker thread that writes a uint64_t
// expiration count of 1 down that pipe once per period (pacing itself against
// CLOCK_MONOTONIC so error does not accumulate). tetrisd's tick handler does
// read(tfd, &expirations, 8) -- with the pipe it gets one expiration per
// read, and because the event loop is level-triggered a backlog keeps the fd
// readable, so catch-up ticks still happen, just one loop pass apiece.
//
// Lifetime: the owner tears a timer down with plain close(fd), same as
// Linux. The ticker thread notices on its next write (EPIPE -- tetrisd
// ignores SIGPIPE process-wide before any timer can exist) and cleans itself
// up, at most one period later.
#ifndef COMPAT_SYS_TIMERFD_H
#define COMPAT_SYS_TIMERFD_H

#include <time.h>

// Darwin has no POSIX interval timers, so <time.h> does not declare this
// either; the layout is the POSIX one.
struct itimerspec {
    struct timespec it_interval;   // period
    struct timespec it_value;      // first expiration
};

#define TFD_NONBLOCK 0x1
#define TFD_CLOEXEC  0x2

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags,
                    const struct itimerspec *new_value,
                    struct itimerspec *old_value);

#endif
