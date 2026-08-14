#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "rc.h"


// ====== Helper functions for rc_load() ======

// Is this word a configuration directive rather than a command?
//
// .tetrishrc has two readers. The daemons read it as configuration; tetrish
// also executes it at startup, the way a shell runs its rc file. A line
// rc_load already ate as a directive is not a command, so the shell skips it
// instead of trying to exec "listen_port" and printing a not-found error for
// every directive in the file. Anything NOT in this list goes to the normal
// command dispatch, so real shell lines (dspawn ...) can live in the same file
// and still run.
//
// This list mirrors the dispatch chain in rc_load below. New directive there,
// new entry here.
int rc_is_directive(const char *word){
    static const char *keys[] = {
        "cert_path", "key_path", "ca_path", "log_path", "log_ipc", "ctl_path",
        "pid_path", "bind", "log_level", "garbage_mq", "listen_port",
        "tcp_backlog", "tcp_keepalive", "client_timeout", "max_clients",
        "max_conns_per_ip", "handshake_budget", "tick_hz", "snapshot_interval",
        "max_rooms", "max_players_per_room", "ctl_perm", "daemonize",
    };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (strcmp(word, keys[i]) == 0) return 1;
    return 0;
}

// String to int with error checking. 0 on success, -1 on failure.
static int parse_int(const char *s, int base, int *out){
    char *end;
    errno = 0; // strtol sets errno to ERANGE on overflow, so we clear it first.
    long v = strtol(s, &end, base); // end gets set to the first character strtol couldnt consume and that is the whole error detection mechanism
    if (end == s || *end != '\0' || errno == ERANGE) 
        return -1; // Three different ways to fail: no digits, leftover junk, or overflow/underflow
    *out = (int)v; // Only write through the out-pointer if we succeed, so a failed parse leaves the caller's field at its default
    return 0;
}

// strncpy doesnt terminate when the source fills the buffer.
// If src is shorter than dstsz, strncpy will fill the rest of dst with null bytes.
// If src is longer than dstsz, strncpy will not null-terminate dst.
// So terminate it here by hand, every time.
static void copy_str(char *dst, const char *src, size_t dstsz) {
    strncpy(dst, src, dstsz - 1);
    dst[dstsz - 1] = '\0';
}

// yes/no value. 0 and writes *out on success, -1 if the word is neither.
static int parse_bool(const char *s, bool *out) {
    if (strcmp(s, "yes") == 0 || strcmp(s, "true") == 0 || strcmp(s, "1") == 0)
        { *out = true;  return 0; }
    if (strcmp(s, "no") == 0 || strcmp(s, "false") == 0 || strcmp(s, "0") == 0)
        { *out = false; return 0; }
    return -1;
}

// -------------------------------------------------------------------------

int rc_load(const char *path, Config *out){
    // Phase 1: Prefill defaults into *out
    out->tick_hz = 20;
    out->listen_port = -1; // sentinel: "not seen yet"(required field)
    out->tcp_backlog = 128;
    out->tcp_keepalive = 120;
    out->daemonize = false; // bool field
    out->max_clients = 64;
    out->max_conns_per_ip = 8; // per-IP cap at accept(); 0 turns the check off
    out->handshake_budget = 32; // admissions per event-loop pass; see tetrisd
    out->ctl_perm = 0700; //in octa literal
    strcpy(out->bind_addr, "127.0.0.1"); //strcpy copies the characters in since C doesnt let you assign to array
    out->cert_path[0] = '\0';
    out->key_path[0] = '\0';
    out->ca_path[0] = '\0';
    out->log_path[0] = '\0';
    out->log_ipc[0] = '\0';
    out->ctl_path[0] = '\0';
    strcpy(out->pid_path, "var/run/tetrisd.pid");
    strcpy(out->log_level, "info");
    out->client_timeout = 60;
    out->snapshot_interval = 60;
    out->max_rooms = 16;
    out->max_players_per_room = 6;
    strcpy(out->garbage_mq, "/tetrish-garbage"); // POSIX mq names must start with '/'
    // Phase 2: open the file. Fail loudly if it is missing: a daemon with no
    // config must refuse to start, not run on guesses.
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "rc_load: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }

    // Phase 3: read the file one line at a time.
    char line[512];
    while (fgets(line, sizeof line, fp)) {
        // (a) Cut at the first '#' (comment) or '\n' (newline left by fgets).
        // strcspn returns the index of the first matching char (or the end),
        // and writing '\0' there truncates the line at that point.
        line[strcspn(line, "#\n")] = '\0';

        // (b) Split into "key value". The sscanf widths (%63s/%255s) are not
        // optional; they are what stops a long token overflowing these
        // buffers. n < 2 means no value: blank or comment-only (skip), or a
        // lone key (malformed, warn).
        char key[64], value[RC_PATHLEN];
        int n = sscanf(line, "%63s %255s", key, value);
        if (n <= 0)
            continue;                       // blank or comment-only line
        if (n == 1) {
            fprintf(stderr, "rc_load: directive '%s' has no value (ignored)\n", key);
            continue;
        }

        // (c) Dispatch: match the key, convert the value, store it. strcmp
        // returns 0 on a match. A second line with the same key just
        // overwrites the first, so "last duplicate wins" comes for free.
        // Required string fields:
        if      (strcmp(key, "cert_path") == 0) copy_str(out->cert_path, value, RC_PATHLEN);
        else if (strcmp(key, "key_path")  == 0) copy_str(out->key_path,  value, RC_PATHLEN);
        else if (strcmp(key, "ca_path")   == 0) copy_str(out->ca_path,   value, RC_PATHLEN);
        else if (strcmp(key, "log_path")  == 0) copy_str(out->log_path,  value, RC_PATHLEN);
        else if (strcmp(key, "log_ipc")   == 0) copy_str(out->log_ipc,   value, RC_PATHLEN);
        else if (strcmp(key, "ctl_path")  == 0) copy_str(out->ctl_path,  value, RC_PATHLEN);
        // Optional string fields (the directive "bind" maps to field bind_addr):
        else if (strcmp(key, "pid_path")  == 0) copy_str(out->pid_path,  value, RC_PATHLEN);
        else if (strcmp(key, "bind")      == 0) copy_str(out->bind_addr, value, RC_PATHLEN);
        else if (strcmp(key, "log_level") == 0) copy_str(out->log_level, value, RC_PATHLEN);
        else if (strcmp(key, "garbage_mq") == 0) copy_str(out->garbage_mq, value, RC_PATHLEN);
        // Integer fields (base 10):
        else if (strcmp(key, "listen_port")          == 0) { if (parse_int(value, 10, &out->listen_port)          != 0) goto badnum; }
        else if (strcmp(key, "tcp_backlog")          == 0) { if (parse_int(value, 10, &out->tcp_backlog)          != 0) goto badnum; }
        else if (strcmp(key, "tcp_keepalive")        == 0) { if (parse_int(value, 10, &out->tcp_keepalive)        != 0) goto badnum; }
        else if (strcmp(key, "client_timeout")       == 0) { if (parse_int(value, 10, &out->client_timeout)       != 0) goto badnum; }
        else if (strcmp(key, "max_clients")          == 0) { if (parse_int(value, 10, &out->max_clients)          != 0) goto badnum; }
        else if (strcmp(key, "max_conns_per_ip")     == 0) { if (parse_int(value, 10, &out->max_conns_per_ip)     != 0) goto badnum; }
        else if (strcmp(key, "handshake_budget")     == 0) { if (parse_int(value, 10, &out->handshake_budget)     != 0) goto badnum; }
        else if (strcmp(key, "tick_hz")              == 0) { if (parse_int(value, 10, &out->tick_hz)              != 0) goto badnum; }
        else if (strcmp(key, "snapshot_interval")    == 0) { if (parse_int(value, 10, &out->snapshot_interval)    != 0) goto badnum; }
        else if (strcmp(key, "max_rooms")            == 0) { if (parse_int(value, 10, &out->max_rooms)            != 0) goto badnum; }
        else if (strcmp(key, "max_players_per_room") == 0) { if (parse_int(value, 10, &out->max_players_per_room) != 0) goto badnum; }
        // ctl_perm is octal in the file ("0700"), so parse base 8:
        else if (strcmp(key, "ctl_perm") == 0) { if (parse_int(value, 8, &out->ctl_perm) != 0) goto badnum; }
        // Boolean:
        else if (strcmp(key, "daemonize") == 0) {
            if (parse_bool(value, &out->daemonize) != 0) {
                fprintf(stderr, "rc_load: daemonize wants yes/no, got '%s'\n", value);
                fclose(fp);
                return -1;
            }
        }
        // Unknown directive: warn, carry on. An older binary still starts on a
        // newer config file that way.
        else
            fprintf(stderr, "rc_load: unknown directive '%s' (ignored)\n", key);

        continue;
    badnum:
        fprintf(stderr, "rc_load: '%s' needs a number, got '%s'\n", key, value);
        fclose(fp);
        return -1;
    }

    fclose(fp);

    // Phase 4: check every REQUIRED key actually got set, i.e. its sentinel
    // was overwritten. A missing one is a hard failure.
    int ok = 0;
    if (out->listen_port < 0)      { fprintf(stderr, "rc_load: missing required 'listen_port'\n"); ok = -1; }
    // A TCP port is 16 bits, so it stops at 65535. Let anything bigger through
    // and the (uint16_t) cast tetrisd does later wraps it to the wrong port
    // without saying a word (70000 becomes 4464). Reject it here instead.
    else if (out->listen_port < 1 || out->listen_port > 65535){
        fprintf(stderr, "rc_load: listen_port %d out of range 1-65535\n", out->listen_port);
        ok = -1;
    }
    if (out->cert_path[0] == '\0') { fprintf(stderr, "rc_load: missing required 'cert_path'\n");   ok = -1; }
    if (out->key_path[0]  == '\0') { fprintf(stderr, "rc_load: missing required 'key_path'\n");    ok = -1; }
    if (out->ca_path[0]   == '\0') { fprintf(stderr, "rc_load: missing required 'ca_path'\n");     ok = -1; }
    if (out->log_path[0]  == '\0') { fprintf(stderr, "rc_load: missing required 'log_path'\n");    ok = -1; }
    if (out->log_ipc[0]   == '\0') { fprintf(stderr, "rc_load: missing required 'log_ipc'\n");     ok = -1; }
    if (out->ctl_path[0]  == '\0') { fprintf(stderr, "rc_load: missing required 'ctl_path'\n");    ok = -1; }

    // Phase 5: report the verdict.
    return ok;
}