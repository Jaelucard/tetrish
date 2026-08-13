// public interface for tetrisu's optional background music: the
// public-domain korobeiniki folk melody, synthesized from note tables
// into a temp wav file and played by an external player subprocess.

#ifndef TETRISU_MUSIC_H
#define TETRISU_MUSIC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// synthesis parameters, shared with tests/test_music.c so the test can
// recompute the expected file layout from the same numbers. two square
// voices are mixed: a 25%-duty melody and a 50%-duty bass, each shaped
// by a short linear attack/release envelope so notes don't click.
#define MUSIC_RATE_HZ    22050
#define MUSIC_AMP_MELODY 6000
#define MUSIC_AMP_BASS   4500
#define MUSIC_GAP_MS     15
#define MUSIC_ATTACK_MS  5
#define MUSIC_RELEASE_MS 20

// one melody step: square-wave frequency in hz (0 = rest), duration in ms
typedef struct {
    uint16_t freq_hz;
    uint16_t dur_ms;
} music_note;

// the two voice tables, exposed so the unit test can derive the expected
// wav duration from the same data the synthesizer uses. both tables sum
// to the same total duration.
extern const music_note music_melody[];
extern const size_t     music_melody_len;
extern const music_note music_bass[];
extern const size_t     music_bass_len;

// writes one pass of the tune as a 16-bit mono pcm wav to a fresh
// mkstemp file. returns 0 and copies the path into path_out on success,
// -1 on any failure (nothing is left behind on failure).
int music_write_wav(char *path_out, size_t path_cap);

// playback state. playing is respawn-on-exit: the player child runs one
// pass of the wav and exits, and the owner's sigchld handling respawns
// it, which is what makes the tune repeat.
typedef struct {
    int    enabled;      // wav written and playback still possible
    int    muted;        // player deliberately stopped, wav kept
    pid_t  pid;          // player child, 0 when none is running
    long   spawned_at;   // seconds clock at last spawn, respawn-storm guard
    char   wav_path[64];
} music_t;

// generates the wav and spawns the first player child. returns 0 on
// success; on any failure prints at most one line to stderr, leaves m
// disabled, and returns -1. the game must run identically either way.
int music_start(music_t *m);

// reaps the player child after sigchld and respawns it unless the music
// is muted or the player is evidently broken (execvp found nothing, or
// the child dies immediately). never blocks.
void music_on_sigchld(music_t *m);

// mute stops the child but keeps the wav; unmute respawns the player.
// silently does nothing when music is disabled.
void music_toggle(music_t *m);

// kills and reaps any child (bounded, non-blocking wait), unlinks the
// temp wav, and disables the state. safe to call repeatedly and safe on
// a zeroed music_t.
void music_stop(music_t *m);

#endif
