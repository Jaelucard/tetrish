// tetrisctl: the admin CLI. it connects to tetrisd's control socket (ctl_path),
// sends one command, prints the reply, then exits. short-lived.
// issue #16: for now it speaks a placeholder line ("STATUS\n"); it moves to the
// HTTTP wire format once libhtttp is ready.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>     // socket, connect
#include <sys/un.h>         // sockaddr_un
#include "rc.h"

int main(int argc, char **argv){
    // argv[1] = config path (default .tetrishrc), argv[2] = command (default STATUS).
    // keeps the argv[1]=config convention the other binaries use.
    const char *rc_path = (argc > 1) ? argv[1] : ".tetrishrc";
    const char *command = (argc > 2) ? argv[2] : "STATUS";

    Config cfg;
    if (rc_load(rc_path, &cfg) != 0){
        fprintf(stderr, "tetrisctl: failed to load configuration from %s\n", rc_path);
        return 1;
    }

    // 1. make a stream socket and dial tetrisd's control socket.
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0){ perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(cfg.ctl_path) >= sizeof addr.sun_path){
        fprintf(stderr, "tetrisctl: ctl_path too long: %s\n", cfg.ctl_path);
        close(fd); return 1;
    }
    strncpy(addr.sun_path, cfg.ctl_path, sizeof addr.sun_path - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0){
        // server down or socket missing -> print something useful and exit non-zero.
        fprintf(stderr, "tetrisctl: cannot connect to %s: %s\n"
                        "  (is tetrisd running?)\n", cfg.ctl_path, strerror(errno));
        close(fd); return 1;
    }

    // 2. send the command as one line: "<COMMAND>\n".
    char line[256];
    int len = snprintf(line, sizeof line, "%s\n", command);
    if (write(fd, line, (size_t)len) < 0){ perror("write"); close(fd); return 1; }

    // 3. read the reply until the server closes the connection, echo it to stdout.
    char buf[1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        fwrite(buf, 1, (size_t)n, stdout);
    if (n < 0){ perror("read"); close(fd); return 1; }

    close(fd);
    return 0;
}
