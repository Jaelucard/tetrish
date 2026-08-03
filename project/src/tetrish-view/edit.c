#include <string.h>

#include "edit.h"

// resets the editor to an empty line with empty history. -1 hist_cursor
// means "not browsing history right now".
void edit_init(edit_state_t *st){
    memset(st, 0, sizeof *st);
    st->hist_cursor = -1;
}

// Copies history slot `index` entries back from the most recent (0 = most
// recent) into buf. Returns EDIT_NONE if index is out of range.
static edit_result_t recall(edit_state_t *st, int index){
    if (index < 0 || index >= st->hist_count) return EDIT_NONE;
    int slot = (st->hist_next - 1 - index + EDIT_HISTORY_MAX) % EDIT_HISTORY_MAX;
    size_t n = strlen(st->history[slot]);
    if (n >= EDIT_LINE_MAX) n = EDIT_LINE_MAX - 1;
    memcpy(st->buf, st->history[slot], n);
    st->len = n;
    st->hist_cursor = index;
    return EDIT_REDRAW;
}

static edit_result_t history_up(edit_state_t *st){     // toward older entries
    return recall(st, (st->hist_cursor < 0) ? 0 : st->hist_cursor + 1);
}

static edit_result_t history_down(edit_state_t *st){   // toward newer entries
    // Not recalling at all: down must leave the line being typed alone --
    // clearing it would lose unsent input on a stray keypress.
    if (st->hist_cursor < 0) return EDIT_NONE;
    if (st->hist_cursor == 0){
        // Down from the newest recalled entry clears back to a fresh empty
        // line, same as most shells.
        st->len = 0;
        st->hist_cursor = -1;
        return EDIT_REDRAW;
    }
    return recall(st, st->hist_cursor - 1);
}

// the editor's whole input path: consumes ONE byte and reports what the
// caller should do about it -- redraw the line, treat it as complete, or
// nothing. handles printable text, backspace, ctrl-u, and the ESC [ A/B
// arrow sequences for history. contract in edit.h.
edit_result_t edit_feed(edit_state_t *st, unsigned char byte){
    // Arrow keys arrive as the three-byte sequence ESC '[' <letter>. Only
    // up/down (history) are handled; anything else after ESC '[' (left,
    // right, delete, ...) is consumed silently rather than inserted as
    // literal garbage into the line.
    if (st->esc_state == 1){
        st->esc_state = (byte == '[') ? 2 : 0;
        return EDIT_NONE;
    }
    if (st->esc_state == 2){
        st->esc_state = 0;
        if (byte == 'A') return history_up(st);
        if (byte == 'B') return history_down(st);
        return EDIT_NONE;
    }
    if (byte == 0x1b){ st->esc_state = 1; return EDIT_NONE; }

    if (byte == '\r' || byte == '\n') return EDIT_LINE_READY;

    if (byte == 0x7f || byte == 0x08){          // backspace: DEL or BS
        if (st->len == 0) return EDIT_NONE;
        st->len--;
        return EDIT_REDRAW;
    }

    if (byte == 0x15){                          // ctrl-u: clear the line
        if (st->len == 0) return EDIT_NONE;
        st->len = 0;
        return EDIT_REDRAW;
    }

    // Printable ASCII only. Commands and their arguments are plain
    // identifiers, so raw control bytes and multi-byte UTF-8 are simply
    // dropped rather than accepted and mis-rendered.
    if (byte >= 0x20 && byte < 0x7f && st->len + 1 < EDIT_LINE_MAX){
        st->buf[st->len++] = (char)byte;
        return EDIT_REDRAW;
    }
    return EDIT_NONE;
}

// called after the caller consumed an EDIT_LINE_READY line: pushes it
// into the history ring (skipping empties and immediate repeats), then
// clears the buffer and recall state for the next line. contract in edit.h.
void edit_commit(edit_state_t *st){
    if (st->len > 0){
        // snapshot the line as a NUL-terminated string for the ring
        size_t n = st->len < EDIT_LINE_MAX - 1 ? st->len : EDIT_LINE_MAX - 1;
        char entry[EDIT_LINE_MAX];
        memcpy(entry, st->buf, n);
        entry[n] = '\0';

        // dedup against the most recent entry only, like shells do: running
        // the same command twice in a row costs one slot, not two
        int last = (st->hist_next - 1 + EDIT_HISTORY_MAX) % EDIT_HISTORY_MAX;
        if (st->hist_count == 0 || strcmp(st->history[last], entry) != 0){
            memcpy(st->history[st->hist_next], entry, n + 1);
            st->hist_next = (st->hist_next + 1) % EDIT_HISTORY_MAX;
            if (st->hist_count < EDIT_HISTORY_MAX) st->hist_count++;
        }
    }
    st->len = 0;
    st->hist_cursor = -1;
}
