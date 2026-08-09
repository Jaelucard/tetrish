// Unit tests for the hand-rolled line editor (src/tetrish-view/edit.c):
// printable input, backspace, ctrl-u, ESC-sequence arrow parsing for
// history, and edit_commit's push/dedup/reset behaviour. Pure state, no
// tty, no I/O -- so it's unit-testable without a terminal.

#include <stdio.h>
#include <string.h>
#include "edit.h"

static int failures = 0;

static void check(const char *what, int cond){
    printf("  %-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) failures++;
}

// Feeds a NUL-terminated string byte by byte, ignoring results.
static void feed_str(edit_state_t *st, const char *s){
    for (; *s; s++) edit_feed(st, (unsigned char)*s);
}

static int buf_eq(const edit_state_t *st, const char *s){
    size_t n = strlen(s);
    return st->len == n && memcmp(st->buf, s, n) == 0;
}

int main(void){
    printf("test_view_edit: edit_init\n");
    {
        edit_state_t st;
        edit_init(&st);
        check("len starts at 0",          st.len == 0);
        check("hist_count starts at 0",   st.hist_count == 0);
        check("hist_cursor starts at -1", st.hist_cursor == -1);
        check("esc_state starts at 0",    st.esc_state == 0);
    }

    printf("\ntest_view_edit: printable input\n");
    {
        edit_state_t st;
        edit_init(&st);
        edit_result_t r1 = edit_feed(&st, 'h');
        edit_result_t r2 = edit_feed(&st, 'i');
        check("printable byte returns EDIT_REDRAW", r1 == EDIT_REDRAW && r2 == EDIT_REDRAW);
        check("buffer holds the typed text",        buf_eq(&st, "hi"));
    }
    {
        edit_state_t st;
        edit_init(&st);
        edit_result_t r = edit_feed(&st, 0x01);          // ctrl-a: not printable, not special
        check("non-printable control byte returns EDIT_NONE", r == EDIT_NONE);
        check("non-printable control byte isn't inserted",    st.len == 0);
    }
    {
        edit_state_t st;
        edit_init(&st);
        for (size_t i = 0; i + 1 < EDIT_LINE_MAX; i++) edit_feed(&st, 'x');
        size_t full_len = st.len;
        edit_result_t r = edit_feed(&st, 'y');           // buffer now full
        check("a full buffer stops growing", st.len == full_len);
        check("a full buffer reports EDIT_NONE for the overflow byte", r == EDIT_NONE);
    }

    printf("\ntest_view_edit: backspace\n");
    {
        edit_state_t st;
        edit_init(&st);
        feed_str(&st, "abc");
        edit_result_t r = edit_feed(&st, 0x7f);          // DEL
        check("backspace (DEL) removes the last char", buf_eq(&st, "ab"));
        check("backspace returns EDIT_REDRAW",          r == EDIT_REDRAW);
        edit_feed(&st, 0x08);                            // BS
        check("backspace (BS) also removes the last char", buf_eq(&st, "a"));
    }
    {
        edit_state_t st;
        edit_init(&st);
        edit_result_t r = edit_feed(&st, 0x7f);
        check("backspace on an empty line returns EDIT_NONE", r == EDIT_NONE);
        check("backspace on an empty line stays empty",       st.len == 0);
    }

    printf("\ntest_view_edit: ctrl-u\n");
    {
        edit_state_t st;
        edit_init(&st);
        feed_str(&st, "clear me");
        edit_result_t r = edit_feed(&st, 0x15);
        check("ctrl-u clears the whole line", st.len == 0);
        check("ctrl-u returns EDIT_REDRAW",   r == EDIT_REDRAW);
    }
    {
        edit_state_t st;
        edit_init(&st);
        edit_result_t r = edit_feed(&st, 0x15);
        check("ctrl-u on an empty line returns EDIT_NONE", r == EDIT_NONE);
    }

    printf("\ntest_view_edit: line ready\n");
    {
        edit_state_t st;
        edit_init(&st);
        feed_str(&st, "status");
        check("\\r yields EDIT_LINE_READY", edit_feed(&st, '\r') == EDIT_LINE_READY);
    }
    {
        edit_state_t st;
        edit_init(&st);
        feed_str(&st, "status");
        check("\\n yields EDIT_LINE_READY", edit_feed(&st, '\n') == EDIT_LINE_READY);
    }

    printf("\ntest_view_edit: edit_commit push/dedup/reset\n");
    {
        edit_state_t st;
        edit_init(&st);
        feed_str(&st, "status");
        edit_feed(&st, '\r');
        edit_commit(&st);
        check("commit resets len to 0",         st.len == 0);
        check("commit resets hist_cursor to -1", st.hist_cursor == -1);
        check("commit pushed one history entry", st.hist_count == 1);
    }
    {
        edit_state_t st;
        edit_init(&st);
        feed_str(&st, "status"); edit_feed(&st, '\r'); edit_commit(&st);
        feed_str(&st, "status"); edit_feed(&st, '\r'); edit_commit(&st);
        check("committing the same line twice in a row is not duplicated",
              st.hist_count == 1);
    }
    {
        edit_state_t st;
        edit_init(&st);
        edit_feed(&st, '\r');   // empty line
        edit_commit(&st);
        check("committing an empty line pushes nothing", st.hist_count == 0);
    }
    {
        edit_state_t st;
        edit_init(&st);
        feed_str(&st, "rooms");   edit_feed(&st, '\r'); edit_commit(&st);
        feed_str(&st, "players"); edit_feed(&st, '\r'); edit_commit(&st);
        feed_str(&st, "rooms");   edit_feed(&st, '\r'); edit_commit(&st);
        check("a repeat of an earlier (non-adjacent) entry is still pushed",
              st.hist_count == 3);
    }

    printf("\ntest_view_edit: history recall via ESC [ A / B\n");
    {
        edit_state_t st;
        edit_init(&st);
        feed_str(&st, "status"); edit_feed(&st, '\r'); edit_commit(&st);
        feed_str(&st, "rooms");  edit_feed(&st, '\r'); edit_commit(&st);

        edit_feed(&st, 0x1b);
        edit_result_t mid = edit_feed(&st, '[');
        check("ESC then '[' alone returns EDIT_NONE (awaiting the letter)", mid == EDIT_NONE);
        edit_result_t r = edit_feed(&st, 'A');
        check("ESC [ A recalls the most recent entry", r == EDIT_REDRAW && buf_eq(&st, "rooms"));

        r = edit_feed(&st, 0x1b); edit_feed(&st, '['); r = edit_feed(&st, 'A');
        check("a second ESC [ A recalls one entry further back", r == EDIT_REDRAW && buf_eq(&st, "status"));

        r = edit_feed(&st, 0x1b); edit_feed(&st, '['); r = edit_feed(&st, 'A');
        check("ESC [ A past the oldest entry stays on the oldest entry",
              r == EDIT_NONE && buf_eq(&st, "status"));
    }
    {
        edit_state_t st;
        edit_init(&st);
        feed_str(&st, "status"); edit_feed(&st, '\r'); edit_commit(&st);
        feed_str(&st, "rooms");  edit_feed(&st, '\r'); edit_commit(&st);

        edit_feed(&st, 0x1b); edit_feed(&st, '['); edit_feed(&st, 'A'); // -> rooms
        edit_feed(&st, 0x1b); edit_feed(&st, '['); edit_feed(&st, 'A'); // -> status
        edit_result_t r = edit_feed(&st, 0x1b);
        edit_feed(&st, '['); r = edit_feed(&st, 'B');                   // back down
        check("ESC [ B after recalling moves toward newer entries",
              r == EDIT_REDRAW && buf_eq(&st, "rooms"));

        r = edit_feed(&st, 0x1b); edit_feed(&st, '['); r = edit_feed(&st, 'B');
        check("ESC [ B past the newest recalled entry clears to an empty line",
              r == EDIT_REDRAW && st.len == 0);
    }
    {
        edit_state_t st;
        edit_init(&st);
        edit_result_t r = edit_feed(&st, 0x1b);
        edit_feed(&st, '[');
        r = edit_feed(&st, 'A');
        check("ESC [ A with no history at all returns EDIT_NONE", r == EDIT_NONE);
    }
    {
        // Down-arrow while typing a fresh line (never recalled history) must
        // not touch the buffer: only down FROM a recalled entry clears back
        // to an empty line. Wiping in-progress input on a stray keypress
        // loses what the user was composing.
        edit_state_t st;
        edit_init(&st);
        feed_str(&st, "old"); edit_feed(&st, '\r'); edit_commit(&st);
        feed_str(&st, "statu");                  // typing, hist_cursor still -1
        edit_feed(&st, 0x1b); edit_feed(&st, '[');
        edit_result_t r = edit_feed(&st, 'B');
        check("down-arrow while typing (not recalling) leaves the line alone",
              r == EDIT_NONE && buf_eq(&st, "statu"));
    }
    {
        edit_state_t st;
        edit_init(&st);
        feed_str(&st, "status"); edit_feed(&st, '\r'); edit_commit(&st);
        edit_feed(&st, 0x1b);
        edit_result_t r = edit_feed(&st, 'x');    // ESC not followed by '['
        check("ESC not followed by '[' resets esc_state and consumes the byte",
              r == EDIT_NONE && st.esc_state == 0);
        r = edit_feed(&st, 'y');
        check("...and typing resumes normally afterward", r == EDIT_REDRAW && buf_eq(&st, "y"));
    }
    {
        edit_state_t st;
        edit_init(&st);
        feed_str(&st, "status"); edit_feed(&st, '\r'); edit_commit(&st);
        edit_feed(&st, 0x1b); edit_feed(&st, '[');
        edit_result_t r = edit_feed(&st, 'C');    // right arrow: unhandled, consumed silently
        check("an unhandled ESC [ letter is consumed without inserting garbage",
              r == EDIT_NONE && st.len == 0);
    }

    printf("\ntest_view_edit: history ring wraparound\n");
    {
        edit_state_t st;
        edit_init(&st);
        char line[16];
        for (int i = 0; i < EDIT_HISTORY_MAX + 5; i++){
            snprintf(line, sizeof line, "cmd%d", i);
            feed_str(&st, line);
            edit_feed(&st, '\r');
            edit_commit(&st);
        }
        check("history count caps at EDIT_HISTORY_MAX", st.hist_count == EDIT_HISTORY_MAX);

        edit_feed(&st, 0x1b); edit_feed(&st, '['); edit_feed(&st, 'A');
        char expect[16];
        snprintf(expect, sizeof expect, "cmd%d", EDIT_HISTORY_MAX + 5 - 1);
        check("after wraparound, ESC [ A still recalls the most recent entry",
              buf_eq(&st, expect));

        for (int i = 0; i < EDIT_HISTORY_MAX - 1; i++)
            edit_feed(&st, 0x1b), edit_feed(&st, '['), edit_feed(&st, 'A');
        snprintf(expect, sizeof expect, "cmd%d", 5);   // oldest surviving entry
        check("after wraparound, the oldest surviving (not overwritten) entry is reachable",
              buf_eq(&st, expect));
    }

    printf("\ntest_view_edit: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
