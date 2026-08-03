// Replays a recorded tetrislogd session. See main.c's header comment for
// the full rationale (replay is just re-driving tb_tick, the same entry
// point tetrisd uses, so it cannot drift from real play). Split out of
// main.c so the repl's `replay <logfile>` command (repl.c) and the
// original CLI usage (`tetrish-view [--verify] <log-file> ...`) share one
// implementation.

#ifndef TETRISH_VIEW_REPLAY_H
#define TETRISH_VIEW_REPLAY_H

// Reads logpath, reconstructs the session matching want_room/want_player
// ("" means "the first session found") by re-driving tb_tick, and either
// verifies it headlessly (verify != 0, prints a report, no terminal needed)
// or shows it in an interactive ncurses viewer (space pause, left/right
// seek, +/- speed, q quit). Returns a process exit code: 0 on success
// (verify: reconstruction matched every recorded snapshot), 1 on a
// load/verify failure, 2 if the interactive viewer was requested but stdout
// is not a terminal.
int replay_run(const char *logpath, const char *want_room,
                const char *want_player, int verify);

#endif
