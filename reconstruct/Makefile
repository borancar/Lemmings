# PC Lemmings, reconstructed.
CC      ?= cc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Wshadow \
           -Wconversion -Wno-sign-conversion
LDFLAGS ?=

# SDL3 is REQUIRED, not optional. The port shows a screen; it is not a bitmap
# converter with a viewer bolted on, and it is not tied to one platform. See
# the note at the top of src/view.c.
SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS   := $(shell pkg-config --libs sdl3)
ifeq ($(strip $(SDL_LIBS)),)
$(error SDL3 not found. Install it (Fedora: sudo dnf install SDL3-devel, \
Debian/Ubuntu: sudo apt install libsdl3-dev, macOS: brew install sdl3) \
and make sure pkg-config can see it.)
endif
CFLAGS  += $(SDL_CFLAGS)
LDFLAGS += $(SDL_LIBS)

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
