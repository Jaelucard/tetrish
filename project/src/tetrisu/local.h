// declares the --local entry point: a full playable game against an
// in-process tb_game, no sockets, no server.

#ifndef TETRISU_LOCAL_H
#define TETRISU_LOCAL_H

#include <stdint.h>

// runs the local game loop until the player quits. seed 0 picks a fresh seed
// from the clock; start_level 0 leaves the engine at its default (level 1).
int tetrisu_local_run(uint32_t seed, uint32_t start_level);

#endif
