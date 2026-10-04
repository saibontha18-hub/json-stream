/* jsev — print the event stream of a JSON document.
 *
 * Usage: jsev [file]        (reads stdin when no file is given)
 *
 * A little devtool for eyeballing what the parser sees. Feeds in 64-byte
 * chunks so chunk-boundary behavior gets exercised, not just the grammar.
 */
#include <stdio.h>
#include <stdlib.h>

#include "json_stream.h"

#define CHUNK 64

static void print_quoted(const char *s, size_t n) {
    putchar('"');
    fwrite(s, 1, n, stdout);
    putchar('"');
    putchar('\n');
}

static void on_event(const json_event_t *ev, void *user) {
    int *depth = user;
    int i;

    if (ev->type == JSON_EV_OBJECT_END || ev->type == JSON_EV_ARRAY_END)
        (*depth)--;
    for (i = 0; i < *depth; i++)
        printf("  ");

    switch (ev->type) {
    case JSON_EV_OBJECT_START:
        puts("{");
        (*depth)++;
        break;
    case JSON_EV_OBJECT_END:
        puts("}");
        break;
    case JSON_EV_ARRAY_START:
        puts("[");
        (*depth)++;
        break;
    case JSON_EV_ARRAY_END:
        puts("]");
        break;
    case JSON_EV_KEY:
        printf("key ");
        print_quoted(ev->str, ev->len);
        break;
    case JSON_EV_STRING:
        printf("str ");
        print_quoted(ev->str, ev->len);
        break;
    case JSON_EV_NUMBER:
        printf("num %.*s\n", (int)ev->len, ev->str);
        break;
    case JSON_EV_TRUE:
        puts("true");
        break;
    case JSON_EV_FALSE:
        puts("false");
        break;
    case JSON_EV_NULL:
        puts("null");
        break;
    }
}

static int fail_at(json_parser_t *p, json_err_t e) {
    unsigned line, col;
    json_parser_where(p, &line, &col);
    fprintf(stderr, "jsev: error at %u:%u: %s\n", line, col, json_err_str(e));
    return 1;
}

int main(int argc, char **argv) {
    FILE *f = stdin;
    json_parser_t p;
    int depth = 0;
    char buf[CHUNK];
    size_t n;
    json_err_t e;

    if (argc > 2) {
        fprintf(stderr, "usage: jsev [file]\n");
        return 2;
    }
    if (argc == 2) {
        f = fopen(argv[1], "r");
        if (!f) {
            fprintf(stderr, "jsev: cannot open %s\n", argv[1]);
            return 2;
        }
    }

    json_parser_init(&p, on_event, &depth);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        e = json_feed(&p, buf, n);
        if (e != JSON_OK) {
            if (f != stdin)
                fclose(f);
            return fail_at(&p, e);
        }
    }
    if (f != stdin)
        fclose(f);

    e = json_finish(&p);
    if (e != JSON_OK)
        return fail_at(&p, e);
    return 0;
}
