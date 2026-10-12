#include "json_stream.h"

#include <string.h>

/* container markers on the nesting stack */
#define C_OBJECT 1
#define C_ARRAY  2

/* internal parser states */
enum {
    ST_VALUE,    /* expecting a value */
    ST_KEY,      /* in object: expecting a key string or '}' */
    ST_COLON,    /* after a key: expecting ':' */
    ST_COMMA,    /* after a value: expecting ',' or a closer */
    ST_TRAILING, /* root value done: only whitespace may follow */
    ST_STRING,   /* inside a "..." token */
    ST_ESCAPED,  /* previous byte was a backslash inside a string */
    ST_UNI,      /* inside a \uXXXX escape: uni_need hex digits to go */
    ST_NUMBER,   /* inside a number token */
    ST_LITERAL   /* inside true/false/null */
};

/* feed_char results */
enum { F_OK = 0, F_REPROCESS = 1, F_ERR = 2 };

static int is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int is_num_char(char c) {
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
           c == 'e' || c == 'E';
}

/* Strict JSON number: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
 * The scanner above is deliberately loose (it just needs to know where
 * the token ends); this decides whether the token is actually a number.
 * No leading zeros, no dangling '.' or 'e'. */
static int number_ok(const char *s, size_t n) {
    size_t i = 0;

    if (i < n && s[i] == '-')
        i++;

    if (i < n && s[i] == '0') {
        i++;
    } else if (i < n && s[i] >= '1' && s[i] <= '9') {
        while (i < n && s[i] >= '0' && s[i] <= '9')
            i++;
    } else {
        return 0;
    }

    if (i < n && s[i] == '.') {
        i++;
        if (i >= n || s[i] < '0' || s[i] > '9')
            return 0;
        while (i < n && s[i] >= '0' && s[i] <= '9')
            i++;
    }

    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-'))
            i++;
        if (i >= n || s[i] < '0' || s[i] > '9')
            return 0;
        while (i < n && s[i] >= '0' && s[i] <= '9')
            i++;
    }

    return i == n;
}

static int fail(json_parser_t *p, json_err_t e) {
    p->err = (int)e;
    p->err_line = p->line;
    p->err_col = p->col;
    return F_ERR;
}

static void emit(json_parser_t *p, json_ev_type_t type, const char *s, size_t n) {
    json_event_t ev;
    ev.type = type;
    ev.str = s;
    ev.len = n;
    p->cb(&ev, p->user);
}

/* A value just completed: either we are done at top level, or we expect
 * a comma/closer inside the enclosing container. */
static void after_value(json_parser_t *p) {
    if (p->depth == 0) {
        p->root_done = 1;
        p->state = ST_TRAILING;
    } else {
        p->state = ST_COMMA;
    }
}

/* Begin tracking a token starting at `start`. Bytes are NOT copied yet —
 * the event will point straight into the caller's buffer unless the token
 * runs past the end of the chunk, in which case we spill to scratch. */
static void tok_begin(json_parser_t *p, const char *start) {
    p->tok_ptr = start;
    p->tok_in_scratch = 0;
    p->scratch_len = 0;
}

/* Chunk ended mid-token: move the bytes seen so far into scratch. */
static int tok_spill(json_parser_t *p, const char *data, size_t i) {
    size_t n = (size_t)((data + i) - p->tok_ptr);
    if (n > sizeof p->scratch)
        return -1;
    memcpy(p->scratch, p->tok_ptr, n);
    p->scratch_len = n;
    p->tok_ptr = NULL;
    p->tok_in_scratch = 1;
    return 0;
}

static int tok_push(json_parser_t *p, char c) {
    if (p->scratch_len >= sizeof p->scratch)
        return -1;
    p->scratch[p->scratch_len++] = c;
    return 0;
}

/* Resolve the token's bytes for the event. */
static const char *tok_end(json_parser_t *p, const char *data, size_t i,
                           size_t *n) {
    if (p->tok_in_scratch) {
        *n = p->scratch_len;
        return p->scratch;
    }
    *n = (size_t)((data + i) - p->tok_ptr);
    return p->tok_ptr;
}

static int open_container(json_parser_t *p, int kind) {
    if (p->depth >= JSON_STREAM_MAX_DEPTH)
        return fail(p, JSON_ERR_DEPTH);
    emit(p, kind == C_OBJECT ? JSON_EV_OBJECT_START : JSON_EV_ARRAY_START,
         NULL, 0);
    p->stack[p->depth++] = (unsigned char)kind;
    p->comma_seen = 0;
    p->state = (kind == C_OBJECT) ? ST_KEY : ST_VALUE;
    return F_OK;
}

static int close_container(json_parser_t *p, int kind) {
    if (p->depth == 0 || p->stack[p->depth - 1] != kind)
        return fail(p, JSON_ERR_UNEXPECTED_CHAR);
    p->depth--;
    emit(p, kind == C_OBJECT ? JSON_EV_OBJECT_END : JSON_EV_ARRAY_END,
         NULL, 0);
    after_value(p);
    return F_OK;
}

/* Handle one byte. F_REPROCESS means "run me again with the same byte" —
 * used when a number ends: the terminator belongs to the next state. */
static int feed_char(json_parser_t *p, const char *data, size_t i) {
    char c = data[i];

    if (p->state == ST_STRING) {
        if (c == '"') {
            size_t n;
            const char *s = tok_end(p, data, i, &n);
            emit(p, p->tok_is_key ? JSON_EV_KEY : JSON_EV_STRING, s, n);
            if (p->tok_is_key)
                p->state = ST_COLON;
            else
                after_value(p);
            return F_OK;
        }
        if (c == '\\') {
            if (p->tok_in_scratch && tok_push(p, c))
                return fail(p, JSON_ERR_TOKEN_TOO_LONG);
            p->state = ST_ESCAPED;
            return F_OK;
        }
        /* Raw control characters are illegal in JSON strings — they
         * have to be written as escapes. */
        if ((unsigned char)c < 0x20)
            return fail(p, JSON_ERR_UNEXPECTED_CHAR);
        if (p->tok_in_scratch && tok_push(p, c))
            return fail(p, JSON_ERR_TOKEN_TOO_LONG);
        return F_OK;
    }

    if (p->state == ST_ESCAPED) {
        if (c == 'u') {
            /* push the 'u' too — the escape bytes live in scratch
             * once the token has spilled across a chunk boundary */
            if (p->tok_in_scratch && tok_push(p, c))
                return fail(p, JSON_ERR_TOKEN_TOO_LONG);
            p->uni_need = 4;
            p->state = ST_UNI;
            return F_OK;
        }
        switch (c) {
        case '"':
        case '\\':
        case '/':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
            break;
        default:
            return fail(p, JSON_ERR_UNEXPECTED_CHAR);
        }
        if (p->tok_in_scratch && tok_push(p, c))
            return fail(p, JSON_ERR_TOKEN_TOO_LONG);
        p->state = ST_STRING;
        return F_OK;
    }

    if (p->state == ST_UNI) {
        int hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F');
        if (!hex)
            return fail(p, JSON_ERR_UNEXPECTED_CHAR);
        if (p->tok_in_scratch && tok_push(p, c))
            return fail(p, JSON_ERR_TOKEN_TOO_LONG);
        if (--p->uni_need == 0)
            p->state = ST_STRING;
        return F_OK;
    }

    if (p->state == ST_NUMBER) {
        if (is_num_char(c)) {
            if (p->tok_in_scratch && tok_push(p, c))
                return fail(p, JSON_ERR_TOKEN_TOO_LONG);
            return F_OK;
        }
        /* Number ends here; c belongs to whatever comes next. */
        {
            size_t n;
            const char *s = tok_end(p, data, i, &n);
            if (!number_ok(s, n))
                return fail(p, JSON_ERR_BAD_NUMBER);
            emit(p, JSON_EV_NUMBER, s, n);
            after_value(p);
        }
        return F_REPROCESS;
    }

    if (p->state == ST_LITERAL) {
        if (c == p->lit_expect[p->lit_matched]) {
            p->lit_matched++;
            if (p->lit_expect[p->lit_matched] == '\0') {
                char t = p->lit_expect[0];
                emit(p, t == 't' ? JSON_EV_TRUE :
                         t == 'f' ? JSON_EV_FALSE : JSON_EV_NULL, NULL, 0);
                after_value(p);
            }
            return F_OK;
        }
        return fail(p, JSON_ERR_BAD_LITERAL);
    }

    switch (p->state) {
    case ST_VALUE:
        if (is_ws(c))
            return F_OK;
        if (c == '{')
            return open_container(p, C_OBJECT);
        if (c == '[')
            return open_container(p, C_ARRAY);
        if (c == ']') { /* "[]" — the empty array closes here */
            if (p->comma_seen)
                return fail(p, JSON_ERR_TRAILING_COMMA);
            return close_container(p, C_ARRAY);
        }
        /* '}' never valid here: "{}" closes from ST_KEY, so this is junk */
        if (c == '"') {
            tok_begin(p, data + i + 1);
            p->tok_is_key = 0;
            p->comma_seen = 0;
            p->state = ST_STRING;
            return F_OK;
        }
        if (c == 't' || c == 'f' || c == 'n') {
            p->lit_expect = c == 't' ? "true" : c == 'f' ? "false" : "null";
            p->lit_matched = 1;
            p->comma_seen = 0;
            p->state = ST_LITERAL;
            return F_OK;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            tok_begin(p, data + i);
            p->comma_seen = 0;
            p->state = ST_NUMBER;
            return F_OK;
        }
        return fail(p, JSON_ERR_UNEXPECTED_CHAR);

    case ST_KEY:
        if (is_ws(c))
            return F_OK;
        if (c == '}') { /* "{}" or end of object */
            if (p->comma_seen)
                return fail(p, JSON_ERR_TRAILING_COMMA);
            return close_container(p, C_OBJECT);
        }
        if (c == '"') {
            tok_begin(p, data + i + 1);
            p->tok_is_key = 1;
            p->comma_seen = 0;
            p->state = ST_STRING;
            return F_OK;
        }
        return fail(p, JSON_ERR_UNEXPECTED_CHAR);

    case ST_COLON:
        if (is_ws(c))
            return F_OK;
        if (c == ':') {
            p->state = ST_VALUE;
            return F_OK;
        }
        return fail(p, JSON_ERR_UNEXPECTED_CHAR);

    case ST_COMMA:
        if (is_ws(c))
            return F_OK;
        if (c == ',') {
            p->comma_seen = 1;
            p->state = (p->stack[p->depth - 1] == C_OBJECT) ? ST_KEY : ST_VALUE;
            return F_OK;
        }
        if (c == '}' || c == ']')
            return close_container(p, c == '}' ? C_OBJECT : C_ARRAY);
        return fail(p, JSON_ERR_UNEXPECTED_CHAR);

    case ST_TRAILING:
        if (is_ws(c))
            return F_OK;
        return fail(p, JSON_ERR_TRAILING_DATA);

    default:
        return fail(p, JSON_ERR_UNEXPECTED_CHAR);
    }
}

void json_parser_init(json_parser_t *p, json_event_cb cb, void *user) {
    memset(p, 0, sizeof *p);
    p->cb = cb;
    p->user = user;
    p->line = 1;
    p->col = 1;
    p->state = ST_VALUE;
}

/* Same clean slate as init, but the callback/user survive — for parsing
 * one document after another with the same parser. */
void json_parser_reset(json_parser_t *p) {
    json_event_cb cb = p->cb;
    void *user = p->user;
    memset(p, 0, sizeof *p);
    p->cb = cb;
    p->user = user;
    p->line = 1;
    p->col = 1;
    p->state = ST_VALUE;
}

json_err_t json_feed(json_parser_t *p, const char *data, size_t len) {
    size_t i;

    if (p->err != JSON_OK)
        return (json_err_t)p->err;

    for (i = 0; i < len; i++) {
        int r;
        for (;;) {
            r = feed_char(p, data, i);
            if (r != F_REPROCESS)
                break;
        }
        if (r == F_ERR)
            return (json_err_t)p->err;
        if (data[i] == '\n') {
            p->line++;
            p->col = 1;
        } else {
            p->col++;
        }
    }

    /* Chunk ended mid-token: park the partial bytes in scratch so the
     * event can point at them when the token completes. Literals need no
     * spill — lit_matched already remembers how far we got. */
    if (len > 0 && !p->tok_in_scratch &&
        (p->state == ST_STRING || p->state == ST_ESCAPED ||
         p->state == ST_UNI || p->state == ST_NUMBER)) {
        if (tok_spill(p, data, len)) {
            p->err = JSON_ERR_TOKEN_TOO_LONG;
            p->err_line = p->line;
            p->err_col = p->col;
            return JSON_ERR_TOKEN_TOO_LONG;
        }
    }

    return JSON_OK;
}

json_err_t json_finish(json_parser_t *p) {
    if (p->err != JSON_OK)
        return (json_err_t)p->err;

    if (p->state == ST_NUMBER) {
        /* A number ran right up to EOF. It must be sitting in scratch:
         * any feed that ended mid-number spilled it there. */
        if (!number_ok(p->scratch, p->scratch_len))
            return fail(p, JSON_ERR_BAD_NUMBER), (json_err_t)p->err;
        emit(p, JSON_EV_NUMBER, p->scratch, p->scratch_len);
        after_value(p);
    } else if (p->state == ST_STRING || p->state == ST_ESCAPED ||
               p->state == ST_UNI || p->state == ST_LITERAL) {
        return fail(p, JSON_ERR_UNEXPECTED_EOF), (json_err_t)p->err;
    }

    if (p->depth != 0 || !p->root_done)
        return fail(p, JSON_ERR_UNEXPECTED_EOF), (json_err_t)p->err;

    return JSON_OK;
}

const char *json_err_str(json_err_t e) {
    switch (e) {
    case JSON_OK:                  return "ok";
    case JSON_ERR_DEPTH:           return "nesting too deep";
    case JSON_ERR_UNEXPECTED_CHAR: return "unexpected character";
    case JSON_ERR_UNEXPECTED_EOF:  return "unexpected end of input";
    case JSON_ERR_TOKEN_TOO_LONG:  return "token exceeds scratch buffer";
    case JSON_ERR_BAD_LITERAL:      return "invalid literal";
    case JSON_ERR_TRAILING_DATA:    return "trailing data after root value";
    case JSON_ERR_BAD_NUMBER:       return "invalid number";
    case JSON_ERR_TRAILING_COMMA:   return "trailing comma";
    default:                       return "unknown error";
    }
}

void json_parser_where(const json_parser_t *p, unsigned *line, unsigned *col) {
    if (p->err != JSON_OK) {
        *line = p->err_line;
        *col = p->err_col;
    } else {
        *line = p->line;
        *col = p->col;
    }
}
