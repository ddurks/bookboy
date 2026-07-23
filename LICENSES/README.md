# Third-party licenses

bookboyadvance bundles or adapts the following third-party works. Each is
redistributed under its own terms, reproduced here as required.

| Component | Where | License |
|-----------|-------|---------|
| **XCharter / Bitstream Charter** fonts | `web/assets/XCharter-*.otf` | Bitstream free-font license (notice-preserving) — see `XCharter-README.txt` |
| **Hyphenation patterns** (`hyph_en_US`) | `tools/data/hyph_en_US.dic` | LGPL / MPL / Apache tri-license — see `hyph_en_US.txt` |
| **stb_image / stb_image_write** | `tools/vendor/stb_*.h` | Public domain or MIT (dual) — see `stb.txt` |
| **Pillow libImaging** (adapted) | resample/composite in `tools/bb_image.c` | HPND — see `Pillow-HPND.txt` |

The bookboyadvance source code and the hand-drawn artwork in `assets/art/`
are covered by the top-level `LICENSE`.

**Not redistributed:** Apple's Charter.ttc (referenced only as an optional
build input on macOS via `FONT_DIR`) is never copied into this repo or any
build output.
