#include <stddef.h>
#include <stdlib.h>
#include <ctype.h>      // isspace: relying on the implicit declaration is an
                        // error on clang and gcc 14+, not just a warning
#include <string.h>
#include <strings.h>   // strncasecmp: it lives here, not in <string.h>
#include <ctype.h>     // isspace, used by trim() below
#include <stdio.h>
#include <time.h>
#include "htttp.h"

static void _msg_reset(htttp_msg_t *msg) {
    memset(msg, 0, sizeof(*msg));
    msg->content_length = -1;
}

static void trim(const char **start, const char **finish) {
    while (*start < *finish && isspace((unsigned char)**start)) {
        (*start)++;
    }
    while (*finish > *start && isspace((unsigned char)*((*finish) - 1))) {
        (*finish)--;
    }
}

static htttp_err_t parse_headers(htttp_msg_t *msg, const char **cur, const char *end) {
    while (1) {
        const char *line_end = find_crlf(*cur, end);
        if (!line_end) {
            return HTTTP_ERR_INCOMPLETE;
        }
 
        /* Blank line means the headers are over. */
        if (line_end == *cur) {
            *cur += 2;   /* step over the blank CRLF */
            return HTTTP_OK;
        }
 
        /* Fixed-size headers array, so cap it or we walk off the end. */
        if (msg->header_count >= HTTTP_MAX_HEADERS) {
            return HTTTP_ERR_MALFORMED;
        }
 
        /* FIRST colon splits the line. A later one belongs to the value,
         * which is how "X-Custom: foo:bar" survives. */
        const char *colon = memchr(*cur, ':', (size_t)(line_end - *cur));
        if (!colon) {
            return HTTTP_ERR_MALFORMED;
        }
 
        /* Name runs cur..colon. Trailing space gets trimmed, leading space
         * does not, because a header name starting with whitespace is a
         * folded continuation line and HTTTP/1.0 does not do those. */
        const char *name_s = *cur;
        const char *name_e = colon;
        while (name_e > name_s && isspace((unsigned char)*(name_e - 1))) {
            name_e--;
        }
 
        if (name_s == name_e) {
            return HTTTP_ERR_MALFORMED;   /* empty name */
        }
 
        /* Value runs colon+1..line_end, trimmed at both ends. */
        const char *val_s = colon + 1;
        const char *val_e = line_end;
        trim(&val_s, &val_e);
 
        /* Pointers into the caller's buffer. Nothing is copied, which is why
         * the buffer has to outlive the msg. */
        htttp_header_t *h = &msg->headers[msg->header_count++];
        h->name = name_s;
        h->name_len = (size_t)(name_e - name_s);
        h->value = val_s;
        h->value_len = (size_t)(val_e - val_s);
 
        /* Cache the three headers the rest of the stack asks for constantly,
         * so it does not have to walk the array every time. strncasecmp and
         * not strncmp because header names are case-insensitive. */
        if (h->name_len == 14 && strncasecmp(name_s, "Content-Length", 14) == 0) {
 
            /* strtol wants a NUL-terminated string and we have a pointer into
             * the middle of a buffer, so copy it out first. */
            char tmp[32];
            if (h->value_len >= sizeof(tmp)) {
                return HTTTP_ERR_MALFORMED;
            }
            memcpy(tmp, val_s, h->value_len);
            tmp[h->value_len] = '\0';
 
            char *ep;
            long n = strtol(tmp, &ep, 10);
            if (*ep != '\0' || n < 0) {
                return HTTTP_ERR_MALFORMED;
            }
 
            msg->content_length = n;
 
        } else if (h->name_len == 12 && strncasecmp(name_s, "Content-Type", 12) == 0) {
            msg->content_type = val_s;   /* points into caller's buf */
        } else if (h->name_len == 9 && strncasecmp(name_s, "Player-Id", 9) == 0) {
            msg->player_id = val_s;      /* points into caller's buf */
        }

        *cur = line_end + 2;   /* advance past this header's CRLF */
    }
}

static htttp_err_t parse_body(htttp_msg_t *msg, const char *cur, const char *end) {
    if (msg->content_length <= 0) {
        /* No body. _msg_reset already left body NULL and body_len 0. */
        return HTTTP_OK;
    }
 
    size_t remaining = (size_t)(end - cur);
    if (remaining < (size_t)msg->content_length) {
        return HTTTP_ERR_INCOMPLETE;
    }
 
    msg->body = cur;
    msg->body_len = (size_t)msg->content_length;
    return HTTTP_OK;
}

static const struct {htttp_method_t m; const char *s;} _method_table[] = {
    {HTTTP_METHOD_JOIN, "JOIN"},
    {HTTTP_METHOD_LEAVE, "LEAVE"},
    {HTTTP_METHOD_START, "START"},
    {HTTTP_METHOD_MOVE, "MOVE"},
    {HTTTP_METHOD_ROTATE, "ROTATE"},
    {HTTTP_METHOD_DROP, "DROP"},
    {HTTTP_METHOD_STATE, "STATE"},
    {HTTTP_METHOD_GET, "GET"},
    {HTTTP_METHOD_HOLD, "HOLD"},
    {HTTTP_METHOD_PAUSE, "PAUSE"},
    {HTTTP_METHOD_RESUME, "RESUME"},
    {HTTTP_METHOD_UNKNOWN, NULL}
};

const char *htttp_method_str(htttp_method_t method) {
    for (int i = 0; _method_table[i].s; i++) {
        if (_method_table[i].m == method) {
            return _method_table[i].s;
        }
    }
    return "UNKNOWN";
}

htttp_method_t htttp_method_from_str(const char *s) {
    if (!s) {
        return HTTTP_METHOD_UNKNOWN;
    }
    for (int i = 0; _method_table[i].s; i++) {
        if (strcmp(s, _method_table[i].s) == 0) {
            return _method_table[i].m;
        }
    }
    return HTTTP_METHOD_UNKNOWN;
}

static const struct {htttp_status_t code; const char *reason;} _status_table[] = {
    {HTTTP_200_OK, "OK"},
    {HTTTP_201_CREATED, "Created"},
    {HTTTP_400_BAD_REQUEST, "Bad Request"},
    {HTTTP_401_UNAUTHORIZED, "Unauthorized"},
    {HTTTP_403_FORBIDDEN, "Forbidden"},
    {HTTTP_404_NOT_FOUND, "Not Found"},
    {HTTTP_409_CONFLICT, "Conflict"},
    {HTTTP_413_PAYLOAD_TOO_LARGE, "Payload Too Large"},
    {HTTTP_429_TOO_MANY_REQUESTS, "Too Many Requests"},
    {HTTTP_500_INTERNAL_ERROR, "Internal Server Error"},
    {0, NULL}
};

const char *htttp_status_reason(htttp_status_t status) {
    for (int i = 0; _status_table[i].reason; i++) {
        if (_status_table[i].code == status) {
            return _status_table[i].reason;
        }
    }
    return "Unknown";
}

const char *htttp_strerror(htttp_err_t err) {
    switch (err) {
    case HTTTP_OK: return "OK";
    case HTTTP_ERR_INCOMPLETE: return "incomplete message";
    case HTTTP_ERR_MALFORMED: return "malformed message";
    case HTTTP_ERR_BAD_VERSION: return "unsupported protocol version";
    case HTTTP_ERR_TOO_LARGE: return "payload too large";
    case HTTTP_ERR_NO_MEMORY: return "memory allocation failure";
    case HTTTP_ERR_BAD_HEADER: return "missing or invalid required header";
    default: return "unknown error";
    }
}

htttp_err_t htttp_parse_request(const char *buf, size_t buf_len, htttp_msg_t *msg) {
    if (!buf || !msg) {
        return HTTTP_ERR_MALFORMED;
    }
    _msg_reset(msg);

    const char *cur = buf;
    const char *end = buf + buf_len;

    /* 1. Where does the request-line end */
    const char *line_end = find_crlf(cur, end);
    if (!line_end) {
        return HTTTP_ERR_INCOMPLETE;
    }

    /* 2. Two spaces cut it into METHOD / PATH / VERSION */
    const char *sp1 = memchr(cur, ' ', (size_t)(line_end - cur));
    if (!sp1) {
        return HTTTP_ERR_MALFORMED;
    }
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(line_end - (sp1 + 1)));
    if (!sp2) {
        return HTTTP_ERR_MALFORMED;
    }

    /* 2.1 METHOD */
    size_t method_len = (size_t)(sp1 - cur);
    if (method_len == 0 || method_len >= HTTTP_MAX_METHOD_LEN) {
        return HTTTP_ERR_MALFORMED;
    }
    memcpy(msg->method_str, cur, method_len);
    msg->method_str[method_len] = '\0';
    msg->method = htttp_method_from_str(msg->method_str);

    /* 2.2 PATH */
    size_t path_len = (size_t)(sp2 - (sp1 + 1));
    if (path_len == 0 || path_len >= HTTTP_MAX_PATH_LEN) {
        return HTTTP_ERR_MALFORMED;
    }
    memcpy(msg->path, sp1 + 1, path_len);
    msg->path[path_len] = '\0';

    /* 2.3 VERSION */
    const char *ver = sp2 + 1;
    size_t ver_len = (size_t)(line_end - ver);
    if (ver_len != (sizeof(HTTTP_VERSION) - 1) || memcmp(ver, HTTTP_VERSION, ver_len) != 0) {
        return HTTTP_ERR_BAD_VERSION;
    }

    cur = line_end + 2;

    /* 3. Parse headers */
    while (1) {
        line_end = find_crlf(cur, end);
        if (!line_end) {
            return HTTTP_ERR_INCOMPLETE;
        }

        if (line_end == cur) { 
            cur += 2;
            break;
        }

        if (msg->header_count >= HTTTP_MAX_HEADERS) {
            return HTTTP_ERR_MALFORMED;
        }

        const char *colon = memchr(cur, ':', (size_t)(line_end - cur));
        if (!colon) {
            return HTTTP_ERR_MALFORMED;
        }

        const char *name_s = cur;
        const char *name_e = colon;           
        
        /* trailing space off the name only */
        while (name_e > name_s && isspace((unsigned char)*(name_e - 1))) {
            name_e--;
        }

        const char *val_s = colon + 1;
        const char *val_e = line_end;

        /* value gets trimmed at both ends */
        trim(&val_s, &val_e);

        /* 3.1 Into the headers array, pointers only, no copying */
        htttp_header_t *h = &msg->headers[msg->header_count++];
        h->name = name_s;
        h->name_len = (size_t)(name_e - name_s);
        h->value = val_s;
        h->value_len = (size_t)(val_e - val_s);

        /* 3.2 Content-Length, the one that needs actual parsing */
        if (h->name_len == 14 && strncasecmp(name_s, "Content-Length", 14) == 0) {
            char tmp[32];

            /* stops a silly-long value overflowing tmp */
            if (h->value_len >= sizeof(tmp)) {
                return HTTTP_ERR_MALFORMED;
            }

            memcpy(tmp, val_s, h->value_len);
            tmp[h->value_len] = '\0';

            char *ep;
            long n = strtol(tmp, &ep, 10);
            if (*ep != '\0' || n < 0) {
                return HTTTP_ERR_MALFORMED;
            }

            msg->content_length = n;
        }

        /* 3.3 Stores pointer to Content-Type value */
        else if (h->name_len == 12 && strncasecmp(name_s, "Content-Type", 12) == 0) {
            msg->content_type = val_s;   /* points into buf */
        }

        /* 3.4 Stores pointer to Player-Id value */
        else if (h->name_len == 9 && strncasecmp(name_s, "Player-Id", 9) == 0) {
            msg->player_id = val_s;      /* points into buf */
        }

        cur = line_end + 2;
    }

    /* 4. Body, only if a length was declared. Short buffer = INCOMPLETE, so
     * a Content-Length bigger than the bytes actually present never lets us
     * point body past the end of buf. */
    if (msg->content_length > 0) {
        size_t remaining = (size_t)(end - cur);
        if (remaining < (size_t)msg->content_length) {
            return HTTTP_ERR_INCOMPLETE;
        }
        msg->body = cur;
        msg->body_len = (size_t)msg->content_length;
    }

    msg->is_request = 1;
    return HTTTP_OK;
}

/*
Walkthrough of the above, for when I have forgotten how this works.

Null pointers out first. Then wipe the output struct, because a half-filled
msg from a failed parse is worse than an empty one.

Two pointers do all the work: cur = where the parser is now, end = one past
the last byte. Nothing ever reads at or past end.

buf
 |
 v
GET /foo HTTTP/1.0\r\n
...
                    ^
                    end

1. Look for the \r\n that ends the request line. line_end points at the \r.
   No CRLF yet just means the request line has not all arrived, so
   INCOMPLETE, not MALFORMED. The caller reads more and comes back.
2. First and second spaces in that line split it into METHOD, PATH, VERSION.
2.1 Length-check METHOD, reject empty or too long, copy it into method_str,
    then map it to the enum through _method_table. An unknown verb is not an
    error here, it just becomes HTTTP_METHOD_UNKNOWN and the dispatcher deals
    with it.
2.2 Same length-check for PATH, copy it into msg->path.
2.3 VERSION has to be exactly "HTTTP/1.0" or it is BAD_VERSION.
Step past the \r\n and we are at the first header.
3 Loop until the empty line. Each header line splits on its first ':', name
  on the left, value on the right.
3.1 Grab the next header slot, store where name and value start and how long
    they are. Still no copying.
3.2 If the name is 'Content-Length' (case-insensitively), copy the value out
    to a NUL-terminated tmp, strtol it, keep the number.
3.3 Content-Type, just remember where the value is.
3.4 Player-Id, same.
4. Body only if a length was declared. Count what is left in the buffer. If
   the header claims 100 bytes and only 50 are here, INCOMPLETE. Otherwise
   body points into the original buffer and body_len is set.
*/

htttp_err_t htttp_parse_response(const char *buf, size_t buf_len, htttp_msg_t *msg) {
    if (!buf || !msg) return HTTTP_ERR_MALFORMED;
    _msg_reset(msg);
 
    const char *cur = buf;
    const char *end = buf + buf_len;
 
    /* 1. Where the status-line ends */
    const char *line_end = find_crlf(cur, end);
    if (!line_end) {
        return HTTTP_ERR_INCOMPLETE;
    }
 
    /* 2. Split "VERSION SP STATUS-CODE SP REASON-PHRASE" */
    const char *sp1 = memchr(cur, ' ', (size_t)(line_end - cur));
    if (!sp1) {
        return HTTTP_ERR_MALFORMED;
    }
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(line_end - (sp1 + 1)));
    if (!sp2) {
        return HTTTP_ERR_MALFORMED;
    }
 
    /* 2.1 VERSION */
    size_t ver_len = (size_t)(sp1 - cur);
    if (ver_len != (sizeof(HTTTP_VERSION) - 1) || memcmp(cur, HTTTP_VERSION, ver_len) != 0) {
        return HTTTP_ERR_BAD_VERSION;
    }
 
    /* 2.2 STATUS-CODE */
    size_t code_len = (size_t)(sp2 - (sp1 + 1));
    if (code_len == 0 || code_len > 3) {
        return HTTTP_ERR_MALFORMED;
    }
 
    char code_buf[4];
    memcpy(code_buf, sp1 + 1, code_len);
    code_buf[code_len] = '\0';
 
    char *ep;
    long code = strtol(code_buf, &ep, 10);
    if (*ep != '\0' || code < 100 || code > 599)
        return HTTTP_ERR_MALFORMED;
 
    msg->status = (htttp_status_t)code;
 
    /* 2.3 REASON-PHRASE */
    size_t reason_len = (size_t)(line_end - (sp2 + 1));
    if (reason_len >= HTTTP_MAX_REASON_LEN) {
        return HTTTP_ERR_MALFORMED;
    }
    memcpy(msg->reason, sp2 + 1, reason_len);
    msg->reason[reason_len] = '\0';
 
    cur = line_end + 2;
 
    /* 4. Headers. Shared with the request path, unlike the start-line. */
    htttp_err_t err = parse_headers(msg, &cur, end);
    if (err != HTTTP_OK) {
        return err;
    }
 
    /* 5. Body, and check the declared length actually fits in what we have */
    err = parse_body(msg, cur, end);
    if (err != HTTTP_OK) {
        return err;
    }
 
    msg->is_request = 0;
    return HTTTP_OK;
}

/*
Same shape as the request parser, only the first line differs.
1. First CRLF ends the status-line.
2. Split it: version SP status-code SP reason-phrase.
3. status-code through strtol, must land in 100..599.
4. Header loop, shared with the request side.
5. Point at the body if Content-Length > 0.
*/

const char *htttp_find_header(const htttp_msg_t *msg, const char *name, size_t *value_len) {
    /* Case-insensitive lookup by name. */
    if (!msg || !name) {
        if (value_len) {
            *value_len = 0;
        }
        return NULL;
    }
 
    size_t name_len = strlen(name);
 
    /* Linear scan. 32 headers max, so no point being clever about it. */
    for (size_t i = 0; i < msg->header_count; i++) {
        const htttp_header_t *h = &msg->headers[i];
 
        /* Length has to match first. strncasecmp alone would say "Date"
         * matches "Date-Modified", since it only looks at name_len bytes. */
        if (h->name_len != name_len)
            continue;
 
        /* Then case-insensitively, so "content-length" finds "Content-Length". */
        if (strncasecmp(h->name, name, name_len) == 0) {
            if (value_len) *value_len = h->value_len;
            return h->value;   /* points into the original parse buffer */
        }
    }
 
    if (value_len) {
        *value_len = 0;
    }
    return NULL;
}

void htttp_builder_init_request(htttp_builder_t *b, htttp_method_t method, const char *path) {
    if (!b) {
        return;
    }
    memset(b, 0, sizeof(*b));
    b->is_request = 1;
    b->method = method;
    b->path = path;
}

void htttp_builder_init_response(htttp_builder_t *b, htttp_status_t status) {
    if (!b) {
        return;
    }
    memset(b, 0, sizeof(*b));
    b->is_request = 0;
    b->status = status;
}

htttp_err_t htttp_builder_add_header(htttp_builder_t *b, const char *name, const char *value) {
    if (!b || !name || !value) {
        return HTTTP_ERR_MALFORMED;
    }
    if (b->header_count >= HTTTP_MAX_HEADERS) {
        return HTTTP_ERR_TOO_LARGE;
    }
    b->headers[b->header_count].name  = name;
    b->headers[b->header_count].value = value;
    b->header_count++;
    return HTTTP_OK;
}

void htttp_builder_set_body(htttp_builder_t *b, const unsigned char *body, size_t body_len) {
    if (!b) {
        return;
    }
    b->body = body;
    b->body_len = body_len;
    /* Do not add Content-Length yourself, htttp_serialise() does it. */
}

char *htttp_serialise(const htttp_builder_t *b, size_t *out_len) {
    /* Emit order, which is also the order the two passes below walk in:
     *   1. start-line
     *        request:  "%s %s HTTTP/1.0\r\n"  (method, path)
     *        response: "HTTTP/1.0 %d %s\r\n"  (status, reason)
     *   2. Date header on responses (RFC 1123, via strftime).
     *   3. Content-Length, if there is a body.
     *   4. every builder header as "Name: Value\r\n".
     *   5. the blank "\r\n".
     *   6. body bytes.
     * Buffer is malloc-ed to the exact size worked out in pass 1.
     */
    if (!b) {
        if (out_len) {
            *out_len = 0;
        }
        return NULL;
    }
 
    /* Pass 1: count the bytes. Size first, then fill, so there is exactly one
     * malloc and no realloc. Everything that ends up on the wire has to be
     * counted here or pass 2 writes past the end of the buffer. */
    char start_line[512];
    char date_header[64];
    char cl_header[64];
    size_t start_line_len;
    size_t date_header_len = 0;
    size_t cl_header_len = 0;
 
    /* Start-line */
    if (b->is_request) {
        /* REQUEST-LINE ::= METHOD SP PATH SP "HTTTP/1.0" CRLF */
        start_line_len = (size_t)snprintf(start_line, sizeof(start_line), "%s %s HTTTP/1.0\r\n", htttp_method_str(b->method), b->path ? b->path : "/");
    } else {
        /* STATUS-LINE ::= "HTTTP/1.0" SP STATUS-CODE SP REASON-PHRASE CRLF */
        const char *reason = htttp_status_reason(b->status);
        start_line_len = (size_t)snprintf(start_line, sizeof(start_line), "HTTTP/1.0 %d %s\r\n", (int)b->status, reason);
    }
 
    /* Date goes on every response, RFC 1123, spec says so.
     * Looks like "Date: Tue, 17 Jun 2026 12:34:56 GMT\r\n" */
    if (!b->is_request) {
        time_t now = time(NULL);
        struct tm *gmt = gmtime(&now);
        char date_val[48];
        strftime(date_val, sizeof(date_val), "%a, %d %b %Y %H:%M:%S GMT", gmt);
        date_header_len = (size_t)snprintf(date_header, sizeof(date_header), "Date: %s\r\n", date_val);
    }
 
    /* Content-Length whenever there is a body, so the caller never has to
     * add it by hand and cannot get it wrong. */
    if (b->body && b->body_len > 0) {
        cl_header_len = (size_t)snprintf(cl_header, sizeof(cl_header), "Content-Length: %zu\r\n", b->body_len);
    }
 
    /* Now the caller's own headers. Each one is "Name: Value\r\n". */
    size_t headers_len = 0;
    for (size_t i = 0; i < b->header_count; i++) {
        if (!b->headers[i].name || !b->headers[i].value) {
            continue;
        }
        headers_len += strlen(b->headers[i].name)
                     + 2                              /* ": " */
                     + strlen(b->headers[i].value)
                     + 2;                             /* "\r\n" */
    }
 
    /* Plus the blank line that ends the headers, plus the body. */
    size_t total = start_line_len
                 + date_header_len
                 + cl_header_len
                 + headers_len
                 + 2                  /* blank line "\r\n" */
                 + b->body_len;
 
    /* Pass 2: allocate, then fill. `p` walks the buffer and every write is
     * one that pass 1 counted, so `p` lands exactly on buf_out + total. */
    char *buf_out = malloc(total + 1);   /* +1 for the NUL below */
    if (!buf_out) {
        if (out_len) {
            *out_len = 0;
        }
        return NULL;
    }
 
    char *p = buf_out;
 
    /* Start-line */
    memcpy(p, start_line, start_line_len);
    p += start_line_len;
 
    /* Date header (responses only) */
    if (date_header_len > 0) {
        memcpy(p, date_header, date_header_len);
        p += date_header_len;
    }
 
    /* Content-Length header */
    if (cl_header_len > 0) {
        memcpy(p, cl_header, cl_header_len);
        p += cl_header_len;
    }
 
    /* Caller-supplied headers */
    for (size_t i = 0; i < b->header_count; i++) {
        if (!b->headers[i].name || !b->headers[i].value) {
            continue;
        }
 
        size_t nlen = strlen(b->headers[i].name);
        size_t vlen = strlen(b->headers[i].value);
 
        memcpy(p, b->headers[i].name, nlen);  p += nlen;
        p[0] = ':'; p[1] = ' ';               p += 2;
        memcpy(p, b->headers[i].value, vlen); p += vlen;
        p[0] = '\r'; p[1] = '\n';             p += 2;
    }
 
    /* The blank line. Headers end here. */
    p[0] = '\r'; p[1] = '\n'; p += 2;
 
    /* Body */
    if (b->body && b->body_len > 0) {
        memcpy(p, b->body, b->body_len);
        p += b->body_len;
    }
 
    /* NUL on the end so the buffer is printf-able. Not counted in out_len,
     * the wire never sees it. */
    *p = '\0';
 
    if (out_len) {
        *out_len = total;
    }
    return buf_out;
}

const char* find_crlf(const char* p, const char* end) {
    while (p + 1 < end) {
        if (p[0] == '\r' && p[1] == '\n') {
            return p;
        }
        p++;
    }
    return NULL;
}