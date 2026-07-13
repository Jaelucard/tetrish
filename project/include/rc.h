// This is the public interface to the config module. 
// This declares the in-memory config struct and the rc_load() function that fills it
// Now any of the programs we write can read config by #include rc.h without knowing how the parsing works

// Define guard
#ifndef RC_H
#define RC_H

#include <stdbool.h>
#define RC_PATHLEN 256 // named constant for buffer size

typedef struct {
    // Required
    int listen_port;
    char cert_path[RC_PATHLEN];
    char key_path[RC_PATHLEN];
    char ca_path[RC_PATHLEN];
    char log_path[RC_PATHLEN];
    char log_ipc[RC_PATHLEN];
    char ctl_path[RC_PATHLEN];
    bool daemonize;
    char pid_path[RC_PATHLEN];
    char bind_addr[RC_PATHLEN];
    int tcp_backlog;
    int tcp_keepalive;
    int client_timeout;
    int max_clients;
    int ctl_perm;
    int tick_hz;
    char log_level[RC_PATHLEN];
    int snapshot_interval;
    int max_rooms;
    int max_players_per_room;
} Config;

// returns 0 on success and -1 on failure (missing file/ missing required keys)
int rc_load(const char *path, Config *out);

#endif