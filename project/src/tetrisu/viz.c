// bar visualizer driven by the score, not by sound: music.c's note table
// says which note sounds at any millisecond, so the "spectrum" is derived
// arithmetically and works with no audio device, no fft, and no capture.
// playback (music.c's player child) and this file are independent
// consumers of the same table. everything is integer arithmetic, all
// state lives in the caller's viz_t, and only viz_draw touches curses.

#include <langinfo.h>
#include <ncurses.h>
#include <string.h>

#include "music.h"
#include "viz.h"

// zeroes the state, scans the melody table once for its playable
// frequency range (rests excluded), and probes the locale codeset to
// decide whether the utf-8 partial-block ramp may be emitted.
void viz_init(viz_t *v)
{
    memset(v, 0, sizeof *v);
    v->enabled = 1;
    v->fmin = 0xffff;
    v->fmax = 0;
    for (size_t i = 0; i < music_melody_len; i++) {
        uint16_t f = music_melody[i].freq_hz;
        if (f == 0)
            continue;
        if (f < v->fmin) v->fmin = f;
        if (f > v->fmax) v->fmax = f;
    }
    if (v->fmin > v->fmax) {          // all-rest table: degrade to flat
        v->fmin = 0;
        v->fmax = 0;
    }
    const char *cs = nl_langinfo(CODESET);
    v->utf8 = (cs != NULL && strcmp(cs, "UTF-8") == 0);
}

// arms the animation clock at the caller's monotonic now.
void viz_begin(viz_t *v, uint32_t now_ms)
{
    v->start_ms = now_ms;
    v->running = 1;
}

// one animation step at elapsed_ms into the looped tune. the sounding
// note's bucket jumps to full height, its two neighbours are raised to
// at least the spill level, and every other bar decays by a fixed step;
// a rest (or a degenerate one-pitch table) decays everything. the
// hostile counter armed by a garbage accent burns down here too.
void viz_step(viz_t *v, uint32_t elapsed_ms)
{
    uint16_t f = 0;
    music_note_at(elapsed_ms, &f);

    int target = -1;
    if (f != 0)
        target = (v->fmax > v->fmin)
               ? (int)((uint32_t)(f - v->fmin) * (VIZ_BARS - 1)
                       / (uint32_t)(v->fmax - v->fmin))
               : 0;

    for (int i = 0; i < VIZ_BARS; i++) {
        if (i == target) {
            v->bars[i] = VIZ_MAX_EIGHTHS;
        } else if (target >= 0 && (i == target - 1 || i == target + 1)) {
            if (v->bars[i] < VIZ_SPILL_EIGHTHS)
                v->bars[i] = VIZ_SPILL_EIGHTHS;
        } else {
            v->bars[i] -= VIZ_DECAY_EIGHTHS;
            if (v->bars[i] < 0)
                v->bars[i] = 0;
        }
    }

    if (v->hostile > 0)
        v->hostile--;
}

// game-event accents. CLEAR is celebratory: every bar to full. GARBAGE
// is hostile: alternating bars to full plus the tint counter, so the
// renderer switches the gradient to all-red until it decays.
void viz_accent(viz_t *v, int kind)
{
    if (kind == VIZ_ACCENT_CLEAR) {
        for (int i = 0; i < VIZ_BARS; i++)
            v->bars[i] = VIZ_MAX_EIGHTHS;
    } else if (kind == VIZ_ACCENT_GARBAGE) {
        for (int i = 0; i < VIZ_BARS; i += 2)
            v->bars[i] = VIZ_MAX_EIGHTHS;
        v->hostile = VIZ_HOSTILE_STEPS;
    }
}

// colour pair for one strip row counted from the bottom, following the
// pair table main.c initialises (5 green, 4 yellow, 7 red): a bottom-
// green to top-red gradient, all red while the hostile tint is live.
static int row_pair(const viz_t *v, int row_from_bottom)
{
    if (v->hostile > 0)
        return 7;
    if (row_from_bottom >= VIZ_STRIP_ROWS - 1)
        return 7;
    if (row_from_bottom >= VIZ_STRIP_ROWS / 2)
        return 4;
    return 5;
}

// draws the strip, top edge at screen row top, from the boards' left
// margin. each bar is two columns wide with a one-column gap. heights
// are in eighths of a row: on a utf-8 locale the u+2581..u+2588 ramp
// renders the fractional cell; otherwise full cells are reverse-video
// spaces and a remainder of half a cell or more is a dim one, the same
// no-colour idiom the board renderer uses. blank cells are not drawn:
// the callers erase() the whole screen every frame. colour is applied
// only when a pair table is actually initialised (COLOR_PAIRS stays 0
// before start_color, which the --local client never calls).
void viz_draw(const viz_t *v, int top, int width)
{
    // eight partial blocks, one per eighth of a row: u+2581 .. u+2588
    static const char *ramp[8] = {
        "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
    };
    const int left = 2;               // matches the boards' BOARD_LEFT
    int colors = has_colors() && COLOR_PAIRS > 8;

    int nbars = (width - left) / 3;
    if (nbars > VIZ_BARS) nbars = VIZ_BARS;
    if (nbars < 1) return;

    for (int i = 0; i < nbars; i++) {
        int x = left + i * 3;
        int full = v->bars[i] / 8;    // whole rows lit from the bottom
        int rem  = v->bars[i] % 8;    // eighths in the partial row above
        for (int rb = 0; rb < VIZ_STRIP_ROWS; rb++) {
            int y = top + VIZ_STRIP_ROWS - 1 - rb;
            int pair = row_pair(v, rb);
            if (colors) attron(COLOR_PAIR(pair));
            if (rb < full) {
                if (v->utf8) mvprintw(y, x, "%s%s", ramp[7], ramp[7]);
                else { attron(A_REVERSE); mvprintw(y, x, "  "); attroff(A_REVERSE); }
            } else if (rb == full && rem > 0) {
                if (v->utf8) mvprintw(y, x, "%s%s", ramp[rem - 1], ramp[rem - 1]);
                else if (rem >= 4) {
                    attron(A_REVERSE | A_DIM);
                    mvprintw(y, x, "  ");
                    attroff(A_REVERSE | A_DIM);
                }
            }
            if (colors) attroff(COLOR_PAIR(pair));
        }
    }
}
