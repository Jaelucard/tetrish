// public interface for tetrisu's optional background music: the
// public-domain korobeiniki folk melody, synthesized from a note table
// into a temp wav file and played by an external player subprocess.

#ifndef TETRISU_MUSIC_H
#define TETRISU_MUSIC_H

#include <stddef.h>
#include <stdint.h>

// synthesis parameters, shared with tests/test_music.c so the test can
// recompute the expected file layout from the same numbers
#define MUSIC_RATE_HZ  22050
#define MUSIC_AMP      8000
#define MUSIC_GAP_MS   15

// one melody step: square-wave frequency in hz (0 = rest), duration in ms
typedef struct {
    uint16_t freq_hz;
    uint16_t dur_ms;
} music_note;

// the melody table, exposed so the unit test can derive the expected wav
// duration from the same data the synthesizer uses
extern const music_note music_melody[];
extern const size_t     music_melody_len;

// writes one pass of the melody as a 16-bit mono pcm wav to a fresh
// mkstemp file. returns 0 and copies the path into path_out on success,
// -1 on any failure (nothing is left behind on failure).
int music_write_wav(char *path_out, size_t path_cap);

#endif
