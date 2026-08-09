#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/signalfd.h>

#include "repl.h"
#include "control.h"
#include "jsonish.h"
#include "edit.h"
#include "attach.h"
#include "replay.h"
#include "rc.h"

typedef int (*repl_fn)(const Config *cfg, const char *args);

typedef struct {
    const char *name;
    const char *help;
    repl_fn     fn;
} repl_cmd;

// prints tetrisd's live counters verbatim from its /status control
// endpoint -- the simplest command, and the one that proves the whole
// request/response path works.
static int cmd_status(const Config *cfg, const char *args){
    (void)args;
    char body[8192];
    int st = control_get(cfg, "/status", body, sizeof body);
    if (st < 0){
        printf("status: could not reach tetrisd (is it running? check ctl_path)\n");
        return 1;
    }
    if (body[0]) printf("%s\n", body);
    if (st != 200){
        printf("status: server returned %d\n", st);
        return 1;
    }
    return 0;
}

// Lists rooms from tetrisd's /rooms control endpoint (an array of
// {id, players, started, ticks} objects -- jb_room in src/tetrisd/main.c),
// one aligned row per room.
static int cmd_rooms(const Config *cfg, const char *args){
    (void)args;
    char body[8192];
    int st = control_get(cfg, "/rooms", body, sizeof body);
    if (st < 0){
        printf("rooms: could not reach tetrisd (is it running? check ctl_path)\n");
        return 1;
    }
    if (st != 200){
        printf("rooms: server returned %d\n", st);
        return 1;
    }
    printf("%-16s %-8s %-8s %s\n", "ROOM", "PLAYERS", "STARTED", "TICKS");
    for (int i = 0; ; i++){
        const char *obj = jsonish_nth_object(body, i);
        if (obj == NULL) break;
        char id[32] = "";
        long players = 0, started = 0, ticks = 0;
        jsonish_str(obj, "id", id, sizeof id);
        jsonish_int(obj, "players", &players);
        jsonish_int(obj, "started", &started);
        jsonish_int(obj, "ticks", &ticks);
        printf("%-16s %-8ld %-8s %ld\n", id, players, started ? "yes" : "no", ticks);
    }
    return 0;
}

// Lists connected players from tetrisd's /players control endpoint (an
// array of {player, fd, room} objects -- jb_player in src/tetrisd/main.c).
static int cmd_players(const Config *cfg, const char *args){
    (void)args;
    char body[8192];
    int st = control_get(cfg, "/players", body, sizeof body);
    if (st < 0){
        printf("players: could not reach tetrisd (is it running? check ctl_path)\n");
        return 1;
    }
    if (st != 200){
        printf("players: server returned %d\n", st);
        return 1;
    }
    printf("%-16s %-6s %s\n", "PLAYER", "FD", "ROOM");
    for (int i = 0; ; i++){
        const char *obj = jsonish_nth_object(body, i);
        if (obj == NULL) break;
        char player[32] = "", room[32] = "";
        long fd = -1;
        jsonish_str(obj, "player", player, sizeof player);
        jsonish_int(obj, "fd", &fd);
        jsonish_str(obj, "room", room, sizeof room);
        printf("%-16s %-6ld %s\n", player, fd, room);
    }
    return 0;
}

// Kicks a player, to the boundary: tetrisd has no kick endpoint yet (see
// REQUIRED_FILES.md), so this looks the player's room up via /players to
// build the endpoint it would need, sends that request anyway, and prints
// whatever tetrisd's real response is (today: 404 unknown control path).
static int cmd_kick(const Config *cfg, const char *args){
    char argbuf[128], *rest = NULL;
    strncpy(argbuf, args ? args : "", sizeof argbuf - 1);
    argbuf[sizeof argbuf - 1] = '\0';
    char *player = repl_split_line(argbuf, &rest);
    if (*player == '\0'){
        printf("usage: kick <player>\n");
        return 1;
    }

    char body[8192];
    int st = control_get(cfg, "/players", body, sizeof body);
    if (st < 0){
        printf("kick: could not reach tetrisd (is it running? check ctl_path)\n");
        return 1;
    }
    if (st != 200){
        printf("kick: server returned %d listing players\n", st);
        return 1;
    }

    char room[32] = "";
    int found = 0;
    for (int i = 0; ; i++){
        const char *obj = jsonish_nth_object(body, i);
        if (obj == NULL) break;
        char p[64] = "";
        jsonish_str(obj, "player", p, sizeof p);
        if (strcmp(p, player) == 0){
            jsonish_str(obj, "room", room, sizeof room);
            found = 1;
            break;
        }
    }
    if (!found){
        printf("kick: no such player: %s\n", player);
        return 1;
    }

    char path[128];
    snprintf(path, sizeof path, "/room/%s/player/%s/kick", room, player);
    st = control_get(cfg, path, body, sizeof body);
    if (st < 0){
        printf("kick: could not reach tetrisd\n");
        return 1;
    }
    if (body[0]) printf("%s\n", body);
    printf("kick: server returned %d\n", st);
    return st == 200 ? 0 : 1;
}

// Live board view: joins `room` as an ordinary participant (tetrisd has no
// spectate mode -- see attach.h) and streams STATE frames until any key is
// pressed. Uses the game plane (net.c), not the control plane, so it needs
// cfg's listen_port/bind_addr/ca_path rather than ctl_path.
static int cmd_attach(const Config *cfg, const char *args){
    char argbuf[64], *rest = NULL;
    strncpy(argbuf, args ? args : "", sizeof argbuf - 1);
    argbuf[sizeof argbuf - 1] = '\0';
    char *room = repl_split_line(argbuf, &rest);
    if (*room == '\0'){
        printf("usage: attach <room>\n");
        return 1;
    }
    return attach_run(cfg, room);
}

// Replays a tetrislogd log file in the interactive ncurses viewer (space
// pause, left/right seek, +/- speed, q quit). Shares replay_run with the
// original `tetrish-view [--verify] <log-file> ...` CLI usage (main.c);
// --verify is not exposed here since a script wanting that headless check
// should use the CLI form directly, not drive it through the repl.
static int cmd_replay(const Config *cfg, const char *args){
    (void)cfg;
    char argbuf[256];
    strncpy(argbuf, args ? args : "", sizeof argbuf - 1);
    argbuf[sizeof argbuf - 1] = '\0';

    char *rest1 = NULL;
    char *logpath = repl_split_line(argbuf, &rest1);
    if (*logpath == '\0'){
        printf("usage: replay <logfile> [room] [player]\n");
        return 1;
    }
    char *rest2 = NULL;
    char *room   = repl_split_line(rest1, &rest2);
    char *rest3  = NULL;
    char *player = repl_split_line(rest2, &rest3);

    return replay_run(logpath, room, player, 0);
}

static const repl_cmd COMMANDS[] = {
    { "status",  "tetrisd's live counters (rooms, players, uptime)", cmd_status  },
    { "rooms",   "list rooms: players, started, ticks",              cmd_rooms   },
    { "players", "list connected players and their room",            cmd_players },
    { "kick",    "kick <player> (server has no kick endpoint yet)",  cmd_kick    },
    { "attach",  "attach <room>: live board view (any key detaches)", cmd_attach },
    { "replay",  "replay <logfile> [room] [player]: interactive viewer", cmd_replay },
};
#define NCOMMANDS (int)(sizeof(COMMANDS) / sizeof(COMMANDS[0]))

// lists every command with its one-line help, generated from the same
// COMMANDS table dispatch uses so help can never drift from reality.
static void print_help(void){
    printf("commands:\n");
    for (int i = 0; i < NCOMMANDS; i++)
        printf("  %-10s %s\n", COMMANDS[i].name, COMMANDS[i].help);
    printf("  %-10s %s\n", "help", "show this list (also: ?)");
    printf("  %-10s %s\n", "quit", "leave the console (also: exit, ctrl-d)");
}

// Runs one already-split command line: quit/exit/help/? are handled here
// directly (they are not part of COMMANDS[]); anything else is looked up in
// COMMANDS[] and reported unknown if it isn't there. Shared by both the
// piped (fgets) loop and the interactive (raw-tty) loop so the dispatch
// logic exists in exactly one place.
static void dispatch_or_quit(const Config *cfg, const char *name, const char *args, int *running){
    if (strcmp(name, "quit") == 0 || strcmp(name, "exit") == 0){ *running = 0; return; }
    if (strcmp(name, "help") == 0 || strcmp(name, "?") == 0){ print_help(); return; }

    for (int i = 0; i < NCOMMANDS; i++)
        if (strcmp(name, COMMANDS[i].name) == 0){ COMMANDS[i].fn(cfg, args); return; }

    printf("unknown command: %s\n", name);
    print_help();
}

// trims the trailing newline (and any \r) fgets leaves on a line.
static void strip_eol(char *line){
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';
}

// The non-interactive loop: fgets from stdin, one command per line. This is
// what runs when stdin isn't a tty (piped input, e.g. tests), so scripted
// input like `printf "status\nquit\n" | tetrish-view` keeps working -- no
// termios, no history.
static int repl_batch(const Config *cfg){
    printf("tetrish-view admin console. type 'help' for commands, 'quit' to leave.\n");

    char line[256];
    int running = 1;
    while (running){
        printf("tetrish> ");
        fflush(stdout);
        if (fgets(line, sizeof line, stdin) == NULL){ printf("\n"); break; }   // EOF / ctrl-d
        strip_eol(line);

        char *args = NULL;
        char *name = repl_split_line(line, &args);
        if (*name == '\0') continue;                      // blank line

        dispatch_or_quit(cfg, name, args, &running);
    }
    return 0;
}

static struct termios g_orig_termios;
static int g_raw_active = 0;

// Restores the tty to whatever mode it was in before enable_raw_mode(),
// registered with atexit() so it runs on every exit path, not just a clean
// quit (a die()/exit() elsewhere must not leave the user's shell in raw
// mode).
static void restore_termios(void){
    if (g_raw_active) tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
}

// Puts stdin into raw-ish mode for the line editor: no local echo (the repl
// draws the line itself), no canonical line buffering (bytes arrive as
// typed), no input CR/NL translation (edit_feed tells \r and \n apart
// itself) and no ctrl-s/ctrl-q flow control. ISIG is left ON, deliberately:
// the tty driver still turns ctrl-c into SIGINT, which is blocked at the
// process level and read back as a signalfd event -- the same clean-signal
// pattern src/tetrisu/main.c uses, rather than a raw 0x03 byte to parse.
static int enable_raw_mode(void){
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) < 0) return -1;
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) return -1;
    g_raw_active = 1;
    atexit(restore_termios);
    return 0;
}

// Redraws the current edit buffer on its own line: return to column 0,
// clear to end of line, reprint the prompt and buffer. Cursor always sits
// at the end of the buffer (edit_state_t has no cursor-in-the-middle
// concept), so nothing further needs positioning.
static void redraw_line(const edit_state_t *st){
    printf("\r\x1b[K""tetrish> %.*s", (int)st->len, st->buf);
    fflush(stdout);
}

// The interactive loop: raw tty, one byte at a time through edit_feed(),
// with up/down history. select() over stdin + a signalfd mirrors
// src/tetrisu/main.c's loop so ctrl-c/ctrl-\ get a clean shutdown (restoring
// the tty via the atexit handler) instead of leaving raw mode stuck. A repl
// keypress never blocks on the network -- each command's own control_get()
// call is what actually waits, same as the batch loop.
static int repl_interactive(const Config *cfg){
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0){
        fprintf(stderr, "tetrish-view: sigprocmask: %s\n", strerror(errno));
        return 1;
    }
    int sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigfd < 0){
        fprintf(stderr, "tetrish-view: signalfd: %s\n", strerror(errno));
        return 1;
    }
    if (enable_raw_mode() < 0){
        fprintf(stderr, "tetrish-view: tcsetattr: %s\n", strerror(errno));
        close(sigfd);
        return 1;
    }

    edit_state_t st;
    edit_init(&st);
    printf("tetrish-view admin console. type 'help' for commands, 'quit' to leave.\n");
    printf("tetrish> ");
    fflush(stdout);

    int running = 1;
    while (running){
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(sigfd, &rfds);
        int maxfd = sigfd > STDIN_FILENO ? sigfd : STDIN_FILENO;

        int r = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (r < 0){
            if (errno == EINTR) continue;    // a signal beat select(); sigfd will still be readable
            fprintf(stderr, "\ntetrish-view: select: %s\n", strerror(errno));
            break;
        }

        if (FD_ISSET(sigfd, &rfds)){
            struct signalfd_siginfo si;
            if (read(sigfd, &si, sizeof si) == (ssize_t)sizeof si){
                printf("\n");
                break;
            }
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)){
            unsigned char byte;
            ssize_t n = read(STDIN_FILENO, &byte, 1);
            if (n <= 0){ printf("\n"); break; }

            if (byte == 0x04 && st.len == 0){    // ctrl-d on an empty line: quit
                printf("\n");
                break;
            }

            edit_result_t res = edit_feed(&st, byte);
            if (res == EDIT_REDRAW){
                redraw_line(&st);
            } else if (res == EDIT_LINE_READY){
                printf("\n");
                char line[EDIT_LINE_MAX];
                memcpy(line, st.buf, st.len);
                line[st.len] = '\0';
                edit_commit(&st);

                char *args = NULL;
                char *name = repl_split_line(line, &args);
                if (*name != '\0') dispatch_or_quit(cfg, name, args, &running);

                if (running){ printf("tetrish> "); fflush(stdout); }
            }
        }
    }

    close(sigfd);
    return 0;
}

int repl_run(const char *rc_path){
    // tetrisd may close a control connection first (shutdown mid-request,
    // or its 2s receive timeout), making control_get's write() raise
    // SIGPIPE. The default action kills the process WITHOUT running atexit
    // handlers, which would leave the tty stuck in raw mode. Ignore it so
    // the write just fails with EPIPE and the command reports the error --
    // same guard, same reason as tetrisctl. (The game plane is already
    // safe: tetrissh sends with MSG_NOSIGNAL.)
    signal(SIGPIPE, SIG_IGN);

    Config cfg;
    if (rc_load(rc_path, &cfg) != 0){
        fprintf(stderr, "tetrish-view: failed to load configuration from %s\n", rc_path);
        return 1;
    }

    // A real terminal gets raw-mode editing and history; anything else
    // (a pipe, a redirected file, tests) falls back to plain fgets so
    // scripted/non-interactive input keeps working.
    if (isatty(STDIN_FILENO)) return repl_interactive(&cfg);
    return repl_batch(&cfg);
}
