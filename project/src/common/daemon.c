// Shared daemonization helper (see include/daemon.h).
#include <stdio.h>          // perror
#include <stdlib.h>         // _exit is in unistd, but keep stdlib for exit codes
#include <unistd.h>         // fork, setsid, dup2, close, _exit, STDIN/OUT/ERR
#include <sys/stat.h>       // umask
#include <fcntl.h>          // open, O_RDWR
#include "daemon.h"

int daemonize(void){
    // Process heirarchy: Parent process -> terminal. Child process -> after first
    // fork. Grandchild after second fork -> becomes daemon
    pid_t pid;
    int fd;

    // Double fork to create daemon
    // Deliberately chosen double fork over a single fork unlike redis because this is less likely to break
    // Double fork has a more through daemonization as well as better resource cleanup and follows traditional Unix daemon patterns more cleanly
    // First fork to create child process
    // < 0 if fork failed
    // = 0 for child process (returned to child)
    // > 0 for parent process (returned to parent, value is child's PID)
    pid = fork();
    if (pid < 0){
        perror("fork failed");
        return -1;
    }
    if (pid > 0){
        _exit(0); // Parent exits to avoid creating a zombie process
        // The child continues as daemon process
        // _exit(0) is used instead of exit(0) to avoid stdio cleanup issues in child process
    }

    // Create new session to detach from controlling terminal
    // makes the process a session leader
    // return >= 0 for success and < 0 for failure
    if (setsid() < 0){
        perror("setsid failed"); // Failed to create new session
        _exit(1);
    }

    // Creating a second fork. This guarantees the daemon is NOT a session
    // leader, so it can never reacquire a controlling terminal.
    pid = fork();
    if (pid < 0){
        _exit(1);
    }
    if (pid > 0){
        _exit(0); // Parent exits, grandchild continues as the true daemon
    }

    // this is the daemon.
    // Redirect stdin/stdout/stderr to /dev/null: a detached daemon must not
    // hold the terminal's streams open. We deliberately do NOT chdir("/"),
    // so relative config paths keep resolving from the project root.
    fd = open("/dev/null", O_RDWR);
    if (fd != -1){
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
    umask(0);                               // don't inherit a restrictive mask
    return 0;
}