// unit test for the visualizer's pure core: music_note_at boundary
// behaviour against prefix sums recomputed here from the same table, and
// viz_step/viz_accent state transitions (bucket mapping, neighbour
// spill at both array edges, monotonic decay, accent saturation and the
// hostile tint burn-down). headless and deterministic: driven entirely
// by synthetic elapsed_ms values, no clocks and no curses calls.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "music.h"
#include "viz.h"

// the frequency-to-bucket mapping viz_step promises, recomputed here so
// the test fails if the two ever drift apart
static int expect_bucket(uint16_t f, uint16_t fmin, uint16_t fmax)
{
    return (int)((uint32_t)(f - fmin) * (VIZ_BARS - 1)
                 / (uint32_t)(fmax - fmin));
}

int main(void)
{
    // recompute the melody prefix sums from the exported table
    uint32_t prefix[256];
    assert(music_melody_len < 256);
    uint32_t acc = 0;
    for (size_t i = 0; i < music_melody_len; i++) {
        prefix[i] = acc;
        acc += music_melody[i].dur_ms;
    }
    uint32_t total = acc;
    assert(total > 0);

    // test 1. note boundaries: first ms and last ms of a note resolve to
    // that note, the exact boundary ms belongs to the next note, and
    // elapsed values past one pass wrap around.
    uint16_t f = 0xffff;
    assert(music_note_at(0, &f) == 0);
    assert(f == music_melody[0].freq_hz);
    assert(music_note_at(prefix[1] - 1, &f) == 0);
    assert(f == music_melody[0].freq_hz);
    assert(music_note_at(prefix[1], &f) == 1);
    assert(f == music_melody[1].freq_hz);
    assert(music_note_at(total, &f) == 0);            // exact wrap = start
    assert(music_note_at(total + prefix[1], NULL) == 1);
    assert(music_note_at(3 * total + 1, &f) == music_note_at(1, NULL));
    size_t last = music_melody_len - 1;
    assert((size_t)music_note_at(total - 1, &f) == last);
    assert(f == music_melody[last].freq_hz);

    // test 2. a rest reports frequency 0
    size_t i_rest = 0;
    while (i_rest < music_melody_len && music_melody[i_rest].freq_hz != 0)
        i_rest++;
    assert(i_rest < music_melody_len);
    assert(music_note_at(prefix[i_rest], &f) == (int)i_rest);
    assert(f == 0);

    // find notes at the extremes of the range for the edge-spill tests
    viz_t v;
    viz_init(&v);
    assert(v.fmin > 0 && v.fmax > v.fmin);
    size_t i_min = 0, i_max = 0;
    for (size_t i = 0; i < music_melody_len; i++) {
        if (music_melody[i].freq_hz == v.fmin) i_min = i;
        if (music_melody[i].freq_hz == v.fmax) i_max = i;
    }
    assert(music_melody[i_min].freq_hz == v.fmin);
    assert(music_melody[i_max].freq_hz == v.fmax);
    assert(expect_bucket(v.fmin, v.fmin, v.fmax) == 0);
    assert(expect_bucket(v.fmax, v.fmin, v.fmax) == VIZ_BARS - 1);

    // test 3. stepping on the lowest note fills bucket 0, spills only
    // onto bucket 1 (no wrap to the far edge), and leaves the rest at 0.
    viz_init(&v);
    viz_step(&v, prefix[i_min]);
    assert(v.bars[0] == VIZ_MAX_EIGHTHS);
    assert(v.bars[1] == VIZ_SPILL_EIGHTHS);
    for (int i = 2; i < VIZ_BARS; i++)
        assert(v.bars[i] == 0);

    // test 4. the highest note fills the last bucket, spills only onto
    // its lower neighbour, and decays the previously-full bucket 0.
    viz_step(&v, prefix[i_max]);
    assert(v.bars[VIZ_BARS - 1] == VIZ_MAX_EIGHTHS);
    assert(v.bars[VIZ_BARS - 2] == VIZ_SPILL_EIGHTHS);
    assert(v.bars[0] == VIZ_MAX_EIGHTHS - VIZ_DECAY_EIGHTHS);

    // test 5. an interior note lands on its computed bucket with spill
    // on both sides.
    size_t i_mid = 0;
    int mid_bucket = -1;
    for (size_t i = 0; i < music_melody_len; i++) {
        uint16_t fq = music_melody[i].freq_hz;
        if (fq == 0) continue;
        int b = expect_bucket(fq, v.fmin, v.fmax);
        if (b > 0 && b < VIZ_BARS - 1) { i_mid = i; mid_bucket = b; break; }
    }
    assert(mid_bucket > 0);
    viz_init(&v);
    viz_step(&v, prefix[i_mid]);
    assert(v.bars[mid_bucket] == VIZ_MAX_EIGHTHS);
    assert(v.bars[mid_bucket - 1] == VIZ_SPILL_EIGHTHS);
    assert(v.bars[mid_bucket + 1] == VIZ_SPILL_EIGHTHS);

    // test 6. holding the same note decays every untouched bar
    // monotonically to zero while the sounding bar stays at full.
    int before = v.bars[mid_bucket + 2 < VIZ_BARS ? mid_bucket + 2 : 0];
    for (int s = 0; s < VIZ_MAX_EIGHTHS / VIZ_DECAY_EIGHTHS + 2; s++) {
        viz_step(&v, prefix[i_mid]);
        for (int i = 0; i < VIZ_BARS; i++) {
            if (i >= mid_bucket - 1 && i <= mid_bucket + 1) continue;
            assert(v.bars[i] <= before);
            assert(v.bars[i] >= 0);
        }
        assert(v.bars[mid_bucket] == VIZ_MAX_EIGHTHS);
    }
    for (int i = 0; i < VIZ_BARS; i++)
        if (i < mid_bucket - 1 || i > mid_bucket + 1)
            assert(v.bars[i] == 0);

    // test 7. a rest decays everything, including the sounding bar and
    // the spill, all the way to a flat strip.
    for (int s = 0; s < VIZ_MAX_EIGHTHS / VIZ_DECAY_EIGHTHS + 2; s++)
        viz_step(&v, prefix[i_rest]);
    for (int i = 0; i < VIZ_BARS; i++)
        assert(v.bars[i] == 0);

    // test 8. CLEAR saturates every bar, then decays back down on rests.
    viz_accent(&v, VIZ_ACCENT_CLEAR);
    for (int i = 0; i < VIZ_BARS; i++)
        assert(v.bars[i] == VIZ_MAX_EIGHTHS);
    assert(v.hostile == 0);
    viz_step(&v, prefix[i_rest]);
    for (int i = 0; i < VIZ_BARS; i++)
        assert(v.bars[i] == VIZ_MAX_EIGHTHS - VIZ_DECAY_EIGHTHS);

    // test 9. GARBAGE spikes alternating bars, arms the hostile tint,
    // and the tint burns down one step at a time to zero.
    viz_init(&v);
    viz_accent(&v, VIZ_ACCENT_GARBAGE);
    for (int i = 0; i < VIZ_BARS; i++)
        assert(v.bars[i] == (i % 2 == 0 ? VIZ_MAX_EIGHTHS : 0));
    assert(v.hostile == VIZ_HOSTILE_STEPS);
    viz_step(&v, prefix[i_rest]);
    assert(v.hostile == VIZ_HOSTILE_STEPS - 1);
    for (int s = 0; s < VIZ_HOSTILE_STEPS + 3; s++)
        viz_step(&v, prefix[i_rest]);
    assert(v.hostile == 0);

    // test 10. determinism: two states driven by the same synthetic
    // sequence stay memcmp-identical.
    viz_t a, b;
    viz_init(&a);
    viz_init(&b);
    for (uint32_t t = 0; t < 3 * total; t += 16) {
        viz_step(&a, t);
        viz_step(&b, t);
    }
    viz_accent(&a, VIZ_ACCENT_GARBAGE);
    viz_accent(&b, VIZ_ACCENT_GARBAGE);
    viz_step(&a, 5 * total + 7);
    viz_step(&b, 5 * total + 7);
    assert(memcmp(&a, &b, sizeof a) == 0);

    printf("test_viz: %zu notes over %u ms, %d buckets in range %u-%u hz, "
           "all checks passed\n",
           music_melody_len, total, VIZ_BARS, v.fmin, v.fmax);
    return 0;
}
