// cava-style bar visualizer for tetrisu, driven by the korobeiniki note
// table in music.c rather than by captured audio. the pure state machine
// (viz_init/viz_step/viz_accent) is testable headless; only viz_draw
// touches curses. nothing here reads a clock, touches the game engine,
// or performs i/o beyond drawing.

#ifndef TETRISU_VIZ_H
#define TETRISU_VIZ_H

#include <stdint.h>

// strip geometry: 14 frequency buckets, 4 terminal rows tall, heights
// held in eighths of a row so the unicode partial-block ramp can render
// them at sub-row resolution.
#define VIZ_BARS         14
#define VIZ_STRIP_ROWS    4
#define VIZ_MAX_EIGHTHS  (VIZ_STRIP_ROWS * 8)

// animation tuning, all integer: the raise spilling onto the sounding
// bar's neighbours, the per-step decay of untouched bars, and how many
// steps the hostile (garbage) tint survives.
#define VIZ_SPILL_EIGHTHS   20
#define VIZ_DECAY_EIGHTHS    2
#define VIZ_HOSTILE_STEPS   30

// game-event accent kinds for viz_accent
#define VIZ_ACCENT_CLEAR    0
#define VIZ_ACCENT_GARBAGE  1

// all visualizer state, owned by the caller: no globals in viz.c.
typedef struct {
    int      bars[VIZ_BARS];  // heights in eighths, 0..VIZ_MAX_EIGHTHS
    uint32_t start_ms;        // caller's monotonic base, set by viz_begin
    int      running;         // clock armed
    int      hostile;         // steps left of hostile tint after garbage
    int      enabled;         // the 'v' toggle, on by default
    uint16_t fmin, fmax;      // melody frequency range, from the table
    int      utf8;            // partial-block ramp is safe to emit
} viz_t;

// zeroes the state, computes the melody's frequency range from the note
// table, and probes the locale for utf-8 safety. call once, after
// setlocale but before the first step or draw.
void viz_init(viz_t *v);

// arms the animation clock: the caller records its monotonic now here
// and passes now - start_ms as elapsed_ms to viz_step from then on.
void viz_begin(viz_t *v, uint32_t now_ms);

// advances the animation to elapsed_ms into the looped tune: the
// sounding note's bar rises to full with a smaller spill onto its two
// neighbours, every other bar decays by a fixed step, and a rest decays
// everything. deterministic given the struct and the argument.
void viz_step(viz_t *v, uint32_t elapsed_ms);

// game-event accents: CLEAR spikes every bar to full; GARBAGE spikes
// alternating bars and arms the short-lived hostile tint.
void viz_accent(viz_t *v, int kind);

// draws the strip with its top edge at screen row top, laid out from
// the boards' left margin across at most width columns. the caller is
// responsible for checking the strip fits the terminal; every drawing
// degradation (no colour, no utf-8) is handled inside.
void viz_draw(const viz_t *v, int top, int width);

#endif
