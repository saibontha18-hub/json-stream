CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -pedantic -O2

LIB = libjsonstream.a
OBJ = json_stream.o

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

test: $(LIB) tests/smoke_test
	./tests/smoke_test

tests/smoke_test: tests/smoke_test.c $(LIB)
	$(CC) $(CFLAGS) -I. -o $@ tests/smoke_test.c -L. -ljsonstream

clean:
	rm -f $(OBJ) $(LIB) tests/smoke_test

.PHONY: all test clean
