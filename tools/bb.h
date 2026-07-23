/* bookboyadvance builder — shared declarations.
 *
 * Typesetting uses fp4 (12.4 fixed point) pen accumulation with a single
 * (+8)>>4 snap per glyph, unhinted fractional advances, and Knuth-style DP
 * line breaking with Liang hyphenation. The GBA runtime replays the same
 * integer semantics, so host previews are pixel-exact.
 */
#ifndef BB_H
#define BB_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;

#define SCREEN_W 240
#define SCREEN_H 160
#define MARGIN_L 8
#define MARGIN_R 8
#define MARGIN_T 4
#define FOOTER_H 11
#define FOOTER_BASE (SCREEN_H - 3)
#define TEXT_W (SCREEN_W - MARGIN_L - MARGIN_R)
#define HYPHEN_PENALTY 1500

#define OP_ITALIC 0x01
#define OP_SPACE 0x02
#define GID_BASE 0x03

#define MAX_GLYPHS 256
#define MINI_CHARS "0123456789 /%"
#define MINI_N 13

extern const u8 PAPER[3], INK[3];

/* round half to even (rint under the default rounding mode) */
static inline long round_half_even(double x) { return (long)rint(x); }

static inline int snap_fp(int fp) { return (fp + 8) >> 4; }

/* q/4 rounded half-to-even, q integer (26.6 -> 12.4 advances) */
static inline int round_q4_even(int q)
{
    int base = q >> 2, rem = q & 3;
    if (rem == 0 || rem == 1)
        return base;
    if (rem == 3)
        return base + 1;
    return base + (base & 1);       /* rem==2: exact half, to even */
}

/* ---------------- dynamic arrays ---------------- */
#define VEC(T) struct { T *v; int n, cap; }
#define VPUSH(a, x) do { \
    if ((a).n == (a).cap) { \
        (a).cap = (a).cap ? (a).cap * 2 : 16; \
        (a).v = realloc((a).v, (size_t)(a).cap * sizeof(*(a).v)); \
    } \
    (a).v[(a).n++] = (x); \
} while (0)

/* ---------------- text model ---------------- */
typedef enum { K_BODY, K_TITLE, K_SUBTITLE, K_BYLINE, K_PART, K_PARTSUB,
               K_DISPLAY } Kind;

typedef struct {
    u8 italic;
    u32 *cp;                        /* codepoints, malloc'd */
    int len;
} Run;

typedef struct {
    Kind kind;
    int indent;
    VEC(Run) runs;
} Para;

typedef struct {
    char idref[8];
    VEC(Para) paras;
} Chapter;

typedef struct {
    VEC(Chapter) v;
} Book;

typedef struct {
    u32 title[128];
    int title_len;
} BookMeta;

/* bb_epub.c */
int epub_parse(const char *epub_dir, Book *book, BookMeta *meta);
int epub_cover_path(const char *epub_dir, char *out, int outsz);
int para_textlen(const Para *p);
void para_text(const Para *p, u32 *out);   /* concatenated runs */
void para_text_cap(const Para *p, u32 *out, int cap);

/* ---------------- font ---------------- */
typedef struct {
    int w, h, bx, by;               /* by baseline-relative */
    int adv_fp;                     /* 12.4 */
    u8 *bm;                         /* w*h alpha levels 0..15 */
} Glyph;

typedef struct {
    int px;
    double gamma;
    int nchars;
    u32 chars[MAX_GLYPHS];
    Glyph g[2][MAX_GLYPHS];         /* [style][charset index] */
    s8 *kern[2];                    /* [nchars*nchars] fp4 */
    int kern_pairs[2];
    int ascent, descent, line_height; /* tight ink metrics */
    int space_fp;
    int question_idx;
} Font;

typedef struct {
    Glyph g[MINI_N];                /* "0123456789 /" @10px, integer adv in adv_fp */
} MiniFont;

void font_build(Font *f, int px, double gamma, const u32 *charset, int n);
void mini_build(MiniFont *m);
int charter_has_glyph(u32 cp);      /* cmap probe before charset is final */
int font_index(const Font *f, u32 cp);      /* -> charset idx, '?' fallback */
int font_kern_fp(const Font *f, int prev_idx, int idx, int style); /* prev -1 = none */

/* title-screen text: render single codepoint at size/ttc-index onto an L
 * canvas with an alpha-mask blend, and its fractional advance in 26.6 */
typedef struct { u8 *px; int w, h; } ImgL;
typedef struct { u8 *px; int w, h; } ImgRGB;   /* 3 bytes per pixel */
void title_draw_char(ImgL *img, int x, int y, u32 cp, int px_size, int fill);
long title_char_adv66(u32 cp, int px_size);
int face_ascent(int px_size);
void font_set_sources(const char *reg, int ri, const char *it, int ii,
                      const char *bold, int bi);

/* bb_hyphen.c — Liang hyphenation points for an ascii word */
void hyphen_load(const char *dic_path);
int hyphen_positions(const u32 *word, int len, int *out, int maxout);

/* bb_image.c — image ops + screens */
ImgL *imgl_new(int w, int h, int fill);
ImgRGB *imgrgb_new(int w, int h);
u8 *load_png_rgba(const char *path, int *w, int *h);
u8 *load_jpeg_rgb(const char *path, int *w, int *h);
u8 *resize_lanczos(const u8 *src, int sw, int sh, int ch, int dw, int dh);
void rgba_over_white_to_L(u8 *rgba, int n, u8 *out_l);
void ink_ramp(const ImgL *gray, ImgRGB *rgb);
void to_rgb555(const ImgRGB *img, u8 *out);
void write_png_2x(const char *path, const ImgRGB *img);
void build_title_bin(const char *png_path, const char *out_path,
                     const char *preview_path);
u8 *cover_screen_raw(const char *img_path);   /* 240x160 RGB555 or NULL */
u8 *cover_screen_from_rgb(const u8 *rgb, int w, int h);
u8 *sprite_encode(const u8 *rgba, int W, int H, int *out_len);
void quantize_median_cut(const u8 *rgb, int npx, int ncolors, u16 *pal_out,
                         u8 *idx_out);
u8 *cover2_encode(const u8 *rgb, int w, int h, int *out_len);
u8 *load_cover_rgb_pub(const char *path, int *w, int *h);

/* fopen or die with a clear message (build/data missing shouldn't segfault) */
static inline FILE *xfopen(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode);
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    return f;
}

/* little-endian file writers */
static inline void w8(FILE *f, unsigned v) { fputc((int)(v & 0xFF), f); }
static inline void w16(FILE *f, unsigned v) { w8(f, v); w8(f, v >> 8); }
static inline void w32(FILE *f, unsigned v) { w16(f, v); w16(f, v >> 16); }

#endif
