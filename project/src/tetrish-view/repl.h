// The ops console's interactive admin repl: prompt, read a line, dispatch
// it to a command, repeat. See DECISIONS.md's repl design entry for why it
// talks to tetrisd over the control plane the way it does.

#ifndef TETRISH_VIEW_REPL_H
#define TETRISH_VIEW_REPL_H

// Runs until the user types "quit"/"exit" or stdin hits EOF. rc_path is the
// config file to load for tetrisd's control-socket path. Returns a process
// exit code (0 on a clean quit, 1 if the config failed to load).
int repl_run(const char *rc_path);

// Splits one input line in place at the first run of whitespace: returns a
// pointer to the command name (leading whitespace skipped, NUL-terminated
// where the split happened) and sets *args to the remaining text with its
// own leading whitespace skipped (never NULL -- an empty string if there
// were no arguments). Does not touch a trailing newline; callers reading
// from a stream strip that first. Exposed (rather than kept static) so
// tests/test_view_repl.c can exercise it without going through stdin.
char *repl_split_line(char *line, char **args);

// True if `name` is a command repl_run's dispatch table knows how to run
// (status, rooms, players, kick, attach, replay). "help", "?", "quit" and
// "exit" are handled by repl_run directly and are not part of this table,
// so they are not "known" by this function's definition. Exposed for the
// same reason as repl_split_line.
int repl_is_known_command(const char *name);

#endif
