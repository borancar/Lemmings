# PC Lemmings, reconstructed.
CC      ?= cc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Wshadow \
           -Wconversion -Wno-sign-conversion
LDFLAGS ?=

# X11 is optional: with it, --show opens a window on the level; without it the
# port still renders to a file. Nothing else is needed to build or run.
X11_CFLAGS := $(shell pkg-config --cflags x11 2>/dev/null && echo -DHAVE_X11)
X11_LIBS   := $(shell pkg-config --libs x11 2>/dev/null)
CFLAGS  += $(X11_CFLAGS)
LDFLAGS += $(X11_LIBS)

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := lemmings

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
