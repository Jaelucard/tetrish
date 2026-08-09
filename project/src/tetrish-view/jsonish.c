// memmem() needs _GNU_SOURCE, defined globally in the Makefile's CFLAGS
// (same as fuzz_htttp.c relies on).
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "jsonish.h"

// Advances past a quoted string. p must point at the opening '"'. Returns a
// pointer just past the closing '"', or just past the end of input if the
// string was never closed (malformed input fails safe: the caller's brace
// counting simply stops making progress rather than reading out of bounds,
// since this never advances beyond a NUL).
static const char *skip_string(const char *p){
    p++;
    while (*p && *p != '"'){
        if (*p == '\\' && p[1]) p++;
        p++;
    }
    return (*p == '"') ? p + 1 : p;
}

// Returns a pointer to the '}' matching the '{' at obj, ignoring braces
// inside quoted strings. If that scan never closes -- an unescaped quote in
// a client-supplied name (jb_room/jb_player do no escaping) leaves the
// object with an odd quote count, putting the real '}' "inside a string"
// from the skipper's point of view -- retry counting braces with strings
// ignored entirely, so a hostile name cannot make the whole object vanish.
// Only truly brace-less input falls back to obj itself (an empty span).
static const char *object_end(const char *obj){
    int depth = 0;
    for (const char *p = obj; *p; p++){
        if (*p == '"'){ p = skip_string(p) - 1; continue; }
        if (*p == '{'){ depth++; }
        else if (*p == '}'){ depth--; if (depth == 0) return p; }
    }
    depth = 0;
    for (const char *p = obj; *p; p++){
        if (*p == '{'){ depth++; }
        else if (*p == '}'){ depth--; if (depth == 0) return p; }
    }
    return obj;
}

// finds the nth top-level '{' in a json array by counting brace depth
// (quoted strings skipped), letting callers iterate /rooms and /players
// entries one object at a time. contract in jsonish.h.
const char *jsonish_nth_object(const char *array, int n){
    int depth = 0, seen = -1;
    for (const char *p = array; *p; p++){
        if (*p == '"'){ p = skip_string(p) - 1; continue; }
        if (*p == '{'){
            if (depth == 0){
                seen++;
                if (seen == n) return p;
            }
            depth++;
        } else if (*p == '}'){
            if (depth > 0) depth--;
        }
    }
    return NULL;
}

// Locates "field": within [obj, object_end(obj)) and points *val_start at
// the first byte of its value, with *val_len bounding how far the value may
// extend (up to the object's own closing brace -- callers still have to
// stop at a comma/quote themselves). Returns 0 if the field isn't present.
//
// A match only counts at a KEY position: immediately after the object's
// '{' or a separating ',' (plus optional whitespace), and followed by ':'.
// Room and player names come from clients and jb_room/jb_player (tetrisd)
// do no escaping, so a hostile name can embed `"room": "fake` inside its
// own value -- a raw substring search would hand that decoy back as the
// real field. Rejected hits are skipped and the search continues, so the
// genuine key later in the object is still found.
static int find_field_value(const char *obj, const char *field,
                             const char **val_start, size_t *val_len){
    const char *end = object_end(obj);
    char needle[80];
    int nlen = snprintf(needle, sizeof needle, "\"%s\"", field);
    if (nlen <= 0 || (size_t)nlen >= sizeof needle) return 0;
    if (end <= obj) return 0;

    const char *from = obj;
    const char *hit;
    while ((hit = memmem(from, (size_t)(end - from), needle, (size_t)nlen)) != NULL){
        from = hit + 1;                       // where a rejected hit resumes

        const char *b = hit;
        while (b > obj && (b[-1] == ' ' || b[-1] == '\t')) b--;
        if (b == obj || (b[-1] != '{' && b[-1] != ',')) continue;

        const char *p = hit + nlen;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end || *p != ':') continue;
        p++;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end) continue;

        *val_start = p;
        *val_len   = (size_t)(end - p);
        return 1;
    }
    return 0;
}

// extracts a quoted string field's value with the quotes stripped,
// truncated to fit the caller's buffer. contract in jsonish.h.
int jsonish_str(const char *obj, const char *field, char *out, size_t cap){
    const char *v; size_t maxlen;
    if (!find_field_value(obj, field, &v, &maxlen)) return 0;
    if (maxlen == 0 || *v != '"' || cap == 0) return 0;
    v++; maxlen--;                  // step inside the opening quote

    // copy up to the closing quote, stopping early if the buffer fills
    size_t n = 0;
    while (n < maxlen && v[n] != '"' && n + 1 < cap) n++;
    if (n < cap){ memcpy(out, v, n); out[n] = '\0'; }
    else { out[0] = '\0'; }
    return 1;
}

// extracts a bare (optionally negative) integer field's value. contract
// in jsonish.h.
int jsonish_int(const char *obj, const char *field, long *out){
    const char *v; size_t maxlen;
    if (!find_field_value(obj, field, &v, &maxlen)) return 0;

    // collect the digit run into a bounded scratch buffer, then convert
    char buf[32];
    size_t n = 0;
    while (n < maxlen && n + 1 < sizeof buf &&
           (isdigit((unsigned char)v[n]) || (n == 0 && v[n] == '-')))
        { buf[n] = v[n]; n++; }
    if (n == 0 || (n == 1 && buf[0] == '-')) return 0;

    buf[n] = '\0';
    *out = strtol(buf, NULL, 10);
    return 1;
}
