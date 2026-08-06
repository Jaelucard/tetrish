// macOS shim for Linux's <sys/signalfd.h>.
//
// This header lives under src/compat/include/, which the Makefile adds to the
// include path ON DARWIN ONLY -- on Linux the real kernel header is used and
// none of this exists. That is the whole porting strategy: the daemon and the
// clients keep their Linux-native source, and the platform difference is
// confined to this directory.
//
// The emulation (src/compat/macos.c) delivers signals through ordinary
// sigaction handlers that write a struct signalfd_siginfo down a self-pipe.
// The read end of that pipe is what signalfd() returns, so select()/kqueue
// see it as a readable descriptor exactly like the Linux original. One
// consequence: the caller has BLOCKED these signals expecting fd delivery,
// so the shim unblocks them again -- a handler on a blocked signal would
// never run. All three callers (tetrisd, tetrisu, tetrish-view) follow the
// block-then-signalfd pattern, so this inversion is safe here.
#ifndef COMPAT_SYS_SIGNALFD_H
#define COMPAT_SYS_SIGNALFD_H

#include <signal.h>
#include <stdint.h>

#define SFD_NONBLOCK 0x1
#define SFD_CLOEXEC  0x2

// Linux's struct is exactly 128 bytes with many fields; every caller in this
// tree reads the whole struct but only ever looks at ssi_signo, so the shim
// carries that one field and pads to the same total size. The pipe writes are
// atomic (128 < PIPE_BUF), so a read never sees a torn struct.
struct signalfd_siginfo {
    uint32_t ssi_signo;
    uint8_t  ssi_pad[124];
};

int signalfd(int fd, const sigset_t *mask, int flags);

#endif
