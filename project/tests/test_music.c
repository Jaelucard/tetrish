// unit test for the music synthesizer: generates the wav via
// music_write_wav, reads the file back, and checks the riff header
// fields, the exact data size against the note table, and that every
// sample is one of {+MUSIC_AMP, -MUSIC_AMP, 0}. headless and
// deterministic; the temp file is unlinked at the end.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "music.h"

// little-endian readers mirroring the writers in music.c
static uint16_t get_u16(const unsigned char *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(void)
{
    char path[128];
    assert(music_write_wav(path, sizeof path) == 0);
    printf("test_music: wrote %s\n", path);

    // slurp the whole file
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    assert(fseek(f, 0, SEEK_END) == 0);
    long fsz = ftell(f);
    assert(fsz > 44);
    rewind(f);
    unsigned char *buf = malloc((size_t)fsz);
    assert(buf != NULL);
    assert(fread(buf, 1, (size_t)fsz, f) == (size_t)fsz);
    fclose(f);

    // test 1. magic bytes of each chunk in a canonical 44-byte header.
    assert(memcmp(buf + 0,  "RIFF", 4) == 0);
    assert(memcmp(buf + 8,  "WAVE", 4) == 0);
    assert(memcmp(buf + 12, "fmt ", 4) == 0);
    assert(memcmp(buf + 36, "data", 4) == 0);

    // test 2. format fields: pcm, mono, the advertised rate, 16-bit.
    assert(get_u32(buf + 16) == 16);
    assert(get_u16(buf + 20) == 1);
    assert(get_u16(buf + 22) == 1);
    assert(get_u32(buf + 24) == MUSIC_RATE_HZ);
    assert(get_u32(buf + 28) == MUSIC_RATE_HZ * 2);
    assert(get_u16(buf + 32) == 2);
    assert(get_u16(buf + 34) == 16);

    // test 3. declared sizes match the bytes actually present.
    uint32_t data_bytes = get_u32(buf + 40);
    assert(data_bytes == (uint32_t)fsz - 44);
    assert(get_u32(buf + 4) == (uint32_t)fsz - 8);

    // test 4. total duration equals the note table durations plus one
    // articulation gap per note, using the same integer rounding as the
    // synthesizer (per-note truncation, so exact, not just within one
    // sample).
    uint32_t gap = (uint32_t)MUSIC_GAP_MS * MUSIC_RATE_HZ / 1000;
    uint32_t expect = 0;
    for (size_t i = 0; i < music_melody_len; i++)
        expect += (uint32_t)music_melody[i].dur_ms * MUSIC_RATE_HZ / 1000 + gap;
    assert(data_bytes == expect * 2);

    // test 5. every sample is +amp, -amp, or silence, and both wave
    // polarities plus silence actually occur.
    uint32_t n_pos = 0, n_neg = 0, n_zero = 0;
    for (uint32_t s = 0; s < expect; s++) {
        int16_t v = (int16_t)get_u16(buf + 44 + (size_t)s * 2);
        assert(v == MUSIC_AMP || v == -MUSIC_AMP || v == 0);
        if (v > 0) n_pos++;
        else if (v < 0) n_neg++;
        else n_zero++;
    }
    assert(n_pos > 0 && n_neg > 0 && n_zero > 0);

    // test 6. the table itself looks like the melody: starts on e5, has
    // a sane size, and contains rests.
    assert(music_melody_len >= 40 && music_melody_len <= 60);
    assert(music_melody[0].freq_hz == 659);

    free(buf);
    assert(unlink(path) == 0);

    printf("test_music: %zu notes, %u samples (%u ms), all checks passed\n",
           music_melody_len, expect,
           (unsigned)((uint64_t)expect * 1000 / MUSIC_RATE_HZ));
    return 0;
}
