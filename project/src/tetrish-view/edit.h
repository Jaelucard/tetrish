// A hand-rolled line editor: basic editing (backspace, ctrl-u) and command
// history (up/down), fed one input byte at a time. Pure state -- no I/O, no
// termios, no drawing -- so it's unit-testable without a terminal
// (tests/test_view_edit.c). repl.c owns the actual raw-mode tty and does
// the drawing in response to what edit_feed reports.
//
// Deliberately not readline or any other external line-editing library:
// CLAUDE.md requires user approval for that, and this is small enough not
// to need it.

#ifndef TETRISH_VIEW_EDIT_H
#define TETRISH_VIEW_EDIT_H

#include <stddef.h>

#define EDIT_LINE_MAX    256
#define EDIT_HISTORY_MAX  32

typedef struct {
    char   buf[EDIT_LINE_MAX];
    size_t len;                 // bytes currently in buf; cursor is always at the end

    char history[EDIT_HISTORY_MAX][EDIT_LINE_MAX];
    int  hist_count;            // populated slots, caps at EDIT_HISTORY_MAX
    int  hist_next;             // ring write cursor
    int  hist_cursor;           // -1 = not recalling; else N entries back from newest

    int  esc_state;             // 0 none, 1 saw ESC, 2 saw ESC '[' (for arrow keys)
} edit_state_t;

typedef enum {
    EDIT_NONE,         // byte consumed, nothing for the caller to do
    EDIT_REDRAW,       // the visible line changed; caller should redraw buf[0..len)
    EDIT_LINE_READY,   // buf[0..len) is a complete line; caller reads it, then
                        // must call edit_commit() before feeding more bytes
} edit_result_t;

void edit_init(edit_state_t *st);

// Feeds one input byte, updating st and reporting what happened.
edit_result_t edit_feed(edit_state_t *st, unsigned char byte);

// Call after consuming an EDIT_LINE_READY line: pushes a non-empty,
// non-duplicate-of-the-last-entry line onto history, then clears the
// buffer and history-recall state for the next line.
void edit_commit(edit_state_t *st);

#endif
