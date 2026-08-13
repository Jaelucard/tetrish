// A minimal, format-specific scanner for tetrisd's control-plane JSON: a
// top-level array of flat objects with string/int fields only (see jb_room
// and jb_player in src/tetrisd/main.c, the only two producers this ever
// has to read). Not a general JSON parser -- no nesting beyond one level,
// no escapes beyond a basic backslash-skip, no unicode handling. Pure text
// processing, no I/O, so it's unit-testable without a socket.

#ifndef TETRISH_VIEW_JSONISH_H
#define TETRISH_VIEW_JSONISH_H

#include <stddef.h>

// Finds the start of the Nth (0-based) top-level object in a JSON array
// string, returning a pointer to its opening '{', or NULL if there isn't
// one. Lets a caller iterate objects one at a time without a general parser.
const char *jsonish_nth_object(const char *array, int n);

// Extracts a string field's value (quotes stripped) from one JSON object
// (text starting at its '{', as returned by jsonish_nth_object). Returns 1
// and copies into out (NUL-terminated, truncated to fit cap) if the field
// was found as a quoted string; returns 0 (out untouched) otherwise.
int jsonish_str(const char *obj, const char *field, char *out, size_t cap);

// Extracts an integer field's value from one JSON object. Returns 1 and
// sets *out if the field was found as a bare (optionally signed) number;
// returns 0 (*out untouched) otherwise.
int jsonish_int(const char *obj, const char *field, long *out);

#endif
