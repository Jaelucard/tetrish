#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>
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

const char* find_crlf(const char* p, const char* end) {
    while (p + 1 < end) {
        if (p[0] == '\r' && p[1] == '\n') {
            return p;
        }
        p++;
    }
    return NULL;
}

static htttp_err_t parse_headers(htttp_msg_t *msg, const char **cur, const char *end) {
    while (1) {
        // Find first occurrence of CRLF between *cur and end.
        const char *line_end = find_crlf(*cur, end);
        if (!line_end) {
            return HTTTP_ERR_INCOMPLETE;
        }
        if (line_end == *cur) {
            *cur += 2;
            return HTTTP_OK;
        }
        if (msg->header_count >= HTTTP_MAX_HEADERS) {
            return HTTTP_ERR_MALFORMED;
        }
 
        // Search the next (size_t)(line_end - *cur) bytes of memory starting at *cur for the first occurrence of ':'.
        const char *colon = memchr(*cur, ':', (size_t)(line_end - *cur));
        if (!colon) {
            return HTTTP_ERR_MALFORMED;
        }
 
        // Get start and end pointers for header name, and trim whitespace from the end.
        // If the name is empty, return an error.
        const char *name_s = *cur;
        const char *name_e = colon;
        while (name_e > name_s && isspace((unsigned char)*(name_e - 1))) {
            name_e--;
        }
        if (name_s == name_e) {
            return HTTTP_ERR_MALFORMED;
        }
 
        // Get start and end pointers for header value, and trim whitespace from both ends.
        const char *val_s = colon + 1;
        const char *val_e = line_end;
        trim(&val_s, &val_e);
 
        // Store zero-copy pointers into the caller's buffer.
        htttp_header_t *h = &msg->headers[msg->header_count++];
        h->name = name_s;
        h->name_len = (size_t)(name_e - name_s);
        h->value = val_s;
        h->value_len = (size_t)(val_e - val_s);
 
        // Cache the three headers that the rest of the stack needs without going through htttp_find_header every time.
        // How? By filling up the three extra fields in htttp_msg_t: content_length, content_type, and player_id. (yes I wrote this by myself HAHAHA)
        // Header 1: Content-Length
        if (h->name_len == 14 && strncasecmp(name_s, "Content-Length", 14) == 0) {
            // Check if header value can fit into temporary buffer.
            char tmp[32];
            if (h->value_len >= sizeof(tmp)) {
                return HTTTP_ERR_MALFORMED;
            }

            // Copy header value into temporary buffer and null-terminate it.
            memcpy(tmp, val_s, h->value_len);
            tmp[h->value_len] = '\0';

            // Convert header value to long and check for errors.
            char *ep;
            long n = strtol(tmp, &ep, 10);
            if (*ep != '\0' || n < 0) {
                return HTTTP_ERR_MALFORMED;
            }

            msg->content_length = n;
        }

        // Header 2: Content-Type (points into caller's buf)
        else if (h->name_len == 12 && strncasecmp(name_s, "Content-Type", 12) == 0) {
            msg->content_type = val_s;
        }

        // Header 3: Player-Id (points into caller's buf)
        else if (h->name_len == 9 && strncasecmp(name_s, "Player-Id", 9) == 0) {
            msg->player_id = val_s;
        }

        *cur = line_end + 2;
    }
}

static htttp_err_t parse_body(htttp_msg_t *msg, const char *cur, const char *end) {
    // No body: leave msg->body = NULL and msg->body_len = 0.
    if (msg->content_length <= 0) {
        if (cur < end) {
            // Trailing bytes with no/zero Content-Length.
            return HTTTP_ERR_BAD_HEADER;
        }
        return HTTTP_OK;
    }

    // Calculates how many bytes are left between the start of the body and the end of the receiving data.
    size_t remaining = (size_t)(end - cur);

    // Incomplete HTTTP message.
    if (remaining < (size_t)msg->content_length) {
        return HTTTP_ERR_INCOMPLETE;
    }

    if (remaining > (size_t)msg->content_length) {
        return HTTTP_ERR_MALFORMED;
    }

    // Note: remaining can be more than (size_t)msg->content_length, but msg->body_len will only save the first (size_t)msg->content_length bytes that belong to the current message's body
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
    {HTTTP_METHOD_ADMIN_STATUS, "ADMIN-STATUS"},
    {HTTTP_METHOD_ADMIN_ROOMS, "ADMIN-ROOMS"},
    {HTTTP_METHOD_ADMIN_ATTACH, "ADMIN-ATTACH"},
    {HTTTP_METHOD_ADMIN_KICK, "ADMIN-KICK"},
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

    // Parsing Step 1: Search for the 1st occurrence of \r\n which terminates the request line.
    const char *line_end = find_crlf(cur, end);
    if (!line_end) {
        return HTTTP_ERR_INCOMPLETE;
    }

    // Parsing Step 2: From the request line, search fro the first and second spaces to get METHOD, PATH, and VERSION substrings.
    const char *sp1 = memchr(cur, ' ', (size_t)(line_end - cur));
    if (!sp1) {
        return HTTTP_ERR_MALFORMED;
    }
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(line_end - (sp1 + 1)));
    if (!sp2) {
        return HTTTP_ERR_MALFORMED;
    }

    // Parsing Step 2.1: METHOD
    size_t method_len = (size_t)(sp1 - cur);
    if (method_len == 0 || method_len >= HTTTP_MAX_METHOD_LEN) {
        return HTTTP_ERR_MALFORMED;
    }
    memcpy(msg->method_str, cur, method_len);
    msg->method_str[method_len] = '\0';
    msg->method = htttp_method_from_str(msg->method_str);

    // Parsing Step 2.2: PATH
    size_t path_len = (size_t)(sp2 - (sp1 + 1));
    if (path_len == 0 || path_len >= HTTTP_MAX_PATH_LEN) {
        return HTTTP_ERR_MALFORMED;
    }
    memcpy(msg->path, sp1 + 1, path_len);
    msg->path[path_len] = '\0';

    // Parsing Step 2.3: VERSION
    const char *ver = sp2 + 1;
    size_t ver_len = (size_t)(line_end - ver);
    if (ver_len != (sizeof(HTTTP_VERSION) - 1) || memcmp(ver, HTTTP_VERSION, ver_len) != 0) {
        return HTTTP_ERR_BAD_VERSION;
    }

    cur = line_end + 2;

    // Parsing Step 3: Headers
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
        
        while (name_e > name_s && isspace((unsigned char)*(name_e - 1))) {
            name_e--;
        }

        const char *val_s = colon + 1;
        const char *val_e = line_end;

        trim(&val_s, &val_e);

        // Parsing Step 3.1: Store in headers array (zero-copy)
        htttp_header_t *h = &msg->headers[msg->header_count++];
        h->name = name_s;
        h->name_len = (size_t)(name_e - name_s);
        h->value = val_s;
        h->value_len = (size_t)(val_e - val_s);

        // Parsing Step 3.2: Cache Content-Length header
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

        // Parsing Step 3.3: Cache Content-Type header
        else if (h->name_len == 12 && strncasecmp(name_s, "Content-Type", 12) == 0) {
            msg->content_type = val_s;   /* points into buf */
        }

        // Parsing Step 3.4: Cache Player-Id header
        else if (h->name_len == 9 && strncasecmp(name_s, "Player-Id", 9) == 0) {
            msg->player_id = val_s;      /* points into buf */
        }

        cur = line_end + 2;
    }

    // Parsing Step 4: Parse body if Content-Length is at least zero.s
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

htttp_err_t htttp_parse_response(const char *buf, size_t buf_len, htttp_msg_t *msg) {
    if (!buf || !msg) {
        return HTTTP_ERR_MALFORMED;
    }
    
    _msg_reset(msg);
 
    const char *cur = buf;
    const char *end = buf + buf_len;
 
    // Parsing Step 1: Search for the 1st occurrence of \r\n which terminates the status line.
    const char *line_end = find_crlf(cur, end);
    if (!line_end) {
        return HTTTP_ERR_INCOMPLETE;
    }
 
    // Parsing Step 2: From the status line, search for the first and second spaces to get VERSION, STATUS-CODE, and REASON-PHRASE substrings.
    const char *sp1 = memchr(cur, ' ', (size_t)(line_end - cur));
    if (!sp1) {
        return HTTTP_ERR_MALFORMED;
    }
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(line_end - (sp1 + 1)));
    if (!sp2) {
        return HTTTP_ERR_MALFORMED;
    }
 
    // Parsing Step 2.1: VERSION
    size_t ver_len = (size_t)(sp1 - cur);
    if (ver_len != (sizeof(HTTTP_VERSION) - 1) || memcmp(cur, HTTTP_VERSION, ver_len) != 0) {
        return HTTTP_ERR_BAD_VERSION;
    }
 
    // Parsing Step 2.2: STATUS-CODE
    size_t code_len = (size_t)(sp2 - (sp1 + 1));
    if (code_len == 0 || code_len > 3) {
        return HTTTP_ERR_MALFORMED;
    }
    char code_buf[4];
    memcpy(code_buf, sp1 + 1, code_len);
    code_buf[code_len] = '\0';
    char *ep;
    long code = strtol(code_buf, &ep, 10);
    if (*ep != '\0' || code < 100 || code > 599) {
        return HTTTP_ERR_MALFORMED;
    }
    msg->status = (htttp_status_t)code;
 
    // Parsing Step 2.3: REASON-PHRASE
    size_t reason_len = (size_t)(line_end - (sp2 + 1));
    if (reason_len >= HTTTP_MAX_REASON_LEN) {
        return HTTTP_ERR_MALFORMED;
    }
    memcpy(msg->reason, sp2 + 1, reason_len);
    msg->reason[reason_len] = '\0';
 
    cur = line_end + 2;
 
    // Parsing Step 3: Headers
    htttp_err_t err = parse_headers(msg, &cur, end);
    if (err != HTTTP_OK) {
        return err;
    }
 
    // Parsing Step 4: Body
    err = parse_body(msg, cur, end);
    if (err != HTTTP_OK) {
        return err;
    }
 
    msg->is_request = 0;
    return HTTTP_OK;
}

const char *htttp_find_header(const htttp_msg_t *msg, const char *name, size_t *value_len) {
    if (!msg || !name) {
        if (value_len) {
            *value_len = 0;
        }
        return NULL;
    }

    size_t name_len = strlen(name);
 
    // Linear Check for Header
    for (size_t i = 0; i < msg->header_count; i++) {
        const htttp_header_t *h = &msg->headers[i];
 
        // Check 1: match in length
        if (h->name_len != name_len)
            continue;
 
        // Check 2: case-insensitive string comparison
        if (strncasecmp(h->name, name, name_len) == 0) {
            if (value_len) *value_len = h->value_len;
            return h->value;
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
}

char *htttp_serialise(const htttp_builder_t *b, size_t *out_len) {
    if (!b) {
        if (out_len) {
            *out_len = 0;
        }
        return NULL;
    }
 
    // Pass 1 - calculate the exact byte count needed to malloc.
    // P1 Step 1: Create temporary storage
    char start_line[512];
    char date_header[64];
    char cl_header[64];
    size_t start_line_len;
    size_t date_header_len = 0;
    size_t cl_header_len = 0;
 
    // P1 Step 2: Start line
    if (b->is_request) {
        /* REQUEST-LINE ::= METHOD SP PATH SP "HTTTP/1.0" CRLF */
        start_line_len = (size_t)snprintf(start_line, sizeof(start_line), "%s %s HTTTP/1.0\r\n", htttp_method_str(b->method), b->path ? b->path : "/");
    } else {
        /* STATUS-LINE ::= "HTTTP/1.0" SP STATUS-CODE SP REASON-PHRASE CRLF */
        const char *reason = htttp_status_reason(b->status);
        start_line_len = (size_t)snprintf(start_line, sizeof(start_line), "HTTTP/1.0 %d %s\r\n", (int)b->status, reason);
    }
 
    // P1 Step 3: Date header
    if (!b->is_request) {
        time_t now = time(NULL);
        struct tm *gmt = gmtime(&now);
        char date_val[48];
        strftime(date_val, sizeof(date_val), "%a, %d %b %Y %H:%M:%S GMT", gmt);
        date_header_len = (size_t)snprintf(date_header, sizeof(date_header), "Date: %s\r\n", date_val);
    }
 
    // P1 Step 4: Content-Length header
    if (b->body && b->body_len > 0) {
        cl_header_len = (size_t)snprintf(cl_header, sizeof(cl_header), "Content-Length: %zu\r\n", b->body_len);
    }
 
    // P1 Step 5: Sum up all the caller-supplied headers.
    size_t headers_len = 0;
    for (size_t i = 0; i < b->header_count; i++) {
        if (!b->headers[i].name || !b->headers[i].value) {
            continue;
        }
        headers_len += strlen(b->headers[i].name) + 2 + strlen(b->headers[i].value) + 2;
    }
 
    // P1 Step 6: Total sum of exactly how many bytes the finished HTTTP message will occupy.
    size_t total = start_line_len + date_header_len + cl_header_len + headers_len + 2 + b->body_len;
 
    // Pass 2 - allocate and fill the output buffer.
    char *buf_out = malloc(total + 1);
    if (!buf_out) {
        if (out_len) {
            *out_len = 0;
        }
        return NULL;
    }
 
    char *p = buf_out;
 
    // P2 Step 1: Copy Start line to p
    memcpy(p, start_line, start_line_len);
    p += start_line_len;
 
    // P2 Step 2: Copy Date header to p
    if (date_header_len > 0) {
        memcpy(p, date_header, date_header_len);
        p += date_header_len;
    }
 
    // P2 Step 3: Copy Content-Length header to p
    if (cl_header_len > 0) {
        memcpy(p, cl_header, cl_header_len);
        p += cl_header_len;
    }
 
    // P2 Step 4: Copy Caller-supplied headers to p
    for (size_t i = 0; i < b->header_count; i++) {
        if (!b->headers[i].name || !b->headers[i].value) {
            continue;
        }

        size_t nlen = strlen(b->headers[i].name);
        size_t vlen = strlen(b->headers[i].value);

        // Name
        memcpy(p, b->headers[i].name, nlen);
        p += nlen;

        // Name: 
        p[0] = ':'; p[1] = ' ';
        p += 2;

        // Name: Value
        memcpy(p, b->headers[i].value, vlen);
        p += vlen;

        // Name: Value\r\n
        p[0] = '\r'; p[1] = '\n';
        p += 2;
    }
 
    // P2 Step 5: Add blank line to p 
    p[0] = '\r'; p[1] = '\n';
    p += 2;
 
    // P2 Step 6: Copy body to p
    if (b->body && b->body_len > 0) {
        memcpy(p, b->body, b->body_len);
        p += b->body_len;
    }
 
    // P2 Step 6: Add NULL terminater for caller convenience, not counted in out_len.
    *p = '\0';
 
    if (out_len) {
        *out_len = total;
    }
    
    return buf_out;
}