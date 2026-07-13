// Shared daemonization helper, used by tetrisd and tetrislogd.
#ifndef DAEMON_H
#define DAEMON_H

// Double-fork into a background daemon (fork -> setsid -> fork), then redirect
// stdin/stdout/stderr to /dev/null. Deliberately does NOT chdir("/"), so
// relative config paths keep resolving from the project root.
// Returns 0 in the surviving daemon process; returns -1 if the first fork fails
// (the intermediate parents _exit and never return).
int daemonize(void);

#endif