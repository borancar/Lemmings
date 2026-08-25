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

SRC := src/game.c src/sdl_io.c src/main.c
OBJ := $(SRC:.c=.o)
BIN := lemmings

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# The menu's input, checked rather than eyeballed: the test pushes synthetic
# SDL events and asserts what menu_run returns. SDL's dummy video driver means
# it needs no display.
TEST_BIN := tests/menu_input
TEST_OBJ := src/game.o src/sdl_io.o

$(TEST_BIN): tests/menu_input.c $(TEST_OBJ)
	$(CC) $(CFLAGS) -Isrc -o $@ $< $(TEST_OBJ) $(LDFLAGS)

# SDL's dummy video driver: the test must never try to open a real window, so
# it runs the same way on a developer's desktop and on a headless machine. The
# test sets the hint itself as well; this is the belt to that pair of braces.
test: $(TEST_BIN)
	SDL_VIDEODRIVER=dummy ./$(TEST_BIN) $(DATA)

clean:
	rm -f $(OBJ) $(BIN) $(TEST_BIN)

.PHONY: all clean test
