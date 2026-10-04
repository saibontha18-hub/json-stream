CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -pedantic -O2

LIB = libjsonstream.a
OBJ = json_stream.o
TOOL = tools/jsev

all: $(LIB) $(TOOL)

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

$(TOOL): tools/jsev.c $(LIB)
	$(CC) $(CFLAGS) -I. -o $@ tools/jsev.c -L. -ljsonstream

test: $(LIB) tests/smoke_test tests/test_json_stream $(TOOL)
	./tests/smoke_test
	./tests/test_json_stream

tests/smoke_test: tests/smoke_test.c $(LIB)
	$(CC) $(CFLAGS) -I. -o $@ tests/smoke_test.c -L. -ljsonstream

tests/test_json_stream: tests/test_json_stream.c $(LIB)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_json_stream.c -L. -ljsonstream

clean:
	rm -f $(OBJ) $(LIB) $(TOOL) tests/smoke_test tests/test_json_stream

.PHONY: all test clean
