# bookboyadvance — build the GBA ROM, host tools, and wasm core.
#
# Host deps: a C compiler, freetype2 + libjpeg (via pkg-config), and unzip.
# GBA deps:  devkitPro (devkitARM) — override DEVKITPRO if not in /opt.
# wasm dep:  emscripten (emcc on PATH).
#
# The default font is the bundled XCharter (web/assets/*.otf), so the build
# is self-contained. To match the author's original ROMs on macOS, point
# FONT_DIR at a directory of XCharter-{Roman,Italic,Bold}.otf, or leave it
# unset. BOOKS_DIR selects the folder of .epub files (default ./books).

DEVKITPRO ?= /opt/devkitpro
DEVKITARM := $(DEVKITPRO)/devkitARM
TOOLS     := $(DEVKITPRO)/tools/bin
CC        := $(DEVKITARM)/bin/arm-none-eabi-gcc
OBJCOPY   := $(DEVKITARM)/bin/arm-none-eabi-objcopy
GBAFIX    := $(TOOLS)/gbafix

# ---- host-side tools (C) ----
HOSTCC     ?= cc
# Prefer pkg-config; fall back to Homebrew's opt paths if freetype isn't found.
FT_FLAGS   := $(shell pkg-config --cflags --libs freetype2 2>/dev/null || \
	PKG_CONFIG_PATH=/opt/homebrew/opt/freetype/lib/pkgconfig pkg-config --cflags --libs freetype2)
JPEG_FLAGS ?= $(shell pkg-config --cflags --libs libjpeg 2>/dev/null || \
	echo "-I/opt/homebrew/opt/jpeg-turbo/include -L/opt/homebrew/opt/jpeg-turbo/lib -ljpeg")
BB_SRCS    := tools/bookbuild.c tools/bb_font.c tools/bb_epub.c tools/bb_hyphen.c tools/bb_image.c

# ---- GBA rom: code stub + appended data (rompack) ----
CFLAGS  := -mthumb -mthumb-interwork -mcpu=arm7tdmi -O2 -Wall -Wextra
LDFLAGS := -specs=gba.specs -mthumb -mthumb-interwork

DATA_BINS := $(addprefix build/data/,font.bin books.bin library.bin mini.bin title.bin art.bin)
STUB      := out/stub.gba
ROM       := out/bookboyadvance.gba

# Web assets that are build outputs (copied from build/data by `make websync`)
WEB_OUT   := web/assets/stub.gba web/assets/title.bin web/assets/art.bin

all: $(ROM)

build/bookbuild: $(BB_SRCS) tools/bb.h tools/bb_art.h
	@mkdir -p build
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $(BB_SRCS) $(FT_FLAGS) $(JPEG_FLAGS) -lm

build/rompack: tools/rompack.c
	@mkdir -p build
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $<

build/data/%.bin: build/bookbuild
	./build/bookbuild .

build/main.o: src/main.c tools/bb_art.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

$(STUB): build/main.o
	@mkdir -p out
	$(CC) $(LDFLAGS) $< -o out/stub.elf
	$(OBJCOPY) -O binary out/stub.elf $@
	$(GBAFIX) $@ -tBOOKBOY -cBKBY -mBB

$(ROM): $(STUB) build/rompack $(DATA_BINS)
	./build/rompack $(STUB) $@ $(DATA_BINS)

data: build/bookbuild
	./build/bookbuild .

# ---- wasm typesetting core for the web converter ----
WASM_SRCS := tools/bb_wasm.c tools/bb_font.c tools/bb_hyphen.c tools/bb_image.c

web/bookboy_core.js: $(WASM_SRCS) tools/bb_epub.c tools/bookbuild.c tools/bb.h tools/bb_art.h
	@mkdir -p web
	emcc -O2 -Wall -DBB_WASM -I tools $(WASM_SRCS) \
	    -sUSE_FREETYPE=1 -sALLOW_MEMORY_GROWTH=1 \
	    -sMODULARIZE=1 -sEXPORT_NAME=createBookboyCore \
	    -sEXPORTED_RUNTIME_METHODS=FS,HEAPU8,ccall,cwrap \
	    -sEXPORTED_FUNCTIONS=_malloc,_free \
	    -o $@

wasm: web/bookboy_core.js

# Copy build outputs the web converter serves (stub + title + art) so the
# site never ships a stale ROM stub.
websync: $(STUB) $(DATA_BINS)
	cp $(STUB) web/assets/stub.gba
	cp build/data/title.bin web/assets/title.bin
	cp build/data/art.bin web/assets/art.bin

# Node smoke test of the wasm core against the bundled fonts.
test: web/bookboy_core.js
	node web/test_core.cjs

clean:
	rm -rf build/*.o build/bookbuild build/rompack out
	rm -f web/bookboy_core.js web/bookboy_core.wasm

.PHONY: all data wasm websync test clean
.SECONDARY:
