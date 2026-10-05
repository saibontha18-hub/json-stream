/* Smoke test: core parser over chunked input.
 * The full suite (strict grammar, chunk-boundary round trips) lives in
 * test_json_stream.c. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "json_stream.h"

#define MAXEV 64

static int types[MAXEV];
static char text[MAXEV][128];
static size_t tlen[MAXEV];
static int nev;

static void on_event(const json_event_t *ev, void *user) {
    size_t n;
    (void)user;
    assert(nev < MAXEV);
    types[nev] = (int)ev->type;
    tlen[nev] = ev->len;
    if (ev->str && ev->len) {
        n = ev->len < (sizeof text[0] - 1) ? ev->len : (sizeof text[0] - 1);
        memcpy(text[nev], ev->str, n);
        text[nev][n] = '\0';
    } else {
        text[nev][0] = '\0';
    }
    nev++;
}

/* Feed s in chunks of `step` bytes, exercising chunk-boundary handling. */
static json_err_t feed_chunks(json_parser_t *p, const char *s, size_t step) {
    size_t len = strlen(s), off = 0;
    json_err_t e = JSON_OK;
    while (off < len && e == JSON_OK) {
        size_t n = len - off < step ? len - off : step;
        e = json_feed(p, s + off, n);
        off += n;
    }
    return e;
}

int main(void) {
    json_parser_t p;
    static const int want1[] = {
        JSON_EV_OBJECT_START,
        JSON_EV_KEY, JSON_EV_STRING,
        JSON_EV_KEY, JSON_EV_NUMBER,
        JSON_EV_KEY, JSON_EV_TRUE,
        JSON_EV_KEY, JSON_EV_ARRAY_START,
        JSON_EV_STRING, JSON_EV_STRING,
        JSON_EV_ARRAY_END,
        JSON_EV_KEY, JSON_EV_NULL,
        JSON_EV_OBJECT_END,
    };

    /* 1. mixed document, awkward 7-byte chunks */
    json_parser_init(&p, on_event, NULL);
    nev = 0;
    assert(feed_chunks(&p,
        "{\"name\":\"Ada\",\"n\":42,\"ok\":true,"
        "\"tags\":[\"x\",\"y\"],\"nil\":null}", 7) == JSON_OK);
    assert(json_finish(&p) == JSON_OK);
    assert(nev == 15);
    assert(memcmp(types, want1, sizeof want1) == 0);
    assert(strcmp(text[1], "name") == 0);
    assert(strcmp(text[2], "Ada") == 0);
    assert(strcmp(text[4], "42") == 0);
    assert(strcmp(text[9], "x") == 0 && strcmp(text[10], "y") == 0);

    /* 2. tokens split across chunk boundaries */
    json_parser_init(&p, on_event, NULL);
    nev = 0;
    assert(json_feed(&p, "{\"k\":\"val", 9) == JSON_OK);
    assert(json_feed(&p, "ue\",\"n\":12", 10) == JSON_OK);
    assert(json_feed(&p, "34}", 3) == JSON_OK);
    assert(json_finish(&p) == JSON_OK);
    assert(nev == 6);
    assert(types[0] == JSON_EV_OBJECT_START);
    assert(types[2] == JSON_EV_STRING && strcmp(text[2], "value") == 0);
    assert(types[4] == JSON_EV_NUMBER && strcmp(text[4], "1234") == 0);
    assert(types[5] == JSON_EV_OBJECT_END);

    /* 3. chunk ends right on the backslash inside a string */
    json_parser_init(&p, on_event, NULL);
    nev = 0;
    assert(json_feed(&p, "{\"s\":\"a\\", 8) == JSON_OK);
    assert(json_feed(&p, "nb\"}", 4) == JSON_OK);
    assert(json_finish(&p) == JSON_OK);
    assert(nev == 4);
    assert(types[2] == JSON_EV_STRING);
    assert(tlen[2] == 4 && memcmp(text[2], "a\\nb", 4) == 0); /* raw, for now */

    /* 4. errors */
    json_parser_init(&p, on_event, NULL);
    assert(json_feed(&p, "{\"a\": }", 7) == JSON_ERR_UNEXPECTED_CHAR);

    json_parser_init(&p, on_event, NULL);
    assert(json_feed(&p, "1 ", 2) == JSON_OK);
    assert(json_feed(&p, "2", 1) == JSON_ERR_TRAILING_DATA);

    json_parser_init(&p, on_event, NULL);
    assert(json_feed(&p, "{\"a\":1", 6) == JSON_OK);
    assert(json_finish(&p) == JSON_ERR_UNEXPECTED_EOF);

    json_parser_init(&p, on_event, NULL);
    assert(json_feed(&p, "[1,}", 4) == JSON_ERR_UNEXPECTED_CHAR); /* mismatched */

    puts("smoke: all good");
    return 0;
}
