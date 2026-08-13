// optional background music for tetrisu. synthesizes the public-domain
// korobeiniki folk melody (the tune associated with tetris) from the note
// tables below into a temp wav file. the melody and the root-fifth bass
// accompaniment were written out by hand from the well-known folk tune
// and its traditional a-minor harmonization; no recording, midi file, or
// existing arrangement is copied. the game-boy-era sound is imitated only
// in character (two pulse voices, 25% and 50% duty). everything here is
// integer arithmetic, and nothing in this file touches the game engine
// or any deterministic state.

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "music.h"

// note lengths in ms at 150 bpm: quarter = 400
#define EIGHTH  200
#define QUARTER 400
#define DOTTED  600

// pitch frequencies in hz, rounded to integers
#define E2   82
#define A2  110
#define B2  123
#define D3  147
#define E3  165
#define A4  440
#define B4  494
#define C5  523
#define D5  587
#define E5  659
#define F5  698
#define G5  784
#define A5  880

// melody: the korobeiniki a-section only (first phrase once, second
// phrase twice, the traditional repeat). one 19-second pass; the track
// repeats via player respawn, not by making these tables longer. freq 0
// is a rest.
const music_note music_melody[] = {
    // a-section, phrase one
    { E5, QUARTER }, { B4, EIGHTH },  { C5, EIGHTH },
    { D5, QUARTER }, { C5, EIGHTH },  { B4, EIGHTH },
    { A4, QUARTER }, { A4, EIGHTH },  { C5, EIGHTH },
    { E5, QUARTER }, { D5, EIGHTH },  { C5, EIGHTH },
    { B4, DOTTED },  { C5, EIGHTH },  { D5, QUARTER }, { E5, QUARTER },
    { C5, QUARTER }, { A4, QUARTER }, { A4, QUARTER }, { 0,  QUARTER },
    // a-section, phrase two
    { D5, DOTTED },  { F5, EIGHTH },  { A5, QUARTER },
    { G5, EIGHTH },  { F5, EIGHTH },
    { E5, DOTTED },  { C5, EIGHTH },  { E5, QUARTER },
    { D5, EIGHTH },  { C5, EIGHTH },
    { B4, QUARTER }, { B4, EIGHTH },  { C5, EIGHTH },  { D5, QUARTER },
    { E5, QUARTER }, { C5, QUARTER }, { A4, QUARTER }, { A4, QUARTER },
    { 0,  QUARTER },
    // a-section, phrase two again
    { D5, DOTTED },  { F5, EIGHTH },  { A5, QUARTER },
    { G5, EIGHTH },  { F5, EIGHTH },
    { E5, DOTTED },  { C5, EIGHTH },  { E5, QUARTER },
    { D5, EIGHTH },  { C5, EIGHTH },
    { B4, QUARTER }, { B4, EIGHTH },  { C5, EIGHTH },  { D5, QUARTER },
    { E5, QUARTER }, { C5, QUARTER }, { A4, QUARTER }, { A4, QUARTER },
    { 0,  QUARTER },
};
const size_t music_melody_len = sizeof music_melody / sizeof music_melody[0];

// bass: root-fifth eighth-note pulses under the a-section (the standard
// am / e / dm harmonization of the folk tune). sums to the same
// 19200 ms as the melody table.
const music_note music_bass[] = {
    // a-section, phrase one: am / e / am / am / e / e / am / am-e
    { A2, EIGHTH }, { E3, EIGHTH }, { A2, EIGHTH }, { E3, EIGHTH },
    { E2, EIGHTH }, { B2, EIGHTH }, { E2, EIGHTH }, { B2, EIGHTH },
    { A2, EIGHTH }, { E3, EIGHTH }, { A2, EIGHTH }, { E3, EIGHTH },
    { A2, EIGHTH }, { E3, EIGHTH }, { A2, EIGHTH }, { E3, EIGHTH },
    { E2, EIGHTH }, { B2, EIGHTH }, { E2, EIGHTH }, { B2, EIGHTH },
    { E2, EIGHTH }, { B2, EIGHTH }, { E2, EIGHTH }, { B2, EIGHTH },
    { A2, EIGHTH }, { E3, EIGHTH }, { A2, EIGHTH }, { E3, EIGHTH },
    { A2, EIGHTH }, { E3, EIGHTH }, { E2, EIGHTH }, { B2, EIGHTH },
    // a-section, phrase two: dm / dm / am / am / e / e / am / am-e
    { D3, EIGHTH }, { A2, EIGHTH }, { D3, EIGHTH }, { A2, EIGHTH },
    { D3, EIGHTH }, { A2, EIGHTH }, { D3, EIGHTH }, { A2, EIGHTH },
    { A2, EIGHTH }, { E3, EIGHTH }, { A2, EIGHTH }, { E3, EIGHTH },
    { A2, EIGHTH }, { E3, EIGHTH }, { A2, EIGHTH }, { E3, EIGHTH },
    { E2, EIGHTH }, { B2, EIGHTH }, { E2, EIGHTH }, { B2, EIGHTH },
    { E2, EIGHTH }, { B2, EIGHTH }, { E2, EIGHTH }, { B2, EIGHTH },
    { A2, EIGHTH }, { E3, EIGHTH }, { A2, EIGHTH }, { E3, EIGHTH },
    { A2, EIGHTH }, { E3, EIGHTH }, { E2, EIGHTH }, { B2, EIGHTH },
    // a-section, phrase two again
    { D3, EIGHTH }, { A2, EIGHTH }, { D3, EIGHTH }, { A2, EIGHTH },
    { D3, EIGHTH }, { A2, EIGHTH }, { D3, EIGHTH }, { A2, EIGHTH },
    { A2, EIGHTH }, { E3, EIGHTH }, { A2, EIGHTH }, { E3, EIGHTH },
    { A2, EIGHTH }, { E3, EIGHTH }, { A2, EIGHTH }, { E3, EIGHTH },
    { E2, EIGHTH }, { B2, EIGHTH }, { E2, EIGHTH }, { B2, EIGHTH },
    { E2, EIGHTH }, { B2, EIGHTH }, { E2, EIGHTH }, { B2, EIGHTH },
    { A2, EIGHTH }, { E3, EIGHTH }, { A2, EIGHTH }, { E3, EIGHTH },
    { A2, EIGHTH }, { E3, EIGHTH }, { E2, EIGHTH }, { B2, EIGHTH },
};
const size_t music_bass_len = sizeof music_bass / sizeof music_bass[0];

// pcm samples a duration in ms occupies (all table durations are
// multiples of 200 ms, so this divides exactly at 22050 hz)
static uint32_t ms_samples(uint32_t ms)
{
    return ms * MUSIC_RATE_HZ / 1000;
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

// renders one voice additively into the mix buffer: a square wave whose
// high portion is period/duty_div samples, shaped by a linear attack and
// release, with gap_ms of silence carved out of each note's tail so
// repeated pitches articulate. all integer division on a running sample
// counter; rests just advance the offset.
static void render_voice(int32_t *mix, uint32_t mix_len,
                         const music_note *t, size_t tlen,
                         int32_t amp, uint32_t duty_div, uint32_t gap_ms)
{
    uint32_t off = 0;
    for (size_t i = 0; i < tlen && off < mix_len; i++) {
        uint32_t n = ms_samples(t[i].dur_ms);
        if (t[i].freq_hz != 0) {
            uint32_t gap = ms_samples(gap_ms);
            uint32_t audible = (n > gap) ? n - gap : 0;
            uint32_t period = MUSIC_RATE_HZ / t[i].freq_hz;
            if (period < 2)
                period = 2;
            uint32_t hi = period / duty_div;
            if (hi == 0)
                hi = 1;
            uint32_t attack  = ms_samples(MUSIC_ATTACK_MS);
            uint32_t release = ms_samples(MUSIC_RELEASE_MS);
            if (attack + release > audible) {
                attack  = audible / 4;
                release = audible / 4;
            }
            for (uint32_t s = 0; s < audible && off + s < mix_len; s++) {
                int32_t a = amp;
                if (attack > 0 && s < attack)
                    a = amp * (int32_t)s / (int32_t)attack;
                else if (release > 0 && audible - s <= release)
                    a = amp * (int32_t)(audible - s) / (int32_t)release;
                mix[off + s] += ((s % period) < hi) ? a : -a;
            }
        }
        off += n;
    }
}

// builds the wav in memory (44-byte riff header plus the two mixed
// square voices) and writes it to a fresh mkstemp file. returns 0 and
// the path on success, -1 on any failure.
int music_write_wav(char *path_out, size_t path_cap)
{
    char tmpl[] = "/tmp/tetrisu-XXXXXX";
    if (path_out == NULL || path_cap < sizeof tmpl)
        return -1;

    // total sample count from the melody table; the bass table sums to
    // the same duration (the unit test holds both tables to that)
    uint32_t total = 0;
    for (size_t i = 0; i < music_melody_len; i++)
        total += ms_samples(music_melody[i].dur_ms);
    uint32_t data_bytes = total * 2;

    int32_t *mix = calloc(total, sizeof *mix);
    if (mix == NULL)
        return -1;
    render_voice(mix, total, music_melody, music_melody_len,
                 MUSIC_AMP_MELODY, 4, MUSIC_GAP_MS);
    render_voice(mix, total, music_bass, music_bass_len,
                 MUSIC_AMP_BASS, 2, 0);

    unsigned char *buf = malloc(44 + (size_t)data_bytes);
    if (buf == NULL) {
        free(mix);
        return -1;
    }

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

    // serialize the mix; the two amplitudes cannot overflow int16, the
    // clamp is belt and braces
    for (uint32_t s = 0; s < total; s++) {
        int32_t v = mix[s];
        if (v > 32767)
            v = 32767;
        if (v < -32768)
            v = -32768;
        put_u16(buf + 44 + (size_t)s * 2, (uint16_t)(int16_t)v);
    }
    free(mix);

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

// forks and execs a player over the wav, trying aplay, afplay, paplay in
// order (each execvp only returns on failure). the child gets /dev/null
// on all three stdio fds so player chatter never reaches the terminal,
// and unblocks the signal mask it inherited so sigterm can stop it.
// records the pid on success.
static int spawn_player(music_t *m)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
            if (nul > 2)
                close(nul);
        }
        sigset_t all;
        sigfillset(&all);
        sigprocmask(SIG_UNBLOCK, &all, NULL);
        char *aplay[]  = { "aplay", "-q", m->wav_path, NULL };
        execvp(aplay[0], aplay);
        char *afplay[] = { "afplay", m->wav_path, NULL };
        execvp(afplay[0], afplay);
        char *paplay[] = { "paplay", m->wav_path, NULL };
        execvp(paplay[0], paplay);
        _exit(127);
    }
    m->pid = pid;
    m->spawned_at = (long)time(NULL);
    return 0;
}

// kills the current child if any and reaps it with a bounded
// non-blocking wait: sigterm, up to ~100 ms of wnohang polls, then
// sigkill as a last resort. never calls waitpid without wnohang.
static void reap_child(music_t *m)
{
    if (m->pid <= 0)
        return;
    kill(m->pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        if (waitpid(m->pid, NULL, WNOHANG) != 0) {
            m->pid = 0;
            return;
        }
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    kill(m->pid, SIGKILL);
    for (int i = 0; i < 20; i++) {
        if (waitpid(m->pid, NULL, WNOHANG) != 0)
            break;
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    m->pid = 0;
}

// generates the wav and spawns the first player. every failure mode
// degrades to silence: one line to stderr (this runs before curses),
// state left disabled, and the caller ignores the return value freely.
int music_start(music_t *m)
{
    memset(m, 0, sizeof *m);
    if (music_write_wav(m->wav_path, sizeof m->wav_path) != 0) {
        fprintf(stderr, "tetrisu: music disabled (could not write wav)\n");
        return -1;
    }
    if (spawn_player(m) != 0) {
        unlink(m->wav_path);
        m->wav_path[0] = '\0';
        fprintf(stderr, "tetrisu: music disabled (fork failed)\n");
        return -1;
    }
    m->enabled = 1;
    return 0;
}

// called when the owner's event loop sees sigchld. reaps the player
// without blocking and respawns it so the tune repeats. a child that
// exited 127 (no player installed) or died within 2 seconds (player
// present but broken, e.g. no audio device) would respawn forever, so
// that disables music for the rest of the session, silently.
void music_on_sigchld(music_t *m)
{
    if (m->pid <= 0)
        return;
    int st;
    if (waitpid(m->pid, &st, WNOHANG) != m->pid)
        return;
    m->pid = 0;
    if (!m->enabled || m->muted)
        return;
    if ((WIFEXITED(st) && WEXITSTATUS(st) == 127) ||
        (long)time(NULL) - m->spawned_at < 2) {
        m->enabled = 0;
        unlink(m->wav_path);
        m->wav_path[0] = '\0';
        return;
    }
    if (spawn_player(m) != 0)
        m->enabled = 0;
}

// mute/unmute. mute kills the player but keeps the wav so unmute is
// just a respawn.
void music_toggle(music_t *m)
{
    if (!m->enabled)
        return;
    if (m->muted) {
        m->muted = 0;
        if (m->pid <= 0 && spawn_player(m) != 0)
            m->enabled = 0;
    } else {
        m->muted = 1;
        reap_child(m);
    }
}

// full teardown: child gone, wav unlinked, state disabled. idempotent,
// and safe on a zeroed struct, so every exit path may call it.
void music_stop(music_t *m)
{
    reap_child(m);
    if (m->wav_path[0] != '\0') {
        unlink(m->wav_path);
        m->wav_path[0] = '\0';
    }
    m->enabled = 0;
    m->muted = 0;
}
