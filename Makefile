# roxy — Redis Layer 7 proxy
# Build: make
# Clean: make clean

CC      ?= cc
CFLAGS  ?= -std=c2x -O2 -Wall -Wextra -D_GNU_SOURCE
LDFLAGS ?= 
INCLUDE := -Iinclude

SRC := \
	src/main.c \
	src/server.c \
	src/dbuf.c \
	src/resp.c  \
	src/hooks.c \
	src/log.c

OBJ := $(SRC:.c=.o)
BIN := roxy

TEST_SRC := src/dbuf_test.c
TEST_BIN := dbuf_test

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

src/%.o: src/%.c include/roxy.h include/log.h include/dbuf.h include/resp.h include/hooks.h
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

$(TEST_BIN): $(TEST_SRC) include/dbuf.h src/dbuf.o
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $(TEST_SRC) src/dbuf.o $(LDFLAGS)

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -f $(OBJ) $(BIN) $(TEST_BIN)
