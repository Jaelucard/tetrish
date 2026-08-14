// Shared daemonization helper (see include/daemon.h).
#include <stdio.h>          // perror
#include <stdlib.h>         // _exit lives in unistd, this is here for the exit codes
#include <unistd.h>         // fork, setsid, dup2, close, _exit, STDIN/OUT/ERR
#include <sys/stat.h>       // umask
#include <fcntl.h>          // open, O_RDWR
#include "daemon.h"

int daemonize(void){
    // Process heirarchy: parent -> terminal. Child -> after the first fork.
    // Grandchild after the second fork -> the daemon
    pid_t pid;
    int fd;

    // Double fork, not redis's single fork. Double fork detaches more
    // thoroughly and is the traditional Unix shape, so there is less of it to
    // get wrong.
    // First fork:
    // < 0 fork failed
    // = 0 in the child
    // > 0 in the parent, and the value is the child's PID
    pid = fork();
    if (pid < 0){
        perror("fork failed");
        return -1;
    }
    if (pid > 0){
        _exit(0); // parent leaves, child carries on towards being the daemon
        // _exit, not exit: no stdio flushing in a forked child
    }

    // New session, which detaches us from the controlling terminal and makes
    // this process a session leader.
    // >= 0 success, < 0 failure
    if (setsid() < 0){
        perror("setsid failed");
        _exit(1);
    }

    // Second fork. The survivor is NOT a session leader, so it can never pick
    // up a controlling terminal again.
    pid = fork();
    if (pid < 0){
        _exit(1);
    }
    if (pid > 0){
        _exit(0); // parent exits, grandchild is the real daemon
    }

    // this is the daemon.
    // stdin/stdout/stderr to /dev/null: a detached daemon must not keep the
    // terminal's streams open. No chdir("/"), on purpose, so relative config
    // paths keep resolving from the project root.
    fd = open("/dev/null", O_RDWR);
    if (fd != -1){
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
    // A known mask, rather than inheriting whatever started us.
    //
    // NOT umask(0), on purpose. A zero mask means every file the daemon
    // creates afterwards gets exactly the mode its open() asked for, and
    // fopen() asks for 0666, so tetrisd's pid file a few lines later lands
    // world-writable. Anyone on the box can then put whatever pid they like in
    // it, and the next operator script running `kill $(cat ...pid)` signals a
    // process of their choosing. 022 leaves files owner-writable, which is what
    // a daemon wants. Anything needing a specific mode sets it at the point of
    // creation, the way ctl_listen does for the control socket.
    umask(022);
    return 0;
}