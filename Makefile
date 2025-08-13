# Makefile

# Configuration (override from CLI if needed, e.g. `make CONFIG=debug`)
CONFIG ?= release

# Compiler and flags
CC      := gcc
CFLAGS.common := -pipe -Wall -Wextra -Wshadow -Wformat=2 -Wcast-qual -Wpointer-arith
CFLAGS.release := -O2 -DNDEBUG
CFLAGS.debug   := -O0 -g3 -ggdb -DDEBUG
# Use GNU C2x for modern C and Linux extensions
CSTD := -std=gnu2x

# Linker flags (add libs if needed, e.g., LDFLAGS += -pthread)
LDFLAGS :=

# Project layout
TARGET := roxy
SRC    := roxy.c
OBJ    := $(SRC:.c=.o)

# Compose flags based on configuration
CFLAGS := $(CSTD) $(CFLAGS.common) $(CFLAGS.$(CONFIG))

.PHONY: all release debug clean run rebuild

all: $(TARGET)

release:
	@$(MAKE) --no-print-directory CONFIG=release

debug:
	@$(MAKE) --no-print-directory CONFIG=debug

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	$(RM) $(OBJ) $(TARGET)

rebuild: clean all
