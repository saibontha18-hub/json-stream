/* json-stream test suite: strict grammar, error positions, round trips.
 * Self-contained — no test framework, just main() and a tally. */
#include <stdio.h>
#include <string.h>

#include "json_stream.h"

#define MAXEV 256

static int etype[MAXEV];
static char etext[MAXEV][256];
static size_t elen[MAXEV];
static int nev;

static int checks;
static int failures;

#define CHECK(cond)                                                      \
    do {                                                                 \
        checks++;                                                        \
        if (!(cond)) {                                                   \
            failures++;                                                  \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);             \
        }                                                                \
    } while (0)

static void on_event(const json_event_t *ev, void *user) {
    size_t n;
    (void)user;
    if (nev >= MAXEV)
        return;
    etype[nev] = (int)ev->type;
    elen[nev] = ev->len;
    if (ev->str && ev->len) {
        n = ev->len < (sizeof etext[0] - 1) ? ev->len : (sizeof etext[0] - 1);
        memcpy(etext[nev], ev->str, n);
        etext[nev][n] = '\0';
    } else {
        etext[nev][0] = '\0';
    }
    nev++;
}

static void fresh(json_parser_t *p) {
    json_parser_init(p, on_event, NULL);
    nev = 0;
}

/* Feed s in chunks of `step` bytes (step 0 = one shot), then finish. */
static json_err_t parse(json_parser_t *p, const char *s, size_t step) {
    size_t len = strlen(s), off = 0;
    json_err_t e = JSON_OK;
    if (step == 0)
        step = len ? len : 1;
    while (off < len && e == JSON_OK) {
        size_t n = len - off < step ? len - off : step;
        e = json_feed(p, s + off, n);
        off += n;
    }
    if (e == JSON_OK)
        e = json_finish(p);
    return e;
}

/* ---------- strict numbers ---------- */

static void test_numbers(void) {
    static const char *good[] = {
        "0", "-0", "42", "-7", "3.14", "-0.5", "1e10", "1E+5", "2.5e-3",
        "0.1", "123456789012345678901234567890", "-2.99792458e8", NULL
    };
    /* input -> expected error as a bare document */
    static const struct { const char *in; json_err_t err; } bad[] = {
        { "01", JSON_ERR_BAD_NUMBER },
        { "00", JSON_ERR_BAD_NUMBER },
        { "-01", JSON_ERR_BAD_NUMBER },
        { "1.", JSON_ERR_BAD_NUMBER },
        { "1e", JSON_ERR_BAD_NUMBER },
        { "1E+", JSON_ERR_BAD_NUMBER },
        { "1e-", JSON_ERR_BAD_NUMBER },
        { "1.2.3", JSON_ERR_BAD_NUMBER },
        { "--1", JSON_ERR_BAD_NUMBER },
        { "1ee5", JSON_ERR_BAD_NUMBER },
        { "-", JSON_ERR_BAD_NUMBER },
        { ".5", JSON_ERR_UNEXPECTED_CHAR },
        { "+", JSON_ERR_UNEXPECTED_CHAR },
        { "e10", JSON_ERR_UNEXPECTED_CHAR },
        { "NaN", JSON_ERR_UNEXPECTED_CHAR },
        { "0x1", JSON_ERR_TRAILING_DATA },
        { NULL, JSON_OK }
    };
    int i;
    json_parser_t p;

    printf("strict numbers\n");
    for (i = 0; good[i]; i++) {
        fresh(&p);
        CHECK(parse(&p, good[i], 0) == JSON_OK);
        CHECK(nev == 1 && etype[0] == JSON_EV_NUMBER &&
              strcmp(etext[0], good[i]) == 0);
    }
    for (i = 0; bad[i].in; i++) {
        fresh(&p);
        CHECK(parse(&p, bad[i].in, 0) == bad[i].err);
    }
    /* bad number assembled from tiny chunks */
    fresh(&p);
    CHECK(json_feed(&p, "[", 1) == JSON_OK);
    CHECK(json_feed(&p, "0", 1) == JSON_OK);
    CHECK(json_feed(&p, "1]", 2) == JSON_ERR_BAD_NUMBER);
    fresh(&p);
    CHECK(json_feed(&p, "[1", 2) == JSON_OK);
    CHECK(json_feed(&p, "e", 1) == JSON_OK);
    CHECK(json_feed(&p, "]", 1) == JSON_ERR_BAD_NUMBER);
    /* number glued to EOF with no terminator */
    fresh(&p);
    CHECK(json_feed(&p, "12.5e-2", 7) == JSON_OK);
    CHECK(json_finish(&p) == JSON_OK);
    CHECK(nev == 1 && etype[0] == JSON_EV_NUMBER &&
          strcmp(etext[0], "12.5e-2") == 0);
    /* bad number glued to EOF */
    fresh(&p);
    CHECK(json_feed(&p, "01", 2) == JSON_OK);
    CHECK(json_finish(&p) == JSON_ERR_BAD_NUMBER);
    /* good number assembled from tiny chunks */
    fresh(&p);
    CHECK(parse(&p, "[-12.34e+56]", 3) == JSON_OK);
    CHECK(nev == 3 && etype[1] == JSON_EV_NUMBER &&
          strcmp(etext[1], "-12.34e+56") == 0);
}

/* ---------- escapes and strings ---------- */

static void test_strings(void) {
    json_parser_t p;
    unsigned line, col;

    printf("escapes and strings\n");
    fresh(&p);
    CHECK(parse(&p, "\"\\n\\t\\\"\\\\\\/\\b\\f\\r\"", 0) == JSON_OK);
    CHECK(nev == 1 && etype[0] == JSON_EV_STRING && elen[0] == 16);

    fresh(&p);
    CHECK(parse(&p, "\"caf\\u00e9\"", 0) == JSON_OK);
    CHECK(nev == 1 && etype[0] == JSON_EV_STRING &&
          elen[0] == 9 && memcmp(etext[0], "caf\\u00e9", 9) == 0);

    /* \u split across chunks */
    fresh(&p);
    CHECK(json_feed(&p, "\"a\\u00", 6) == JSON_OK);
    CHECK(json_feed(&p, "e9\"", 3) == JSON_OK);
    CHECK(json_finish(&p) == JSON_OK);
    CHECK(nev == 1 && etype[0] == JSON_EV_STRING &&
          memcmp(etext[0], "a\\u00e9", 7) == 0);

    /* bad escapes */
    fresh(&p);
    CHECK(parse(&p, "\"\\x\"", 0) == JSON_ERR_UNEXPECTED_CHAR);
    fresh(&p);
    CHECK(parse(&p, "\"\\u12xz\"", 0) == JSON_ERR_UNEXPECTED_CHAR);
    fresh(&p);
    CHECK(parse(&p, "\"\\u12", 0) == JSON_ERR_UNEXPECTED_EOF);
    fresh(&p);
    CHECK(parse(&p, "\"abc\\", 0) == JSON_ERR_UNEXPECTED_EOF);

    /* raw control characters are illegal, even tab */
    fresh(&p);
    CHECK(parse(&p, "\"a\tb\"", 0) == JSON_ERR_UNEXPECTED_CHAR);
    fresh(&p);
    CHECK(parse(&p, "\"a\x01" "b\"", 0) == JSON_ERR_UNEXPECTED_CHAR);
    json_parser_where(&p, &line, &col);
    CHECK(line == 1 && col == 3);

    /* utf-8 bytes pass straight through */
    fresh(&p);
    CHECK(parse(&p, "\"caf\xc3\xa9\"", 0) == JSON_OK);
    CHECK(nev == 1 && elen[0] == 5);
}

/* ---------- trailing commas ---------- */

static void test_trailing_commas(void) {
    json_parser_t p;

    printf("trailing commas\n");
    fresh(&p);
    CHECK(parse(&p, "[1,]", 0) == JSON_ERR_TRAILING_COMMA);
    fresh(&p);
    CHECK(parse(&p, "{\"a\":1,}", 0) == JSON_ERR_TRAILING_COMMA);
    fresh(&p);
    CHECK(parse(&p, "[[1,],2]", 0) == JSON_ERR_TRAILING_COMMA);
    fresh(&p);
    CHECK(parse(&p, "[1, ]", 0) == JSON_ERR_TRAILING_COMMA);
    fresh(&p);
    CHECK(parse(&p, "{\"a\":1 , }", 0) == JSON_ERR_TRAILING_COMMA);

    /* but these are fine */
    fresh(&p);
    CHECK(parse(&p, "[1,2]", 0) == JSON_OK);
    fresh(&p);
    CHECK(parse(&p, "[]", 0) == JSON_OK);
    fresh(&p);
    CHECK(parse(&p, "{}", 0) == JSON_OK);
    fresh(&p);
    CHECK(parse(&p, "{\"a\":1,\"b\":[2,3]}", 0) == JSON_OK);
}

/* ---------- literals, positions, limits ---------- */

static void test_misc(void) {
    json_parser_t p;
    unsigned line, col;
    char deep[140];
    int i;

    printf("literals, positions, limits\n");
    /* "nul" then EOF */
    fresh(&p);
    CHECK(json_feed(&p, "nul", 3) == JSON_OK);
    CHECK(json_finish(&p) == JSON_ERR_UNEXPECTED_EOF);

    /* "truz": dies on the z, position reported */
    fresh(&p);
    CHECK(json_feed(&p, "{\n  \"a\": truz\n}", 14) == JSON_ERR_BAD_LITERAL);
    json_parser_where(&p, &line, &col);
    CHECK(line == 2 && col == 11);

    /* "truex": literal ends, then x is trailing data */
    fresh(&p);
    CHECK(json_feed(&p, "truex", 5) == JSON_ERR_TRAILING_DATA);

    /* sticky errors keep coming back */
    fresh(&p);
    CHECK(json_feed(&p, "[1,}", 4) == JSON_ERR_UNEXPECTED_CHAR);
    CHECK(json_feed(&p, "more", 4) == JSON_ERR_UNEXPECTED_CHAR);
    CHECK(json_finish(&p) == JSON_ERR_UNEXPECTED_CHAR);

    /* nesting limit: 64 deep is fine, 65 is not */
    for (i = 0; i < 64; i++)
        deep[i] = '[';
    for (i = 0; i < 64; i++)
        deep[64 + i] = ']';
    deep[128] = '\0';
    fresh(&p);
    CHECK(parse(&p, deep, 0) == JSON_OK);
    deep[64] = '['; /* 65 opens now */
    deep[65] = '\0';
    fresh(&p);
    CHECK(parse(&p, deep, 0) == JSON_ERR_DEPTH);

    /* empty and whitespace-only input */
    fresh(&p);
    CHECK(json_finish(&p) == JSON_ERR_UNEXPECTED_EOF);
    fresh(&p);
    CHECK(parse(&p, "  \n\t ", 0) == JSON_ERR_UNEXPECTED_EOF);

    /* root scalars */
    fresh(&p);
    CHECK(parse(&p, "true", 0) == JSON_OK && nev == 1 &&
          etype[0] == JSON_EV_TRUE);
    fresh(&p);
    CHECK(parse(&p, " null ", 0) == JSON_OK && nev == 1 &&
          etype[0] == JSON_EV_NULL);

    /* err strings cover every code */
    for (i = 0; i <= (int)JSON_ERR_TRAILING_COMMA; i++)
        CHECK(json_err_str((json_err_t)i) != NULL);

    /* where() with no error reports the current position */
    fresh(&p);
    CHECK(json_feed(&p, "[1,\n", 4) == JSON_OK);
    json_parser_where(&p, &line, &col);
    CHECK(line == 2 && col == 1);
}

/* ---------- round trips ---------- */

/* Rebuilds canonical JSON (no whitespace) from the event stream. */
typedef struct {
    char buf[4096];
    size_t len;
    int first[JSON_STREAM_MAX_DEPTH + 1];
    unsigned depth;
} emitter_t;

static void em_putc(emitter_t *e, char c) {
    if (e->len < sizeof e->buf - 1)
        e->buf[e->len++] = c;
}

static void em_putn(emitter_t *e, const char *s, size_t n) {
    size_t i;
    for (i = 0; i < n; i++)
        em_putc(e, s[i]);
}

static void em_comma(emitter_t *e) {
    if (!e->first[e->depth])
        em_putc(e, ',');
}

/* Before a value: no comma when it is the first element, none at all
 * right after a key (the colon is already printed), comma otherwise. */
static void em_value(emitter_t *e) {
    if (e->first[e->depth] == 2)
        e->first[e->depth] = 0;
    else
        em_comma(e);
}

static void on_rebuild(const json_event_t *ev, void *user) {
    emitter_t *e = user;
    switch (ev->type) {
    case JSON_EV_OBJECT_START:
        em_value(e);
        em_putc(e, '{');
        e->depth++;
        e->first[e->depth] = 1;
        break;
    case JSON_EV_OBJECT_END:
        e->depth--;
        em_putc(e, '}');
        e->first[e->depth] = 0;
        break;
    case JSON_EV_ARRAY_START:
        em_value(e);
        em_putc(e, '[');
        e->depth++;
        e->first[e->depth] = 1;
        break;
    case JSON_EV_ARRAY_END:
        e->depth--;
        em_putc(e, ']');
        e->first[e->depth] = 0;
        break;
    case JSON_EV_KEY:
        em_comma(e);
        em_putc(e, '"');
        em_putn(e, ev->str, ev->len);
        em_putc(e, '"');
        em_putc(e, ':');
        e->first[e->depth] = 2; /* next value: colon printed, no comma */
        break;
    case JSON_EV_STRING:
        em_value(e);
        em_putc(e, '"');
        em_putn(e, ev->str, ev->len);
        em_putc(e, '"');
        e->first[e->depth] = 0;
        break;
    case JSON_EV_NUMBER:
        em_value(e);
        em_putn(e, ev->str, ev->len);
        e->first[e->depth] = 0;
        break;
    case JSON_EV_TRUE:
        em_value(e);
        em_putn(e, "true", 4);
        e->first[e->depth] = 0;
        break;
    case JSON_EV_FALSE:
        em_value(e);
        em_putn(e, "false", 5);
        e->first[e->depth] = 0;
        break;
    case JSON_EV_NULL:
        em_value(e);
        em_putn(e, "null", 4);
        e->first[e->depth] = 0;
        break;
    }
}

static void test_round_trip(void) {
    static const char *docs[] = {
        "{\"name\":\"Ada\",\"n\":42,\"ok\":true,"
        "\"tags\":[\"x\",\"y\"],\"nil\":null}",
        " [ 1 , 2.5 , -3e2 ] ",
        "{\"a\":{\"b\":[{},[]]},\"c\":\"x\\ny\"}",
        "{\"u\":\"A\\u00e9\",\"n\":-0.25,\"e\":1E-9}",
        "[true,false,null]",
        "\"just a string\"",
        "[[[[\"deep\"]]]]",
        NULL
    };
    static const char *canon[] = {
        "{\"name\":\"Ada\",\"n\":42,\"ok\":true,"
        "\"tags\":[\"x\",\"y\"],\"nil\":null}",
        "[1,2.5,-3e2]",
        "{\"a\":{\"b\":[{},[]]},\"c\":\"x\\ny\"}",
        "{\"u\":\"A\\u00e9\",\"n\":-0.25,\"e\":1E-9}",
        "[true,false,null]",
        "\"just a string\"",
        "[[[[\"deep\"]]]]",
        NULL
    };
    int i;
    size_t step;

    printf("round trips (chunk sizes 1..16)\n");
    for (i = 0; docs[i]; i++) {
        for (step = 1; step <= 16; step++) {
            json_parser_t p;
            emitter_t e;
            memset(&e, 0, sizeof e);
            e.first[0] = 1;
            json_parser_init(&p, on_rebuild, &e);
            if (parse(&p, docs[i], step) != JSON_OK) {
                printf("  FAIL doc %d step %zu: parse error %s\n",
                       i, step, json_err_str(p.err));
                failures++;
                checks++;
                continue;
            }
            e.buf[e.len] = '\0';
            checks++;
            if (strcmp(e.buf, canon[i]) != 0) {
                printf("  FAIL doc %d step %zu:\n    got  %s\n    want %s\n",
                       i, step, e.buf, canon[i]);
                failures++;
            }
        }
    }
}

/* reset() clears sticky errors and half-parsed state but keeps the
 * callback — for parsing one document after another with one parser. */
static void test_reset(void) {
    json_parser_t p;
    json_parser_init(&p, on_event, NULL);

    /* Poison it: trailing garbage leaves a sticky error. */
    nev = 0;
    CHECK(parse(&p, "{\"a\":1} junk", 0) == JSON_ERR_TRAILING_DATA);

    /* Reset, then a fresh document parses clean and events still fire. */
    json_parser_reset(&p);
    nev = 0;
    CHECK(parse(&p, "[1,2]", 0) == JSON_OK);
    CHECK(nev > 0);
    CHECK(etype[0] == JSON_EV_ARRAY_START);

    /* Reset also works mid-document. */
    fresh(&p);
    CHECK(json_feed(&p, "{\"a\":", 5) == JSON_OK);
    json_parser_reset(&p);
    nev = 0;
    CHECK(parse(&p, "true", 0) == JSON_OK);
    CHECK(nev == 1 && etype[0] == JSON_EV_TRUE);
}

int main(void) {
    test_numbers();
    test_strings();
    test_trailing_commas();
    test_misc();
    test_round_trip();
    test_reset();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
