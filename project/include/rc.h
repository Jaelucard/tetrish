// Public interface to the config module.
// Declares the in-memory config struct and the rc_load() that fills it.
// Any program can read the config by #include rc.h, without knowing anything
// about how the parsing works.

// The include guard
#ifndef RC_H
#define RC_H

#include <stdbool.h>      // for bool type
#define RC_PATHLEN 256    // named constant for buffer size

typedef struct {
    // Required
    int listen_port;             // For the TCP port
    // The next six are arrays inside the struct, not pointers.
    // Hand rc_load() a pointer to this struct and it fills the arrays in place,
    // no allocation anywhere, so there is nothing to leak or free.
    char cert_path[RC_PATHLEN]; 
    char key_path[RC_PATHLEN];
    char ca_path[RC_PATHLEN];
    char log_path[RC_PATHLEN];
    char log_ipc[RC_PATHLEN];
    char ctl_path[RC_PATHLEN];
    bool daemonize; 
    char pid_path[RC_PATHLEN]; // Path to the PID file where tetrisd will write its process ID
    char bind_addr[RC_PATHLEN]; // Which interface to listen on. Default is 127.0.0.1
    int tcp_backlog; // How many completed-handshake connections the kernel holds for us before we accept() them. This is what absorbs a burst of arrivals.
    int tcp_keepalive;
    int client_timeout;
    int max_clients; // Most clients connected at once. The cap on how much memory and how many fds we can be made to spend.
    int max_conns_per_ip;   // Per-IP connection cap, enforced at accept(). 0 disables it.
    int handshake_budget;   // Handshakes completed per event-loop pass.
    // Mode the control socket is created with, 0700 by default so only the
    // owner can reach it. This is the ONLY thing between a local user and the
    // control plane: the channel is plaintext by design and /shutdown asks for
    // no credentials, so whoever can open the socket can stop the daemon.
    // tetrisd applies it with a umask around bind() in ctl_listen, since bind()
    // is what creates the socket node.
    int ctl_perm;
    int tick_hz; // How many times per secodn a started room will tick and advance its games. Default is 20
    char log_level[RC_PATHLEN]; // How chatty the log is: "debug", "info", "warn" or "error". Default "info"
    int snapshot_interval; // How often the server writes a full snapshot of the game state, for logging and later replay. Default 60
    int max_rooms; // Rooms allowed active at once. Default 16
    int max_players_per_room; // Seats in one room. Default 6
    char garbage_mq[RC_PATHLEN];   // POSIX mq name for Battle Royale garbage (must start with '/')
} Config;

// returns 0 on success and -1 on failure (missing file/ missing required keys)
int rc_load(const char *path, Config *out);

// True if `word` is one of the directive names rc_load understands. tetrish
// uses it when executing .tetrishrc at startup, to skip the lines already
// consumed as configuration and run only the ones that are commands. Longer
// note on the definition.
int rc_is_directive(const char *word);

#endif