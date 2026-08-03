// Pure parsing/lookup logic for the repl, split out from repl.c so it has
// zero dependency on control.c (and therefore libhtttp): these functions do
// no I/O at all, which is exactly what makes them unit-testable without a
// socket (see tests/test_view_repl.c).

#include <string.h>

#include "repl.h"

// Command names repl.c's real dispatch table (COMMANDS[], in repl.c) knows
// how to run. Duplicated here rather than shared via a common header, on
// purpose: keeping this file free of any control.c/libhtttp dependency is
// the whole point of the split. Keep this list in sync with repl.c's
// COMMANDS table; a mismatch fails safe (a command repl_run could actually
// run would just be reported as unknown here, not crash).
static const char *const KNOWN_COMMANDS[] = { "status", "rooms", "players", "kick", "attach", "replay" };
#define NKNOWN (int)(sizeof(KNOWN_COMMANDS) / sizeof(KNOWN_COMMANDS[0]))

int repl_is_known_command(const char *name){
    for (int i = 0; i < NKNOWN; i++)
        if (strcmp(name, KNOWN_COMMANDS[i]) == 0) return 1;
    return 0;
}

// splits "name args..." in place at the first whitespace run: returns the
// command name and points *args at the rest (empty string if none). full
// contract in repl.h; exposed there so tests can drive it directly.
char *repl_split_line(char *line, char **args){
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    char *name = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p){
        *p = '\0';
        p++;
        while (*p == ' ' || *p == '\t') p++;
    }
    *args = p;
    return name;
}
