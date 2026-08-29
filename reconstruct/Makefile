# PC Lemmings, reconstructed.
CC      ?= cc
CXX     ?= c++
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Wshadow \
           -Wconversion -Wno-sign-conversion
LDFLAGS ?=

# The reconstruction is C. The ONE exception is the OPL2, because ymfm is C++
# - see src/opl.h for why an OPL2 is not transcribed at all, and
# vendor/README.md for what ymfm is. Vendored code is compiled without this
# project's warning set: those warnings exist to catch mistakes in code we
# write, and turning them on someone else's source produces noise nobody may
# act on without editing a file the next update overwrites.
CXXFLAGS ?= -O2 -g -std=c++14
LDFLAGS  += -lstdc++

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

# Two binaries, and the split is the point. `lemmings` is the game: main.c
# mirrors the original, which has no command line. `lemdev` is the developer
# harness - one screen to a file, a forced rating or scroll - and none of it
# corresponds to anything in vgalemmi.exe.
CORE := src/game.c src/data.c src/sdl_io.c src/adlib.c src/harness.c
BIN     := lemmings
DEV_BIN := lemdev

# The OPL2 and the ymfm units it needs. ymfm_opl.cpp implements the whole OPL
# family, so it refers to the ADPCM, PCM and SSG units for the Y8950 and
# YMF278B; this port uses none of them and cannot link without them.
OPL_SRC := src/opl_ymfm.cpp \
           vendor/ymfm/ymfm_opl.cpp vendor/ymfm/ymfm_adpcm.cpp \
           vendor/ymfm/ymfm_pcm.cpp vendor/ymfm/ymfm_ssg.cpp
OPL_OBJ := $(OPL_SRC:.cpp=.o)

all: $(BIN) $(DEV_BIN)

$(BIN): $(CORE:.c=.o) $(OPL_OBJ) src/main.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(DEV_BIN): $(CORE:.c=.o) $(OPL_OBJ) src/devmain.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# The headers are dependencies too. Without this line a change to game.h
# leaves every .o alone, the binary keeps the old constants, and the next
# measurement is of a build that no longer exists in the source. That is the
# stale-artefact trap CLAUDE.md records for capture files, and it applies just
# as well to object files.
HDRS := $(wildcard src/*.h)

src/%.o: src/%.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# THE SHARED LIBRARY tools/ LOADS THROUGH ctypes, and it lives here because
# it kept breaking everywhere else. Eight verifiers each hand-wrote a cc line
# naming the port's sources, so every file the port has gained since broke all
# eight at once and none of them said why: data.c did it, and adlib.c and the
# OPL would have done it next. A file list written out nine times is a file
# list that will differ - the same reason level_play_step exists.
#
# It is not built by `all`: a tool asks for it, and CLAUDE.md's rule about
# checking the build succeeded is why they ask through make rather than
# rebuild it themselves.
LIB     := libgame.so
PIC_OBJ := $(CORE:.c=.pic.o) $(OPL_SRC:.cpp=.pic.o)

src/%.pic.o: src/%.c $(HDRS)
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

%.pic.o: %.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -fPIC -c -o $@ $<

$(LIB): $(PIC_OBJ)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

# The menu's input, checked rather than eyeballed: the test pushes synthetic
# SDL events and asserts what menu_run returns. SDL's dummy video driver means
# it needs no display.
TEST_BIN := tests/menu_input
TEST_OBJ := src/game.o src/data.o src/sdl_io.o src/adlib.o \
            src/harness.o $(OPL_OBJ)

$(TEST_BIN): tests/menu_input.c $(TEST_OBJ)
	$(CC) $(CFLAGS) -Isrc -o $@ $< $(TEST_OBJ) $(LDFLAGS)

# SDL's dummy video driver: the test must never try to open a real window, so
# it runs the same way on a developer's desktop and on a headless machine. The
# test sets the hint itself as well; this is the belt to that pair of braces.
SCREEN_BIN := tests/screens

$(SCREEN_BIN): tests/screens.c $(TEST_OBJ)
	$(CC) $(CFLAGS) -Isrc -o $@ $< $(TEST_OBJ) $(LDFLAGS)

# The OPL2 needs no game data and no display - it is the chip on its own.
OPL_BIN := tests/opl

$(OPL_BIN): tests/opl.c $(OPL_OBJ)
	$(CC) $(CFLAGS) -Isrc -o $@ $< $(OPL_OBJ) -lstdc++ -lm

test: $(TEST_BIN) $(SCREEN_BIN) $(OPL_BIN)
	SDL_VIDEODRIVER=dummy ./$(TEST_BIN)
	SDL_VIDEODRIVER=dummy ./$(SCREEN_BIN) $(DATA)
	./$(OPL_BIN)
	@# Every function must say where it came from. CLAUDE.md has said so all
	@# along and it decayed anyway - 45 functions said nothing - so it is a
	@# test now rather than a preference.
	uv run --project .. python tests/provenance.py

# The same screens under AddressSanitizer and UBSan. The menu loop allocates
# and frees a screen every time round, and nothing else here runs it more than
# once - which is the shape of run that never sees a leak or a double free.
# clang by default: on this machine gcc is installed without libasan, and a
# target that cannot link is a target nobody runs. Override with SAN_CC.
SAN_CC ?= clang
sanitize:
	$(SAN_CC) -O1 -g -std=c11 -Wall -Wextra -Wno-sign-conversion \
	    -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -Isrc -o tests/screens_asan tests/screens.c src/game.c src/data.c \
	    src/sdl_io.c src/adlib.c \
	    $(OPL_SRC) -lstdc++ $(SDL_CFLAGS) $(SDL_LIBS)
	SDL_VIDEODRIVER=dummy ASAN_OPTIONS=detect_leaks=1 \
	    ./tests/screens_asan $(DATA)

clean:
	rm -f src/*.o vendor/ymfm/*.o $(BIN) $(DEV_BIN) $(TEST_BIN) \
	      $(SCREEN_BIN) $(OPL_BIN) tests/screens_asan tests/menu_input_asan \
	      src/*.pic.o vendor/ymfm/*.pic.o $(LIB)

.PHONY: all clean test sanitize
