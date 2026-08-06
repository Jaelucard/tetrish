// macOS shim for Linux's <sys/epoll.h>, backed by kqueue. Darwin-only
// include path; see the note at the top of sys/signalfd.h.
//
// Scope: exactly what tetrisd uses -- level-triggered EPOLLIN (EPOLLOUT is
// wired through for completeness), EPOLL_CTL_ADD/MOD/DEL, and a millisecond
// timeout in epoll_wait. kqueue is level-triggered by default and its
// returned ident is the fd, so the mapping is direct. Edge-triggered flags
// (EPOLLET, EPOLLONESHOT) are deliberately not defined: code that needs them
// should fail to compile here rather than silently behave differently.
//
// epoll_data is preserved through kevent's udata pointer, so data.u64/ptr
// round-trip intact on LP64 (which every supported Darwin is).
#ifndef COMPAT_SYS_EPOLL_H
#define COMPAT_SYS_EPOLL_H

#include <stdint.h>

#define EPOLLIN  0x001
#define EPOLLOUT 0x004
#define EPOLLERR 0x008
#define EPOLLHUP 0x010

#define EPOLL_CLOEXEC 0x1

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

typedef union epoll_data {
    void     *ptr;
    int       fd;
    uint32_t  u32;
    uint64_t  u64;
} epoll_data_t;

struct epoll_event {
    uint32_t     events;
    epoll_data_t data;
};

int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);

#endif
