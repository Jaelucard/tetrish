#ifndef HTTTP_H
#define HTTTP_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define HTTTP_VERSION "HTTTP/1.0"
#define HTTTP_MAX_HEADERS 32
#define HTTTP_MAX_METHOD_LEN 16
#define HTTTP_MAX_PATH_LEN 256
#define HTTTP_MAX_REASON_LEN 64

typedef enum {
    HTTTP_OK =  0,               /* Success                                  */
    HTTTP_ERR_INCOMPLETE  = -1,  /* Buffer ends mid-message, need more data  */
    HTTTP_ERR_MALFORMED = -2,    /* Parse error: bad grammar                 */
    HTTTP_ERR_BAD_VERSION = -3,  /* Version field is not "HTTTP/1.0"         */
    HTTTP_ERR_TOO_LARGE = -4,    /* Content-Length exceeds limit             */
    HTTTP_ERR_NO_MEMORY = -5,    /* malloc failure in serialiser             */
    HTTTP_ERR_BAD_HEADER = -6,   /* A required header is missing or invalid  */
} htttp_err_t;

typedef enum {
    HTTTP_METHOD_UNKNOWN = 0,
    HTTTP_METHOD_JOIN,
    HTTTP_METHOD_LEAVE,
    HTTTP_METHOD_START,
    HTTTP_METHOD_MOVE,
    HTTTP_METHOD_ROTATE,
    HTTTP_METHOD_DROP,
    HTTTP_METHOD_STATE, /* Server-originated broadcast. */
    HTTTP_METHOD_GET,   /* For control-plane polls.     */
    HTTTP_METHOD_HOLD,  /* Appended: swap the active piece with the held one.
                           Last on purpose, so every value above keeps the
                           number it already had and a stale peer that never
                           sends HOLD is unaffected. */
} htttp_method_t;

typedef enum {
    HTTTP_200_OK = 200,
    HTTTP_201_CREATED = 201,
    HTTTP_400_BAD_REQUEST = 400,
    HTTTP_401_UNAUTHORIZED = 401,
    HTTTP_403_FORBIDDEN = 403,
    HTTTP_404_NOT_FOUND = 404,
    HTTTP_409_CONFLICT = 409,
    HTTTP_413_PAYLOAD_TOO_LARGE = 413,
    HTTTP_429_TOO_MANY_REQUESTS = 429,
    HTTTP_500_INTERNAL_ERROR = 500,
} htttp_status_t;

typedef struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} htttp_header_t;

typedef struct {
    int is_request;
    htttp_method_t method;
    char method_str[HTTTP_MAX_METHOD_LEN];
    char path[HTTTP_MAX_PATH_LEN];
    htttp_status_t status;
    char reason[HTTTP_MAX_REASON_LEN];
    htttp_header_t headers[HTTTP_MAX_HEADERS];
    size_t header_count;
    const char *body;           /* Points into parse buffer. May be NULL */
    size_t body_len;            /* 0 if no body */
    const char *player_id;      /* Value of Player-Id header or NULL */
    long content_length;        /* -1 if absent */
    const char *content_type;   /* Value of Content-Type or NULL */
} htttp_msg_t;

typedef struct {
    int is_request;
    htttp_method_t method;
    const char *path;
    htttp_status_t status;
    struct {
        const char *name;
        const char *value;
    } headers[HTTTP_MAX_HEADERS];
    size_t header_count;
    const unsigned char *body;
    size_t body_len;
} htttp_builder_t;

htttp_err_t htttp_parse_request(const char *buf, size_t buf_len, htttp_msg_t *msg);
htttp_err_t htttp_parse_response(const char *buf, size_t buf_len, htttp_msg_t *msg);
const char *htttp_find_header(const htttp_msg_t *msg, const char *name, size_t *value_len);

void htttp_builder_init_request(htttp_builder_t *b, htttp_method_t method, const char *path);
void htttp_builder_init_response(htttp_builder_t *b, htttp_status_t status);
htttp_err_t htttp_builder_add_header(htttp_builder_t *b, const char *name, const char *value);
void htttp_builder_set_body(htttp_builder_t *b, const unsigned char *body, size_t body_len);
char *htttp_serialise(const htttp_builder_t *b, size_t *out_len);

const char *htttp_method_str(htttp_method_t method);
htttp_method_t htttp_method_from_str(const char *s);
const char *htttp_status_reason(htttp_status_t status);
const char *htttp_strerror(htttp_err_t err);
const char* find_crlf(const char* p, const char* end);

#endif

/*
[header pair] picohttpparser: htttp_header_t
>> a single parsed header
>> `name`, `value`: point into the original parse buffer (zero-copy)
>> `name_len`, `value_len`: do NOT include a NUL terminator

[parsed message] picohttpparser: htttp_msg_t
>> Unified structure for both requests and responses
>> Set `is_request = 1` for requests, `is_request = 0` for responses
>> All string pointers reference the caller's parse buffer directly
>> Call htttp_parse_request() or htttp_parse_response() to populate

[builder] htttp_builder_t
>> Accumulates headers for a message being constructed
>> Pass to htttp_serialise() when done

[parse API] picohttpparser: htttp_parse_request
>> Parses a complete HTTTP request from `buf`
>> @param buf     Null-terminated (or length-bounded) message buffer
>> @param buf_len Length of `buf` in bytes
>> @param msg     Output: populated on success
>> Returns: HTTTP_OK on success, negative htttp_err_t on failure
>> Note: string fields in `msg` point into `buf`; keep `buf` alive while using `msg`

[parse API] picohttpparser: htttp_parse_response
>> Parses a complete HTTTP response from `buf`
>> Same ownership rules as htttp_parse_request()

[parse API] picohttpparser: htttp_find_header
>> Searches `msg->headers` for a header whose name matches `name` (case-insensitive)
>> Returns: pointer to the header value string (NOT NUL-terminated, length in *value_len) on success, NULL if not found

[serialise API] htttp_builder_init_request
>> Initialises `b` to build a request with the given method and path
>> `path` must remain valid until htttp_serialise() returns

[serialise API] htttp_builder_init_response
>> Initialises `b` to build a response with the given status
>> The canonical reason phrase is filled in automatically

[serialise API] htttp_builder_add_header
>> Appends a header to `b`
>> `name` and `value` must remain valid until htttp_serialise() returns
>> Returns: HTTTP_OK, or HTTTP_ERR_TOO_LARGE if header limit is reached

[serialise API] htttp_builder_set_body
>> Sets the body. Automatically adds Content-Length (HUH?)
>> `body` must remain valid until htttp_serialise() returns

[serialise API] htttp_serialise
>> Serialises `b` into a freshly malloc-ed buffer in HTTTP/1.0 wire format
>> Automatically prepends a `Date` header (RFC 1123) on responses
>> @param b       Populated builder.
>> @param out_len Set to the buffer length on success.
>> Returns: malloc-ed wire buffer on success and then caller must free(), NULL on allocation failure

[Method Helpers] htttp_method_str
>> Returns: the canonical string for a method (e.g. "JOIN"), or "UNKNOWN"
>> Returned pointer is to a static string; do not free() (THAT'S ALLOWED?!?)

[Method Helpers] htttp_method_from_str
>> Converts a method string to an htttp_method_t
>> Returns: HTTTP_METHOD_UNKNOWN for unrecognised strings

[Method Helpers] htttp_status_reason
>> Returns: the canonical reason phrase for a status code (e.g. 200 → "OK", 409 → "Conflict"), "Unknown" for unrecognised codes

[Method Helpers] htttp_strerror
>> Returns: a human-readable description of an htttp_err_t
*/