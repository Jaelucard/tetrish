// tetrish-view's entry point: dispatches between its two faces.
//
//   tetrish-view                                the admin console (repl.c):
//                                                a repl against tetrisd's
//                                                control plane.
//   tetrish-view [--verify] <log-file> [room] [player]
//                                                the replay viewer
//                                                (replay.c): reconstructs a
//                                                recorded session by
//                                                re-driving tb_tick, the
//                                                same entry point tetrisd
//                                                uses. See replay.c's header
//                                                comment for why that makes
//                                                replay trustworthy.
//                                                --verify runs it headless
//                                                and prints a pass/fail
//                                                report, for scripts/tests.
//
// The repl also has its own `replay <logfile> [room] [player]` command
// (repl.c), which calls the same replay_run() as this CLI path -- one
// implementation either way.
//
// keys (interactive replay only): space pause, left/right seek, +/- speed, q quit

#include <stdio.h>
#include <string.h>
#include "repl.h"
#include "replay.h"

// The default control-plane config, same as tetrisctl's own default.
#define DEFAULT_RC_PATH ".tetrishrc"

int main(int argc, char **argv){
    // No arguments at all: the admin console, not the replay viewer. Argument
    // parsing for the viewer only starts once we know we're in that mode, so
    // this has to come first.
    if (argc < 2)
        return repl_run(DEFAULT_RC_PATH);

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0){
        fprintf(stderr,
                "usage: %s\n"
                "         admin console: connects to tetrisd's control plane\n"
                "       %s [--verify] <log-file> [room] [player]\n"
                "         replays a recorded session from tetrislogd's log file\n"
                "         --verify  reconstruct without ncurses and print the\n"
                "                   result, so replay can be checked from a\n"
                "                   script or a test\n",
                argv[0], argv[0]);
        return 2;
    }

    // --verify runs the whole reconstruction headless. It exists because the
    // interactive viewer needs a terminal, which means it cannot be exercised
    // by an automated test, and an unverifiable replay is not worth much. This
    // mode is what proves the recorded format actually contains enough to
    // rebuild a game.
    int verify = 0, argi = 1;
    if (strcmp(argv[argi], "--verify") == 0){ verify = 1; argi++; }
    if (argi >= argc){ fprintf(stderr, "tetrish-view: no log file given\n"); return 2; }

    const char *logpath     = argv[argi];
    const char *want_room   = (argi + 1 < argc) ? argv[argi + 1] : "";
    const char *want_player = (argi + 2 < argc) ? argv[argi + 2] : "";

    return replay_run(logpath, want_room, want_player, verify);
}
