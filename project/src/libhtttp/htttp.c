#include <stddef.h>
#include <stdlib.h>
#include <ctype.h>      // isspace: relying on the implicit declaration is an
                        // error on clang and gcc 14+, not just a warning
#include <string.h>
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
 
        /* An empty line signals the end of the header section. */
        if (line_end == *cur) {
            *cur += 2;   /* skip the blank CRLF itself */
            return HTTTP_OK;
        }
 
        /* Guard against too many headers. */
        if (msg->header_count >= HTTTP_MAX_HEADERS) {
            return HTTTP_ERR_MALFORMED;
        }
 
        /* Split the line on the first colon. */
        const char *colon = memchr(*cur, ':', (size_t)(line_end - *cur));
        if (!colon) {
            return HTTTP_ERR_MALFORMED;
        }
 
        /* Name: from cur to colon, trimming trailing whitespace only.
         * Leading whitespace in a header name is illegal (folded headers
         * are not supported in HTTTP/1.0). */
        const char *name_s = *cur;
        const char *name_e = colon;
        while (name_e > name_s && isspace((unsigned char)*(name_e - 1))) {
            name_e--;
        }
 
        if (name_s == name_e) {
            return HTTTP_ERR_MALFORMED;   /* empty name */
        }
 
        /* Value: from after the colon to line_end, trimmed both sides. */
        const char *val_s = colon + 1;
        const char *val_e = line_end;
        trim(&val_s, &val_e);
 
        /* Store zero-copy pointers into the caller's buffer. */
        htttp_header_t *h = &msg->headers[msg->header_count++];
        h->name = name_s;
        h->name_len = (size_t)(name_e - name_s);
        h->value = val_s;
        h->value_len = (size_t)(val_e - val_s);
 
        /* Cache the three well-known headers that the rest of the stack
         * needs without going through htttp_find_header every time.
         * Header names are case-insensitive per spec, so use strncasecmp. */
        if (h->name_len == 14 && strncasecmp(name_s, "Content-Length", 14) == 0) {
 
            /* Parse the value as a non-negative decimal integer.
             * We need a NUL-terminated copy because strtol requires it. */
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
        /* No body: leave msg->body = NULL, msg->body_len = 0. */
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

    /* 1. Find the request-line */
    const char *line_end = find_crlf(cur, end);
    if (!line_end) {
        return HTTTP_ERR_INCOMPLETE;
    }

    /* 2. Split request-line into METHOD / PATH / VERSION */
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
        
        /* trim name trailing space */
        while (name_e > name_s && isspace((unsigned char)*(name_e - 1))) {
            name_e--;
        }

        const char *val_s = colon + 1;
        const char *val_e = line_end;

        /* remove surrounding whitespace */
        trim(&val_s, &val_e);

        /* 3.1 Store in headers array (zero-copy) */
        htttp_header_t *h = &msg->headers[msg->header_count++];
        h->name = name_s;
        h->name_len = (size_t)(name_e - name_s);
        h->value = val_s;
        h->value_len = (size_t)(val_e - val_s);

        /* 3.2 Cache well-known headers */
        if (h->name_len == 14 && strncasecmp(name_s, "Content-Length", 14) == 0) {
            char tmp[32];

            /* Prevents overflow */
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

    /* 4. Body */
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
Rejects null pointers.
Clears all fields in the output structure before parsing.
Creates two pointers: cur = current parser position, and end = one byte past the buffer.
Example:
buf
 |
 v
GET /foo HTTTP/1.0\r\n
...
                    ^
                    end

1. Searches for \r\n which terminates the request line. line_end points to the \r. If CRLF isn't found, the request line isn't complete yet.
2. From the request line, search for the first and second spaces to get METHOD, PATH and VERSION substrings
2.1 Get length of METHOD string, reject empty or overly long strings, copies the string to the variable method_str in the msg struct, and converts the string in the variable method_str to enum using the _method_table
2.2 Get length of PATH string, reject empty or overly long strings, and copy the string to the variable path in the msg struct
2.3 Points at the VERSION string, compute VERSION string length, and accepts only HTTTP/1.0.
Move past \r\n to the first header
3 While headers are not empty line, find next header line. In the header line, find the first occurrence of ':' to get content-length and then extract header name and value
3.1 Get next header slot, then store name and value locations.
3.2 Checks whether header name is 'Content-Length' (case-insensitive), convert value to string, convert tmp from string to long integer, and store parsed length.
3.3 Stores pointer to Content-Type value
3.4 Stores pointer to Player-Id value
4. Only parse body if length is specified: calculate bytes left in buffer, if header says 'Content-Length: 100' but only 50 bytes remain then return HTTTP_ERR_INCOMPLETE, else body points into original buffer and stores body length
*/

htttp_err_t htttp_parse_response(const char *buf, size_t buf_len, htttp_msg_t *msg) {
    if (!buf || !msg) return HTTTP_ERR_MALFORMED;
    _msg_reset(msg);
 
    const char *cur = buf;
    const char *end = buf + buf_len;
 
    /* 1. Locate the status-line */
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
 
    /* 4. Parse headers */
    htttp_err_t err = parse_headers(msg, &cur, end);
    if (err != HTTTP_OK) {
        return err;
    }
 
    /* 5. Point body into buf, validate it fits */
    err = parse_body(msg, cur, end);
    if (err != HTTTP_OK) {
        return err;
    }
 
    msg->is_request = 0;
    return HTTTP_OK;
}

/*
1. Find first CRLF → status-line.
2. Split: version SP status-code SP reason-phrase.
3. Parse status-code as integer → msg->status.
4. Same header loop as request parser.
5. Point body if Content-Length > 0.
*/

const char *htttp_find_header(const htttp_msg_t *msg, const char *name, size_t *value_len) {
    /* Case-insensitive header search */
    if (!msg || !name) {
        if (value_len) {
            *value_len = 0;
        }
        return NULL;
    }
 
    size_t name_len = strlen(name);
 
    /* Linear scan: HTTTP_MAX_HEADERS is 32, so this is O(1) in practice. */
    for (size_t i = 0; i < msg->header_count; i++) {
        const htttp_header_t *h = &msg->headers[i];
 
        /* Must match in length first — avoids false positives where one name is a prefix of another (e.g. "Date" vs "Date-Modified"). */
        if (h->name_len != name_len)
            continue;
 
        /* Case-insensitive compare: "content-length" matches "Content-Length". */
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
    /* Content-Length is emitted automatically by htttp_serialise(). */
}

char *htttp_serialise(const htttp_builder_t *b, size_t *out_len) {
    /* TODO: implement serialiser.
     *
     * Algorithm:
     *   1. Build the start-line:
     *        request:  "%s %s HTTTP/1.0\r\n"  (method, path)
     *        response: "HTTTP/1.0 %d %s\r\n"  (status, reason)
     *   2. For responses, prepend a Date header (RFC 1123 via strftime).
     *   3. If body present, emit "Content-Length: %zu\r\n".
     *   4. Emit all builder headers as "Name: Value\r\n".
     *   5. Emit empty line "\r\n".
     *   6. Append body bytes if any.
     *   7. malloc the final buffer to exact size; fill and return.
     *
     * Current stub: emits nothing, returns NULL.
     */
    if (!b) {
        if (out_len) {
            *out_len = 0;
        }
        return NULL;
    }
 
    /* ---------------------------------------------------------------
     * Pass 1 — calculate the exact byte count we need to malloc.
     *
     * We do two passes (size then fill) so we only call malloc once
     * and never realloc.  Every piece of the wire format is accounted
     * for here before we touch heap memory.
     * --------------------------------------------------------------- */
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
 
    /* Date header: responses always include it (RFC 1123 format).
     * Example: "Date: Tue, 17 Jun 2026 12:34:56 GMT\r\n" */
    if (!b->is_request) {
        time_t now = time(NULL);
        struct tm *gmt = gmtime(&now);
        char date_val[48];
        strftime(date_val, sizeof(date_val), "%a, %d %b %Y %H:%M:%S GMT", gmt);
        date_header_len = (size_t)snprintf(date_header, sizeof(date_header), "Date: %s\r\n", date_val);
    }
 
    /* Content-Length header: emitted whenever body is present. */
    if (b->body && b->body_len > 0) {
        cl_header_len = (size_t)snprintf(cl_header, sizeof(cl_header), "Content-Length: %zu\r\n", b->body_len);
    }
 
    /* Sum up all the caller-supplied headers.
     * Wire format per header: "Name: Value\r\n" */
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
 
    /* Blank line terminating the headers + optional body. */
    size_t total = start_line_len
                 + date_header_len
                 + cl_header_len
                 + headers_len
                 + 2                  /* blank line "\r\n" */
                 + b->body_len;
 
    /* ---------------------------------------------------------------
     * Pass 2 — allocate and fill the output buffer.
     *
     * We use a write cursor `p` that advances through the malloc-ed
     * region.  Every memcpy / strncpy writes exactly as many bytes as
     * we counted above, so `p` reaches `buf + total` at the end.
     * --------------------------------------------------------------- */
    char *buf_out = malloc(total + 1);   /* +1 for a NUL convenience byte */
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
 
    /* Blank line — signals end of headers on the wire */
    p[0] = '\r'; p[1] = '\n'; p += 2;
 
    /* Body */
    if (b->body && b->body_len > 0) {
        memcpy(p, b->body, b->body_len);
        p += b->body_len;
    }
 
    /* NUL-terminate for caller convenience (not counted in out_len). */
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