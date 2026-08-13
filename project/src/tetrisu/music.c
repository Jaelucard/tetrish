// optional background music for tetrisu. synthesizes the public-domain
// korobeiniki folk melody (the tune associated with tetris) from the note
// table below into a temp wav file. the melody was written out by hand
// from the well-known tune; no recording or existing transcription is
// copied. everything here is integer arithmetic, and nothing in this file
// touches the game engine or any deterministic state.

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "music.h"

// note lengths in ms at 150 bpm: quarter = 400
#define EIGHTH  200
#define QUARTER 400
#define DOTTED  600

// pitch frequencies in hz, rounded to integers
#define A4  440
#define B4  494
#define C5  523
#define D5  587
#define E5  659
#define F5  698
#define G5  784
#define A5  880

// korobeiniki a-section: first phrase once, second phrase twice (the
// traditional repeat). the whole track loops via player respawn, not by
// making this table longer. freq 0 is a rest.
const music_note music_melody[] = {
    // phrase one
    { E5, QUARTER }, { B4, EIGHTH },  { C5, EIGHTH },
    { D5, QUARTER }, { C5, EIGHTH },  { B4, EIGHTH },
    { A4, QUARTER }, { A4, EIGHTH },  { C5, EIGHTH },
    { E5, QUARTER }, { D5, EIGHTH },  { C5, EIGHTH },
    { B4, DOTTED },  { C5, EIGHTH },  { D5, QUARTER }, { E5, QUARTER },
    { C5, QUARTER }, { A4, QUARTER }, { A4, QUARTER }, { 0,  QUARTER },
    // phrase two
    { D5, DOTTED },  { F5, EIGHTH },  { A5, QUARTER },
    { G5, EIGHTH },  { F5, EIGHTH },
    { E5, DOTTED },  { C5, EIGHTH },  { E5, QUARTER },
    { D5, EIGHTH },  { C5, EIGHTH },
    { B4, QUARTER }, { B4, EIGHTH },  { C5, EIGHTH },  { D5, QUARTER },
    { E5, QUARTER }, { C5, QUARTER }, { A4, QUARTER }, { A4, QUARTER },
    { 0,  QUARTER },
    // phrase two again
    { D5, DOTTED },  { F5, EIGHTH },  { A5, QUARTER },
    { G5, EIGHTH },  { F5, EIGHTH },
    { E5, DOTTED },  { C5, EIGHTH },  { E5, QUARTER },
    { D5, EIGHTH },  { C5, EIGHTH },
    { B4, QUARTER }, { B4, EIGHTH },  { C5, EIGHTH },  { D5, QUARTER },
    { E5, QUARTER }, { C5, QUARTER }, { A4, QUARTER }, { A4, QUARTER },
    { 0,  QUARTER },
};
const size_t music_melody_len = sizeof music_melody / sizeof music_melody[0];

// pcm samples one note occupies, not counting the inter-note gap
static uint32_t note_samples(uint16_t dur_ms)
{
    return (uint32_t)dur_ms * MUSIC_RATE_HZ / 1000;
}

// silence inserted after every note so repeated pitches articulate
static uint32_t gap_samples(void)
{
    return (uint32_t)MUSIC_GAP_MS * MUSIC_RATE_HZ / 1000;
}

// little-endian field writers for the riff header and the samples
static void put_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)(v >> 8);
}

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

// writes the whole buffer to fd, retrying on short writes
static int write_all(int fd, const unsigned char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

// builds the wav in memory (44-byte riff header plus square-wave samples:
// +amp for the first half of each wave period, -amp for the second, all
// integer division on a running sample counter) and writes it to a fresh
// mkstemp file. returns 0 and the path on success, -1 on any failure.
int music_write_wav(char *path_out, size_t path_cap)
{
    char tmpl[] = "/tmp/tetrisu-XXXXXX";
    if (path_out == NULL || path_cap < sizeof tmpl)
        return -1;

    // total sample count first, so the header sizes are exact
    uint32_t total = 0;
    for (size_t i = 0; i < music_melody_len; i++)
        total += note_samples(music_melody[i].dur_ms) + gap_samples();
    uint32_t data_bytes = total * 2;

    unsigned char *buf = malloc(44 + (size_t)data_bytes);
    if (buf == NULL)
        return -1;

    memcpy(buf + 0, "RIFF", 4);
    put_u32(buf + 4, 36 + data_bytes);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    put_u32(buf + 16, 16);                     // fmt chunk size
    put_u16(buf + 20, 1);                      // pcm
    put_u16(buf + 22, 1);                      // mono
    put_u32(buf + 24, MUSIC_RATE_HZ);
    put_u32(buf + 28, MUSIC_RATE_HZ * 2);      // byte rate
    put_u16(buf + 32, 2);                      // block align
    put_u16(buf + 34, 16);                     // bits per sample
    memcpy(buf + 36, "data", 4);
    put_u32(buf + 40, data_bytes);

    unsigned char *p = buf + 44;
    for (size_t i = 0; i < music_melody_len; i++) {
        uint32_t n = note_samples(music_melody[i].dur_ms);
        if (music_melody[i].freq_hz == 0) {
            memset(p, 0, (size_t)n * 2);
            p += (size_t)n * 2;
        } else {
            uint32_t half = MUSIC_RATE_HZ / (2u * music_melody[i].freq_hz);
            if (half == 0)
                half = 1;
            for (uint32_t s = 0; s < n; s++) {
                int16_t v = ((s / half) % 2 == 0) ? MUSIC_AMP : -MUSIC_AMP;
                put_u16(p, (uint16_t)v);
                p += 2;
            }
        }
        memset(p, 0, (size_t)gap_samples() * 2);
        p += (size_t)gap_samples() * 2;
    }

    int fd = mkstemp(tmpl);
    if (fd < 0) {
        free(buf);
        return -1;
    }
    if (write_all(fd, buf, 44 + (size_t)data_bytes) != 0) {
        close(fd);
        unlink(tmpl);
        free(buf);
        return -1;
    }
    close(fd);
    free(buf);
    memcpy(path_out, tmpl, sizeof tmpl);
    return 0;
}
