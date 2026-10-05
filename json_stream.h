#ifndef JSON_STREAM_H
#define JSON_STREAM_H

/* json_stream — a small SAX-style JSON parser in C11.
 *
 * Feed it bytes in whatever chunks you have (off a socket, a file read
 * loop, ...) and it calls you back with parse events. No malloc anywhere:
 * the parser itself is a caller-owned struct, and string/number payloads
 * point straight into your input buffer — no copies in the common case.
 *
 * The one wrinkle: if a token straddles a chunk boundary it gets assembled
 * in a small fixed scratch buffer inside the parser, and the event then
 * points there instead. Either way, treat the pointer as borrowed: it is
 * only good until the next event fires, so copy out anything you keep.
 *
 * Errors are sticky — once feed/finish reports one, the parser keeps
 * reporting it until you re-init. Line/column of the failure are kept
 * for diagnostics via json_parser_where().
 */

#include <stddef.h>

#define JSON_STREAM_MAX_DEPTH 64
#define JSON_STREAM_SCRATCH   1024

typedef enum {
    JSON_EV_OBJECT_START,
    JSON_EV_OBJECT_END,
    JSON_EV_ARRAY_START,
    JSON_EV_ARRAY_END,
    JSON_EV_KEY,    /* str/len = the key text (raw, escapes not decoded) */
    JSON_EV_STRING, /* str/len = raw string content, no quotes */
    JSON_EV_NUMBER, /* str/len = raw number text; caller converts */
    JSON_EV_TRUE,
    JSON_EV_FALSE,
    JSON_EV_NULL
} json_ev_type_t;

typedef struct {
    json_ev_type_t type;
    const char *str; /* borrowed; valid for KEY/STRING/NUMBER only */
    size_t len;
} json_event_t;

typedef enum {
    JSON_OK = 0,
    JSON_ERR_DEPTH,          /* nesting deeper than JSON_STREAM_MAX_DEPTH */
    JSON_ERR_UNEXPECTED_CHAR, /* char that fits nowhere in the grammar */
    JSON_ERR_UNEXPECTED_EOF,  /* input ended mid-value/container */
    JSON_ERR_TOKEN_TOO_LONG,  /* token bigger than the scratch buffer */
    JSON_ERR_BAD_LITERAL,      /* "truz" and friends */
    JSON_ERR_TRAILING_DATA,    /* non-whitespace after the root value */
    JSON_ERR_BAD_NUMBER,       /* number that breaks the JSON grammar */
    JSON_ERR_TRAILING_COMMA    /* "[1,]" and friends */
} json_err_t;

struct json_parser;
typedef void (*json_event_cb)(const json_event_t *ev, void *user);

/* Treat every field below the callback as private. */
typedef struct json_parser {
    json_event_cb cb;
    void *user;

    unsigned char stack[JSON_STREAM_MAX_DEPTH]; /* 1 = object, 2 = array */
    unsigned depth;

    int state; /* internal state machine */
    int err;   /* sticky json_err_t */

    unsigned line, col;         /* 1-based position of next byte */
    unsigned err_line, err_col; /* where the sticky error happened */

    int root_done; /* root value complete; only whitespace may follow */

    /* in-progress token (string or number) */
    const char *tok_ptr; /* start of token bytes, or NULL once spilled */
    int tok_in_scratch;
    int tok_is_key;      /* string token sits in key position */
    char scratch[JSON_STREAM_SCRATCH];
    size_t scratch_len;

    /* in-progress literal (true/false/null) */
    const char *lit_expect;
    size_t lit_matched;

    /* \u escape in progress: hex digits still needed */
    unsigned uni_need;

    /* a ',' was just consumed; a closer here means a trailing comma */
    int comma_seen;
} json_parser_t;

void json_parser_init(json_parser_t *p, json_event_cb cb, void *user);

/* Feed a chunk. Returns JSON_OK, or the (sticky) error. May emit events,
 * including the completion of a token that started in an earlier chunk.
 *
 * Preconditions (caller bugs, not checked): p is a valid parser, and data
 * is non-NULL when len > 0. json_feed(p, NULL, 0) is a harmless no-op. A
 * NULL callback is allowed — the input still parses, events are just
 * dropped. */
json_err_t json_feed(json_parser_t *p, const char *data, size_t len);

/* Call when no more input is coming. Completes a number that ran right up
 * to EOF; anything else left open is JSON_ERR_UNEXPECTED_EOF. */
json_err_t json_finish(json_parser_t *p);

const char *json_err_str(json_err_t e);

/* Line/column of the sticky error, or of the current position if no error. */
void json_parser_where(const json_parser_t *p, unsigned *line, unsigned *col);

#endif
