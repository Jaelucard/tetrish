// Shared daemonization helper, used by tetrisd and tetrislogd.
#ifndef DAEMON_H
#define DAEMON_H

// Double-fork into a background daemon (fork -> setsid -> fork), then point
// stdin/stdout/stderr at /dev/null. Does NOT chdir("/"), on purpose, so
// relative config paths keep resolving from the project root.
// Returns 0 in the surviving daemon, -1 if the first fork fails. The
// intermediate parents _exit and never return at all.
int daemonize(void);

#endif