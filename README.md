# bookboyadvance

Turn EPUBs into a paperback-style e-reader ROM for the Game Boy Advance.

Text is typeset ahead of time — justified [XCharter](https://ctan.org/pkg/xcharter)
(a free Bitstream Charter) with real kerning and Liang hyphenation, chapters,
per-book bookmarks, and four reading palettes — then packed into a `.gba` ROM
that runs on a flashcart or emulator. A hand-drawn library screen lets you
browse a stack of books, with an animated pair of hands that pick your
selection off the shelf.

There are two ways to make a ROM:

- **In your browser** at **[bookboy.drawvid.com](https://bookboy.drawvid.com)** —
  drop in EPUBs, get a `.gba`. Nothing is uploaded; the same C typesetting
  core runs locally as WebAssembly.
- **From this repo** with `make`, for the full multi-book library build.

## How it works

The core idea is one deterministic, fixed-point typesetting engine whose
output is replayed identically on three targets: the host CLI, WebAssembly,
and the ARM7 GBA runtime. Layout is computed once (glyph positions in 12.4
fixed point, snapped once per glyph), so the device just replays line records
— no layout on a 16 MHz CPU.

```
EPUB ──parse──► paragraphs ──typeset──► binary blobs ──pack──► .gba
      (bb_epub.c / DOMParser)  (bb_font, bb_image,   (rompack.c / JS)
                                bookbuild layout+DP)
```

- `tools/` — the host C toolchain: EPUB parsing (`bb_epub.c`), FreeType
  rasterization + kerning (`bb_font.c`), image/cover work (`bb_image.c`),
  hyphenation (`bb_hyphen.c`), and layout/pagination/emission (`bookbuild.c`).
  `bb_wasm.c` compiles the same core to WebAssembly; `rompack.c` concatenates
  a prebuilt code stub with the data blobs into a ROM.
- `src/main.c` — the entire GBA runtime (mode-4 double-buffered, IWRAM
  blitters, VBlank-halt idle, SRAM bookmarks).
- `web/` — the zero-dependency converter site (`app.js` reads the EPUB zip
  with `DecompressionStream` and `DOMParser`, then drives the wasm core).
- `assets/art/` — the hand-drawn PNGs; their measured geometry lives in
  `tools/bb_art.h`.

## Building

**Dependencies**

- A host C compiler, `freetype2` and `libjpeg` (found via `pkg-config`), and
  `unzip`.
- [devkitPro / devkitARM](https://devkitpro.org) for the GBA stub. Override
  its location with `DEVKITPRO=...` if not in `/opt/devkitpro`.
- [Emscripten](https://emscripten.org) (`emcc` on `PATH`) for the wasm core.

**Make targets**

```sh
make            # build out/bookboyadvance.gba (stub + typeset data)
make data       # just regenerate the data blobs
make wasm       # build the WebAssembly core (web/bookboy_core.{js,wasm})
make websync    # copy the stub/title/art the web converter serves
make test       # node smoke test of the wasm core
```

Put your `.epub` files in `./books` (or set `BOOKS_DIR`). The builder converts
every EPUB it can, then packs as many as fit in 32 MB, smallest-first.

**Environment variables**

| Var | Default | Meaning |
|-----|---------|---------|
| `BOOKS_DIR` | `books` | folder of `.epub` files |
| `FONT_DIR` | `web/assets` | dir with `XCharter-{Roman,Italic,Bold}.otf` |
| `FONT_PX` | `14` | body text size in pixels |
| `FONT_GAMMA` | `0.72` | antialiasing gamma lift |
| `BB_FORCE` | — | comma-separated title substrings to keep first when the shelf overflows 32 MB |
| `BB_PREVIEW` | — | if set, render sample page PNGs into `build/preview/` |
| `BB_DEBUG` | — | if set, trace EPUB parsing |

> The default font is the bundled, freely-licensed XCharter. The project was
> originally developed against Apple's Charter.ttc on macOS; that font is
> never redistributed here, so builds use XCharter unless you point `FONT_DIR`
> elsewhere. XCharter is the same Matthew Carter design and kerns slightly
> more richly, so output differs marginally from an Apple-Charter build.

## Putting a ROM on a Game Boy

You need a **flashcart** — a cartridge with a microSD slot. Copy the `.gba`
onto the card (FAT32, any folder) and pick it from the cart menu. Reading
position saves automatically. In the reader: `A`/`→`/`R` page forward, `←`/`L`
back, `↑`/`↓` jump chapters, `SELECT` cycles palettes, `B` returns to the
library.

Current flashcart landscape (2026): the **EZ-Flash Air** (~$40) is the budget
pick; the **EverDrive GBA Mini** (~$99) is the premium drag-and-drop option.
Buy from the makers' listed resellers to avoid clones. Any GBA plays it — the
backlit SP (AGS-101) has the best screen; the frontlit SP (AGS-001) pairs well
with the high-contrast palette. No hardware? [mGBA](https://mgba.io) (desktop),
Delta (iOS), or Lemuroid (Android).

## Deploying the web converter

The site is fully static. Build and sync, then push `web/` to any static host
(the reference deploy is S3 + CloudFront):

```sh
make wasm && make websync
# then upload web/ ; serve *.wasm with Content-Type: application/wasm
```

## Licensing

- **Code** — MIT (`LICENSE`).
- **Artwork** in `assets/art/` — CC BY 4.0 (`assets/art/LICENSE`).
- **Bundled third-party** components (XCharter fonts, hyphenation patterns,
  stb, adapted Pillow resample code) retain their own licenses — see
  `LICENSES/`.
