/* bookboyadvance — a paperback library in your Game Boy Advance SP.
 *
 * All typesetting happened on the PC: books_bin holds one pre-justified
 * page/line stream per book, library_bin the shelf index (encoded titles,
 * offsets, page counts, optional cover screens). This runtime replays line
 * records with a 12.4 fixed-point pen (kerning per glyph pair, position
 * snapped once per glyph), blits 4bpp-alpha glyphs into an EWRAM canvas,
 * and DMAs the canvas to the hidden mode-4 page before flipping.
 *
 * Flow: title -> library -> (cover) -> reader.  Every book keeps its own
 * bookmark in SRAM; SELECT cycles reading palettes anywhere in the reader.
 */
#include <stdint.h>
#include "../tools/bb_art.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int32_t s32;

#define REG_DISPCNT   (*(volatile u16 *)0x4000000)
#define REG_DISPSTAT  (*(volatile u16 *)0x4000004)
#define REG_VCOUNT    (*(volatile u16 *)0x4000006)
#define REG_KEYINPUT  (*(volatile u16 *)0x4000130)
#define REG_IE        (*(volatile u16 *)0x4000200)
#define REG_IF        (*(volatile u16 *)0x4000202)
#define REG_WAITCNT   (*(volatile u16 *)0x4000204)
#define REG_IME       (*(volatile u16 *)0x4000208)
#define REG_DMA3SAD   (*(volatile u32 *)0x40000D4)
#define REG_DMA3DAD   (*(volatile u32 *)0x40000D8)
#define REG_DMA3CNT   (*(volatile u32 *)0x40000DC)
#define REG_ISR_ADDR  (*(volatile u32 *)0x3007FFC)
#define REG_INTR_CHECK (*(volatile u16 *)0x3007FF8)   /* BIOS IntrWait flags */

#define MODE3         0x0403u
#define MODE4         0x0404u
#define DISP_PAGE1    0x0010u

#define BG_PALETTE    ((volatile u16 *)0x5000000)
#define VRAM_FRONT    ((void *)0x6000000)
#define VRAM_BACK     ((void *)0x600A000)
#define SRAM          ((volatile u8 *)0x0E000000)

#define DMA_ENABLE    (1u << 31)
#define DMA32         (1u << 26)
#define DMA_SRC_FIXED (2u << 23)

#define KEY_A         0x0001
#define KEY_B         0x0002
#define KEY_SELECT    0x0004
#define KEY_START     0x0008
#define KEY_RIGHT     0x0010
#define KEY_LEFT      0x0020
#define KEY_UP        0x0040
#define KEY_DOWN      0x0080
#define KEY_R         0x0100
#define KEY_L         0x0200
#define KEY_ANY       0x03FF

#define SCREEN_W      240
#define SCREEN_H      160

#define OP_ITALIC     0x01
#define OP_SPACE      0x02
#define GID_BASE      0x03
#define NO_PREV       0xFFFF

#define MARGIN_L      8
#define MARGIN_R      8
#define FOOTER_BASE   (SCREEN_H - 3)

/* Reading palettes, cycled with SELECT. Preset 0 must match PAPER/INK in
 * tools/bb_image.c (the previews are rendered with it). CURVE_LIGHT lifts
 * the antialiasing mid-tones to counter the LCD's gamma crush on frontlit
 * (AGS-001) panels. */
typedef struct {
    u8 paper[3], ink[3];
    const u8 *curve;
} Palette;

static const u8 CURVE_LINEAR[16] = {0, 17, 34, 51, 68, 85, 102, 119,
                                    136, 153, 170, 187, 204, 221, 238, 255};
static const u8 CURVE_LIGHT[16] = {0, 7, 17, 29, 43, 58, 74, 91,
                                   109, 128, 148, 168, 189, 210, 232, 255};

static const Palette PRESETS[] = {
    {{0xF2, 0xEA, 0xD7}, {0x2B, 0x26, 0x20}, CURVE_LINEAR}, /* paperback */
    {{0xFF, 0xFB, 0xF0}, {0x00, 0x00, 0x00}, CURVE_LINEAR}, /* high contrast */
    {{0xFF, 0xFF, 0xFF}, {0x08, 0x08, 0x08}, CURVE_LIGHT},  /* bright */
    {{0x10, 0x10, 0x16}, {0xD8, 0xD4, 0xC8}, CURVE_LINEAR}, /* night */
};
#define NPRESETS (sizeof(PRESETS) / sizeof(PRESETS[0]))

static u32 cur_pal;

/* Library book-spine colors: muted paperback shades. Each gets its own
 * 16-entry spine->ink ramp at palette 16 + i*16, so titles antialias
 * correctly on colored spines (reader text keeps entries 0-15). */
/* the title art's grey tones (216/184/152/128), duotone-mapped */
static const u8 SPINE_COLORS[4][3] = {
    {0xD4, 0xCC, 0xBB},
    {0xBB, 0xB3, 0xA4},
    {0xA2, 0x9B, 0x8D},
    {0x90, 0x88, 0x7C},
};
#define SPINE_INK_R 0x2B
#define SPINE_INK_G 0x26
#define SPINE_INK_B 0x20

/* glyph blits land at pal_base + level: 0 for reader text, a spine ramp
 * base on the library stack */
static u32 blit_pal_base;

/* Data is appended after the code stub as [magic][offset table][blobs] so
 * ROMs can be assembled by concatenation (rompack natively, plain JS on the
 * web) with no ARM toolchain. The table is found by scanning ROM at
 * 256-byte boundaries for the magic. */
typedef struct {
    u8 magic[8];
    u32 version;
    u32 font_off, books_off, library_off, mini_off, title_off;
    u32 default_pal;                /* 0 = unset, else preset index + 1 */
    u32 art_off;
} DataTable;

static u32 table_default_pal;

static const u8 *font_bin;
static const u8 *books_bin;
static const u8 *library_bin;
static const u8 *mini_bin;
static const u8 *title_bin;
static const u8 *art_bin;

/* "BKBYDAT1", stored inverted so the pattern itself never occurs in the
 * code stub (a plain literal would be a false match for the scanner) */
static const u8 MAGIC_INV[8] = {
    0xFFu - 'B', 0xFFu - 'K', 0xFFu - 'B', 0xFFu - 'Y',
    0xFFu - 'D', 0xFFu - 'A', 0xFFu - 'T', 0xFFu - '1',
};

static int data_init(void)
{
    for (u32 addr = 0x08000100; addr < 0x08200000; addr += 256) {
        const DataTable *t = (const DataTable *)addr;
        u32 ok = 1;
        for (u32 k = 0; k < 8; k++)
            if ((u8)(0xFFu - t->magic[k]) != MAGIC_INV[k])
                ok = 0;
        if (ok && t->version == 1) {    /* the web packer evolves separately */
            const u8 *base = (const u8 *)t;
            font_bin = base + t->font_off;
            books_bin = base + t->books_off;
            library_bin = base + t->library_off;
            mini_bin = base + t->mini_off;
            title_bin = base + t->title_off;
            art_bin = t->art_off ? base + t->art_off : 0;
            table_default_pal = t->default_pal;
            return 1;
        }
    }
    return 0;
}

/* EZ-Flash & emulators detect the save type from this string. */
/* aligned: EZ-Flash/emulators scan the save-ID on word boundaries */
__attribute__((used, aligned(4))) static const char save_type[] = "SRAM_V113";

static u8 canvas[SCREEN_W * SCREEN_H] __attribute__((section(".sbss"), aligned(4)));

static void canvas_clear(void);
static void vsync(void);
static void dma_copy32(const void *src, void *dst, u32 bytes);

/* ---- font ---- */
static struct {
    u16 nglyphs;
    u8 line_height, ascent, space_px;
    const u8 *ascii_map;
    const u16 *adv_fp;
    const s8 *kern;
    const u32 *offsets;
    const u8 *blob;
} F;

static void font_init(void)
{
    const u8 *p = font_bin;
    F.nglyphs = (u16)(p[4] | (p[5] << 8));
    F.line_height = p[6];
    F.ascent = p[7];
    F.space_px = p[8];
    u32 n = F.nglyphs;
    u32 off = 12;
    F.ascii_map = p + off;          off += 128;
    F.adv_fp = (const u16 *)(p + off); off += 4 * n;
    F.kern = (const s8 *)(p + off);
    off += (2 * n * n + 3) & ~3u;
    F.offsets = (const u32 *)(p + off); off += 8 * n;
    F.blob = p + off;
}

/* ---- library ---- */
#define LIB_ENTRY_SZ 96
#define MAX_BOOKS 80            /* matches the builder's shelf[80] cap */

static struct {
    u32 nbooks;
} L;

static void lib_init(void)
{
    L.nbooks = *(const u32 *)(library_bin + 4);
    if (L.nbooks > MAX_BOOKS)
        L.nbooks = MAX_BOOKS;
}

static const u8 *lib_entry(u32 i)
{
    return library_bin + 8 + i * LIB_ENTRY_SZ;
}

static u32 lib_book_off(u32 i)  { return *(const u32 *)(lib_entry(i) + 0); }
static u32 lib_cover_off(u32 i) { return *(const u32 *)(lib_entry(i) + 4); }
static u32 lib_cover2_off(u32 i){ return *(const u32 *)(lib_entry(i) + 8); }
static u32 lib_npages(u32 i)
{
    const u8 *e = lib_entry(i);
    return (u32)(e[12] | (e[13] << 8));
}
static u32 lib_title_len(u32 i) { return lib_entry(i)[14]; }
static const u8 *lib_title(u32 i) { return lib_entry(i) + 16; }

/* ---- current book ---- */
static struct {
    const u8 *base;
    u32 npages, ntoc;
    const u8 *toc;
    const u32 *page_off;
} B;

static void book_init(u32 book)
{
    const u8 *p = books_bin + lib_book_off(book);
    B.base = p;
    B.npages = *(const u32 *)(p + 4);
    B.ntoc = *(const u32 *)(p + 8);
    B.toc = p + 16;
    u32 offs_pos = 16 + B.ntoc * 48;
    B.page_off = (const u32 *)(p + *(const u32 *)(p + offs_pos));
}

/* ---- drawing ---- */
__attribute__((section(".iwram"), target("arm"), long_call, noinline))
static void blit_glyph(u32 style, u32 gid, s32 pen_x, s32 baseline)
{
    const u8 *g = F.blob + F.offsets[style * F.nglyphs + gid];
    s32 w = g[0], h = g[1];
    if (!w)
        return;
    s32 x0 = pen_x + (s8)g[2];
    s32 y0 = baseline + (s8)g[3];
    const u8 *rows = g + 4;
    s32 row_bytes = (w + 1) >> 1;
    for (s32 r = 0; r < h; r++) {
        s32 y = y0 + r;
        if (y < 0 || y >= SCREEN_H)
            continue;
        u8 *dst = canvas + y * SCREEN_W + x0;
        const u8 *src = rows + r * row_bytes;
        for (s32 c = 0; c < w; c++) {
            u32 a = (src[c >> 1] >> ((c & 1) * 4)) & 0xF;
            s32 x = x0 + c;
            if (a && x >= 0 && x < SCREEN_W) {
                u32 cur = dst[c];
                u32 lvl = (cur >= blit_pal_base && cur < blit_pal_base + 16)
                              ? cur - blit_pal_base : 0;
                if (a > lvl)
                    dst[c] = (u8)(blit_pal_base + a);
            }
        }
    }
}

static s32 draw_stream(const u8 *s, u32 nbytes, s32 x, s32 baseline)
{
    s32 pen_fp = x << 4;
    u32 prev = NO_PREV;
    u32 style = 0;
    u32 n = F.nglyphs;
    for (u32 i = 0; i < nbytes; i++) {
        u32 b = s[i];
        if (b == OP_ITALIC) {
            style ^= 1;
            prev = NO_PREV;
        } else if (b == OP_SPACE) {
            i++;
            pen_fp = ((((pen_fp + 8) >> 4) + s[i]) << 4);
            prev = NO_PREV;
        } else {
            u32 gid = b - GID_BASE;
            if (prev != NO_PREV)
                pen_fp += F.kern[style * n * n + prev * n + gid];
            blit_glyph(style, gid, (pen_fp + 8) >> 4, baseline);
            pen_fp += F.adv_fp[style * n + gid];
            prev = gid;
        }
    }
    return (pen_fp + 8) >> 4;
}

/* ascii text via the font's ascii map (UI chrome: LIBRARY header) */
static s32 draw_ascii(const char *s, s32 x, s32 baseline, s32 tracking)
{
    while (*s) {
        u8 gid = F.ascii_map[(u8)*s];
        if (gid) {
            u8 b = gid;
            x = draw_stream(&b, 1, x, baseline) + tracking;
        } else {
            x += F.space_px + tracking;
        }
        s++;
    }
    return x - tracking;
}

/* ---- mini footer font: "0123456789 /%", dimmed ---- */
static const u8 *mini_rec(u32 i)
{
    const u16 *offs = (const u16 *)(mini_bin + 2);
    return mini_bin + offs[i];
}

static s32 mini_gid(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c == ' ')
        return 10;
    if (c == '/')
        return 11;
    if (c == '%')
        return 12;
    return -1;
}

static u32 mini_width(const char *s)
{
    u32 w = 0;
    for (; *s; s++) {
        s32 gi = mini_gid(*s);
        if (gi >= 0)
            w += mini_rec(gi)[4];
    }
    return w;
}

static void mini_draw(const char *s, s32 x, s32 baseline)
{
    for (; *s; s++) {
        s32 gi = mini_gid(*s);
        if (gi < 0)
            continue;
        const u8 *g = mini_rec(gi);
        s32 w = g[0], h = g[1];
        s32 x0 = x + (s8)g[2], y0 = baseline + (s8)g[3];
        const u8 *rows = g + 5;
        s32 row_bytes = (w + 1) >> 1;
        for (s32 r = 0; r < h; r++) {
            s32 y = y0 + r;
            if (y < 0 || y >= SCREEN_H)
                continue;
            for (s32 c = 0; c < w; c++) {
                u32 a = (rows[r * row_bytes + (c >> 1)] >> ((c & 1) * 4)) & 0xF;
                a = (a * 3) >> 2;
                s32 X = x0 + c;
                if (a && X >= 0 && X < SCREEN_W) {
                    u8 *dst = canvas + y * SCREEN_W + X;
                    u32 cur = *dst;
                    u32 lvl = (cur >= blit_pal_base &&
                               cur < blit_pal_base + 16)
                                  ? cur - blit_pal_base : 0;
                    if (a > lvl)
                        *dst = (u8)(blit_pal_base + a);
                }
            }
        }
        x += g[4];
    }
}

static char *fmt_u32(char *out, u32 v)
{
    char tmp[12];
    u32 t = 0;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (t)
        *out++ = tmp[--t];
    *out = 0;
    return out;
}

/* ---- table of contents ---- */
static u32 toc_page(u32 i)
{
    const u8 *e = B.toc + i * 48;
    return (u32)(e[0] | (e[1] << 8));
}

static u32 story_of_page(u32 page)
{
    u32 s = 0;
    for (u32 i = 0; i < B.ntoc; i++)
        if (toc_page(i) <= page)
            s = i;
    return s;
}

/* ---- page rendering ---- */
static void render_page(u32 page)
{
    canvas_clear();
    const u8 *p = B.base + B.page_off[page];
    u32 nlines = *p++;
    for (u32 l = 0; l < nlines; l++) {
        u32 y = p[0], x = p[1], len = p[2];
        p += 3;
        draw_stream(p, len, (s32)x, (s32)y);
        p += len;
    }
    /* footer: book percent left, page-in-chapter right */
    char buf[20];
    u32 pct = ((page + 1) * 100) / B.npages;
    if (pct > 100)
        pct = 100;
    char *e = fmt_u32(buf, pct);
    *e++ = '%';
    *e = 0;
    mini_draw(buf, MARGIN_L, FOOTER_BASE);
    u32 s = story_of_page(page);
    u32 cstart = B.ntoc ? toc_page(s) : 0;
    u32 cend = (s + 1 < B.ntoc) ? toc_page(s + 1) : B.npages;
    e = fmt_u32(buf, page - cstart + 1);
    *e++ = ' '; *e++ = '/'; *e++ = ' ';
    fmt_u32(e, cend - cstart);
    mini_draw(buf, (s32)(SCREEN_W - MARGIN_R - (s32)mini_width(buf)),
              FOOTER_BASE);
}

/* ---- system ---- */
/* VBlank IRQ so vsync() can Halt the CPU instead of spinning — a reader
 * idles minutes per page, and busy-waiting would drain the SP battery for
 * nothing. The handler just acknowledges IF and the BIOS IntrWait flag.
 * gba.specs installs no handler, so we own the ISR vector at 0x3007FFC. */
__attribute__((section(".iwram"), target("arm"), used))
static void irq_handler(void)
{
    u16 f = REG_IF;
    REG_IF = f;                     /* ack hardware */
    REG_INTR_CHECK |= f;            /* ack for VBlankIntrWait */
}

static void system_init(void)
{
    /* SRAM 8 waits (battery-SRAM spec), ROM WS0 3/1, prefetch on. 3/1 is
     * EZ-Flash-safe; 2/1 glitches EZ-Omega PSRAM. Also ~2x ROM code speed. */
    REG_WAITCNT = 0x4317;
    REG_ISR_ADDR = (u32)irq_handler;
    REG_DISPSTAT |= 0x0008;         /* VBlank IRQ enable in DISPSTAT */
    REG_IE = 0x0001;                /* VBlank */
    REG_IME = 1;
}

static void vsync(void)
{
    __asm volatile("swi 0x05" ::: "r0", "r1", "r2", "r3");  /* VBlankIntrWait */
}

static void dma_copy32(const void *src, void *dst, u32 bytes)
{
    REG_DMA3SAD = (u32)src;
    REG_DMA3DAD = (u32)dst;
    REG_DMA3CNT = (bytes >> 2) | DMA_ENABLE | DMA32;
    (void)REG_DMA3CNT;
}

/* DMA-fill `bytes` at dst from a single 32-bit source word */
static u32 dma_fill_word;
static void dma_fill32(void *dst, u32 word, u32 bytes)
{
    dma_fill_word = word;
    REG_DMA3SAD = (u32)&dma_fill_word;
    REG_DMA3DAD = (u32)dst;
    REG_DMA3CNT = (bytes >> 2) | DMA_ENABLE | DMA32 | DMA_SRC_FIXED;
    (void)REG_DMA3CNT;
}

static void canvas_clear(void)
{
    dma_fill32(canvas, 0, sizeof(canvas));   /* ~0.6ms vs ~7ms CPU loop */
}

static void set_palette(void)
{
    const Palette *p = &PRESETS[cur_pal];
    for (u32 i = 0; i < 16; i++) {
        u32 w = p->curve[i];
        u32 c[3];
        for (u32 k = 0; k < 3; k++)
            c[k] = (u32)(p->paper[k] +
                         ((s32)p->ink[k] - (s32)p->paper[k]) * (s32)w / 255);
        BG_PALETTE[i] = (u16)((c[0] >> 3) | ((c[1] >> 3) << 5) |
                              ((c[2] >> 3) << 10));
    }
}

static void set_spine_palette(void)
{
    const s32 ink[3] = {SPINE_INK_R, SPINE_INK_G, SPINE_INK_B};
    for (u32 i = 0; i < 16; i++) {  /* white bank: art + selected book */
        u32 c[3];
        for (u32 k = 0; k < 3; k++)
            c[k] = (u32)(255 + (ink[k] - 255) * (s32)i / 15);
        BG_PALETTE[PAL_WHITE + i] =
            (u16)((c[0] >> 3) | ((c[1] >> 3) << 5) | ((c[2] >> 3) << 10));
    }
    for (u32 s = 0; s < PAL_SPINE_BANKS; s++)
        for (u32 i = 0; i < 16; i++) {
            u32 c[3];
            for (u32 k = 0; k < 3; k++)
                c[k] = (u32)(SPINE_COLORS[s][k] +
                             (ink[k] - (s32)SPINE_COLORS[s][k]) * (s32)i / 15);
            BG_PALETTE[PAL_SPINE_BASE + s * 16 + i] =
                (u16)((c[0] >> 3) | ((c[1] >> 3) << 5) | ((c[2] >> 3) << 10));
        }
}

/* draw canvas to the hidden mode-4 page, then show it */
static u32 shown_page;

static void present(void)
{
    void *back = shown_page ? VRAM_FRONT : VRAM_BACK;
    dma_copy32(canvas, back, sizeof(canvas));
    vsync();
    shown_page ^= 1;
    REG_DISPCNT = shown_page ? (MODE4 | DISP_PAGE1) : MODE4;
}

/* ---- input with key repeat ---- */
static u16 keys_now, keys_prev;
static u16 rpt_key;
static u32 rpt_timer;

static void keys_poll(void)
{
    keys_prev = keys_now;
    keys_now = (u16)(~REG_KEYINPUT) & KEY_ANY;
}

static u16 keys_hit(void)
{
    return (u16)(keys_now & ~keys_prev);
}

static u16 keys_repeat(u16 mask)
{
    u16 hit = (u16)(keys_hit() & mask);
    if (hit) {
        rpt_key = hit;
        rpt_timer = 20;
        return hit;
    }
    if (keys_now & rpt_key & mask) {
        if (--rpt_timer == 0) {
            rpt_timer = 7;
            return rpt_key;
        }
    } else {
        rpt_key = 0;
    }
    return 0;
}

/* ---- SRAM: per-book bookmarks ----
 * [0..3]='BKB2' [4]=palette [5]=last_book [6..7]=0
 * then per book: u32 npages_guard, u32 page                       */
static u32 last_book;

static void sram_w(u32 off, u32 v, u32 n)
{
    for (u32 i = 0; i < n; i++)
        SRAM[off + i] = (u8)(v >> (8 * i));
}

static u32 sram_r(u32 off, u32 n)
{
    u32 v = 0;
    for (u32 i = 0; i < n; i++)
        v |= (u32)SRAM[off + i] << (8 * i);
    return v;
}

static void save_meta(void)
{
    SRAM[0] = 'B'; SRAM[1] = 'K'; SRAM[2] = 'B'; SRAM[3] = '2';
    SRAM[4] = (u8)cur_pal;
    SRAM[5] = (u8)last_book;
    SRAM[6] = 0; SRAM[7] = 0;
}

static void save_book_page(u32 book, u32 page)
{
    /* Write the page first, then the npages guard as the commit record:
     * a power-off mid-write (SP users flip off right after turning a page)
     * leaves a stale guard rather than a valid guard over a partial page,
     * so load_book_page rejects the torn write. */
    u32 off = 8 + book * 8;
    sram_w(off + 4, page, 4);
    sram_w(off, lib_npages(book), 4);
    save_meta();
}

static u32 load_book_page(u32 book)
{
    u32 off = 8 + book * 8;
    if (sram_r(off, 4) != lib_npages(book))
        return 0;                   /* re-typeset or never opened */
    u32 page = sram_r(off + 4, 4);
    return page < lib_npages(book) ? page : 0;
}

static u32 book_pct(u32 book)
{
    u32 np = lib_npages(book);
    if (!np)
        return 0;
    return ((load_book_page(book) + 1) * 100) / np;
}

static int sram_valid(void)
{
    return SRAM[0] == 'B' && SRAM[1] == 'K' && SRAM[2] == 'B' &&
           SRAM[3] == '2';
}

/* ---- screens ---- */
static void show_title(void)
{
    const u8 *t = title_bin;
    u16 paper = (u16)(t[0] | (t[1] << 8));
    u32 sx = (u32)(t[2] | (t[3] << 8)), sy = (u32)(t[4] | (t[5] << 8));
    u32 sw = (u32)(t[6] | (t[7] << 8)), sh = (u32)(t[8] | (t[9] << 8));
    const u8 *img = t + 12;
    const u8 *strip = img + SCREEN_W * SCREEN_H * 2;
    const u8 *strip_off = strip + (u32)(sw * sh) * 2;
    int two_strips = paper == 0xFFFF;
    REG_DISPCNT = MODE3;
    dma_copy32(img, VRAM_FRONT, SCREEN_W * SCREEN_H * 2);
    u32 frame = 0;
    for (;;) {
        vsync();
        keys_poll();
        if (keys_hit() & (KEY_START | KEY_A))
            return;
        frame++;
        u32 on = (frame & 63) < 40;
        volatile u16 *vram = (volatile u16 *)VRAM_FRONT;
        for (u32 r = 0; r < sh; r++) {
            volatile u16 *dst = vram + (sy + r) * SCREEN_W + sx;
            const u8 *srow = (on ? strip : strip_off) + (r * sw) * 2;
            for (u32 c = 0; c < sw; c++)
                dst[c] = two_strips || on
                             ? (u16)(srow[c * 2] | (srow[c * 2 + 1] << 8))
                             : paper;
        }
    }
}

static void show_cover(u32 cover_off)
{
    if (!cover_off)
        return;
    REG_DISPCNT = MODE3;
    dma_copy32(books_bin + cover_off, VRAM_FRONT, SCREEN_W * SCREEN_H * 2);
    for (;;) {
        vsync();
        keys_poll();
        if (keys_hit() & KEY_ANY)
            return;
    }
}


/* ---- art sprites (level 4bpp + alpha 1bpp, canvas-placed) ---- */

static const u8 *art_spr(u32 id)
{
    const u32 *tab = (const u32 *)(art_bin + 8);
    return art_bin + tab[id * 2];
}

/* draw sprite at its baked canvas spot shifted by (dx,dy), colors from
 * pal_base ramp; alpha-masked. Hot path: lives in IWRAM (ROM waitstates
 * made stack redraws visibly laggy) and skips transparent 8px runs. */
__attribute__((section(".iwram"), target("arm"), long_call, noinline))
static void blit_sprite(u32 id, s32 dx, s32 dy, u32 pal_base)
{
    const u8 *sp = art_spr(id);
    s32 x0 = sp[0] + dx, y0 = sp[1] + dy;
    s32 w = sp[2], h = sp[3];
    s32 lvl_rb = (w + 1) >> 1, al_rb = (w + 7) >> 3;
    const u8 *lvl = sp + 8;
    const u8 *al = sp + 8 + lvl_rb * h;
    for (s32 r = 0; r < h; r++) {
        s32 y = y0 + r;
        if (y < 0 || y >= SCREEN_H)
            continue;
        u8 *dst = canvas + y * SCREEN_W;
        const u8 *lr = lvl + r * lvl_rb;
        const u8 *ar = al + r * al_rb;
        for (s32 cb = 0; cb < al_rb; cb++) {
            u32 abyte = ar[cb];
            if (!abyte)
                continue;           /* 8 transparent pixels at once */
            s32 cbase = cb << 3;
            s32 cend = cbase + 8 < w ? cbase + 8 : w;
            for (s32 c = cbase; c < cend; c++) {
                if (!(abyte & (1u << (c & 7))))
                    continue;
                s32 x = x0 + c;
                if (x < 0 || x >= SCREEN_W)
                    continue;
                u32 v = (lr[c >> 1] >> ((c & 1) * 4)) & 0xF;
                dst[x] = (u8)(pal_base + v);
            }
        }
    }
}

static void fill_rect(s32 x, s32 y, s32 w, s32 h, u8 idx)
{
    if (x < 0) { w += x; x = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    for (s32 r = 0; r < h; r++) {
        s32 Y = y + r;
        if (Y < 0 || Y >= SCREEN_H)
            continue;
        u8 *dst = canvas + Y * SCREEN_W + x;
        for (s32 c = 0; c < w; c++)
            dst[c] = idx;
    }
}

/* held-book pose, composed: white book face sized to the cover, ink
 * outline, cover pixels, and the two show_hand sprites cupping the
 * bottom corners (their pocket anchor nails the alignment) */
static void show_held_book(u32 off)
{
    u32 w = AR_COVER_W, h = AR_COVER_H;
    const u8 *c2 = 0;
    if (off) {
        c2 = books_bin + off;
        w = c2[0];
        h = c2[1];
        const u8 *pal = c2 + 4;
        for (u32 i = 0; i < 128; i++)
            BG_PALETTE[PAL_COVER + i] =
                (u16)(pal[i * 2] | (pal[i * 2 + 1] << 8));
    }
    /* bare cover, corners straight into the hand pockets */
    s32 bx = (SCREEN_W - (s32)w) / 2;
    s32 by = AR_BOOK_BOTTOM - (s32)h;
    if (c2) {
        const u8 *px = c2 + 4 + 256;
        for (u32 r = 0; r < h; r++) {
            u8 *dst = canvas + (by + (s32)r) * SCREEN_W + bx;
            for (u32 c = 0; c < w; c++)
                dst[c] = (u8)(PAL_COVER + px[r * w + c]);
        }
    } else {
        fill_rect(bx, by, (s32)w, (s32)h, PAL_WHITE);
    }
    const u8 *hl = art_spr(ART_SHOWL);
    s32 lx = bx - (hl[0] + AR_POCKET_X);
    s32 ly = AR_BOOK_BOTTOM - (hl[1] + AR_POCKET_Y);
    blit_sprite(ART_SHOWL, lx, ly, PAL_WHITE);
    const u8 *hr = art_spr(ART_SHOWR);
    s32 rw = hr[2];
    s32 rx = (bx + (s32)w) - (hr[0] + rw - AR_POCKET_X);
    blit_sprite(ART_SHOWR, rx, ly, PAL_WHITE);
}

/* ---- library: hand-drawn book stack + grab animation ----
 * The selected book sits at the art's spine position (4th from the bottom
 * of the screen); the stack scrolls around it. A plays grab_book1-5 into
 * the held-book pose with the quantized cover, then the info bubble. */

static u32 spine_hash(u32 i)
{
    const u8 *t = lib_title(i);
    u32 h = 2166136261u;
    for (u32 k = 0; k < lib_title_len(i); k++) {
        h ^= t[k];
        h *= 16777619u;
    }
    return h ^ (lib_npages(i) * 2654435761u);
}

static u32 spine_bank(u32 i)
{
    return PAL_SPINE_BASE + (spine_hash(i) % PAL_SPINE_BANKS) * 16;
}

/* rows visible above/below the anchor */
#define STACK_UP 3
#define STACK_DOWN 4

static void draw_spine_row2(u32 idx, s32 k, u32 bank, s32 drop)
{
    s32 dy = k * AR_PITCH + drop;
    /* slight per-book shove; the selected book stays put for the anims */
    s32 dx = k == 0 ? 0 : (s32)((spine_hash(idx) >> 8) % 9) - 4;
    blit_sprite(ART_BOOK, dx, dy, bank);
    blit_pal_base = bank;
    s32 baseline = AR_SPINE_Y + dy + 5 + F.ascent;
    draw_stream(lib_title(idx), lib_title_len(idx), AR_SPINE_X + dx + 14,
                baseline);
    if (sram_valid() && load_book_page(idx) > 0) {
        char pct[8];
        char *e = fmt_u32(pct, book_pct(idx));
        *e++ = '%';
        *e = 0;
        mini_draw(pct,
                  AR_SPINE_X + dx + AR_SPINE_W - 12 - (s32)mini_width(pct),
                  baseline);
    }
    blit_pal_base = 0;
}

/* scroll-feedback arrows left of the stack; grow briefly on press */
static void draw_arrow(s32 cx, s32 cy, s32 size, int up)
{
    for (s32 r = 0; r < size; r++) {
        s32 half = up ? r : size - 1 - r;
        s32 y = cy + (up ? r : r);
        for (s32 x = cx - half; x <= cx + half; x++)
            if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H)
                canvas[y * SCREEN_W + x] = 15;
    }
}

static void draw_arrows(u32 cursor, u32 up_hot, u32 down_hot)
{
    s32 cx = 9;
    s32 mid = AR_SPINE_Y + AR_SPINE_H / 2;
    if (cursor > 0) {
        s32 sz = up_hot ? 7 : 4;
        draw_arrow(cx, mid - 14 - sz, sz, 1);
    }
    if (cursor + 1 < L.nbooks) {
        s32 sz = down_hot ? 7 : 4;
        draw_arrow(cx, mid + 14, sz, 0);
    }
}

static void draw_stack3(u32 cursor, int with_selected, s32 drop)
{
    canvas_clear();
    for (s32 k = STACK_DOWN; k >= -STACK_UP; k--) {
        s32 idx = (s32)cursor + k;
        if (idx < 0 || idx >= (s32)L.nbooks)
            continue;
        if (k == 0 && !with_selected)
            continue;
        draw_spine_row2((u32)idx, k, k == 0 ? PAL_WHITE : spine_bank((u32)idx),
                        k < 0 ? drop : 0);
    }
}

static void draw_stack(u32 cursor, int with_selected)
{
    draw_stack3(cursor, with_selected, 0);
}

static void speak_info(u32 book)
{
    blit_sprite(ART_SPEAK, 0, 0, PAL_WHITE);
    char info[64];
    char *e = info;
    e = fmt_u32(e, B.ntoc);
    const char *s1 = B.ntoc == 1 ? " chapter   " : " chapters   ";
    while (*s1) *e++ = *s1++;
    e = fmt_u32(e, B.npages);
    const char *s2 = " pages   ";
    while (*s2) *e++ = *s2++;
    e = fmt_u32(e, book_pct(book));
    *e++ = '%'; *e = 0;
    blit_pal_base = PAL_WHITE;
    draw_ascii(info, AR_SPEAK_X + 14, AR_SPEAK_Y + 3 + F.ascent, 0);
    blit_pal_base = 0;
}

static void anim_wait(u32 frames)
{
    for (u32 i = 0; i < frames; i++) {
        vsync();
        keys_poll();
    }
}

/* returns selected book, or -1 if B pressed (back to title) */
static int library(void)
{
    u32 cursor = last_book < L.nbooks ? last_book : 0;
    u32 up_t = 0, down_t = 0;
    for (;;) {
        /* browse */
        draw_stack(cursor, 1);
        draw_arrows(cursor, up_t, down_t);
        present();
        for (;;) {
            vsync();
            keys_poll();
            u16 k = keys_repeat(KEY_UP | KEY_DOWN);
            u16 hit = keys_hit();
            u32 nc = cursor;
            if ((k & KEY_DOWN) && cursor + 1 < L.nbooks)
                nc = cursor + 1;
            else if ((k & KEY_UP) && cursor > 0)
                nc = cursor - 1;
            int redraw = 0;
            if (hit & KEY_UP) {
                redraw |= up_t == 0;
                up_t = 10;
            }
            if (hit & KEY_DOWN) {
                redraw |= down_t == 0;
                down_t = 10;
            }
            if (up_t && --up_t == 0)
                redraw = 1;         /* arrow shrinks back */
            if (down_t && --down_t == 0)
                redraw = 1;
            if (nc != cursor || redraw) {
                cursor = nc;
                draw_stack(cursor, 1);
                draw_arrows(cursor, up_t, down_t);
                present();
            }
            if (hit & KEY_B)
                return -1;
            if (hit & (KEY_A | KEY_START))
                break;
        }
        /* grab animation: frames 1-2 reach (book still in stack),
         * 3-5 the frame art carries the book */
        book_init(cursor);          /* for chapter count in the bubble */
        /* frame 0 = the idle hands rising, then the four grab poses */
        static const u8 ANIM[5] = {ART_HANDS, ART_GRAB1, ART_GRAB2,
                                   ART_GRAB3, ART_GRAB4};
        for (u32 f = 0; f < 5; f++) {
            draw_stack(cursor, f < 3);  /* frame art carries the book from grab3 */
            blit_sprite(ANIM[f], 0, 0, PAL_WHITE);
            present();
            anim_wait(3);
        }
        /* held pose lands; the stack drops closed beneath it (two steps,
         * tiny bounce, settle) where the motion is actually visible */
        static const u8 DROPS[4] = {11, AR_PITCH, AR_PITCH - 3, AR_PITCH};
        for (u32 d = 0; d < 4; d++) {
            draw_stack3(cursor, 0, DROPS[d]);
            show_held_book(lib_cover2_off(cursor));
            present();
            anim_wait(2);
        }
        anim_wait(4);
        draw_stack3(cursor, 0, AR_PITCH);
        show_held_book(lib_cover2_off(cursor));
        speak_info(cursor);
        present();
        for (;;) {
            vsync();
            keys_poll();
            u16 hit = keys_hit();
            if (hit & (KEY_A | KEY_START))
                return (int)cursor; /* open the reader */
            if (hit & KEY_B)
                break;              /* put the book back */
        }
    }
}

/* ---- reader ---- */
static void reader(u32 book, u32 page)
{
    render_page(page);
    present();
    save_book_page(book, page);
    for (;;) {
        vsync();
        keys_poll();
        u32 new_page = page;
        u16 k = keys_repeat(KEY_A | KEY_RIGHT | KEY_R | KEY_LEFT | KEY_L);
        if (k & (KEY_A | KEY_RIGHT | KEY_R)) {
            if (page + 1 < B.npages)
                new_page = page + 1;
        } else if (k & (KEY_LEFT | KEY_L)) {
            if (page > 0)
                new_page = page - 1;
        }
        u16 hit = keys_hit();
        if (hit & KEY_DOWN) {
            u32 s = story_of_page(page);
            if (s + 1 < B.ntoc)
                new_page = toc_page(s + 1);
        } else if (hit & KEY_UP) {
            u32 s = story_of_page(page);
            u32 start = toc_page(s);
            new_page = (page == start && s > 0) ? toc_page(s - 1) : start;
        } else if (hit & KEY_B) {
            save_book_page(book, page);
            return;                 /* back to library */
        } else if (hit & KEY_SELECT) {
            cur_pal = (cur_pal + 1) % NPRESETS;
            vsync();                /* write palette in VBlank, no tear */
            set_palette();          /* instant: palette-only, no re-render */
            save_meta();
        }
        if (new_page != page) {
            page = new_page;
            render_page(page);
            present();
            save_book_page(book, page);
        }
    }
}

int main(void)
{
    system_init();
    if (!data_init() || !art_bin) {  /* bare stub: nothing to show */
        REG_DISPCNT = MODE4;
        dma_fill32(VRAM_FRONT, 0, SCREEN_W * SCREEN_H);  /* clear BIOS logo */
        BG_PALETTE[0] = 0x7C10;     /* purple = missing data */
        for (;;)
            vsync();                /* Halt, don't spin */
    }
    font_init();
    lib_init();
    if (table_default_pal >= 1 && table_default_pal <= NPRESETS)
        cur_pal = table_default_pal - 1;
    if (sram_valid()) {
        if (SRAM[4] < NPRESETS)
            cur_pal = SRAM[4];
        if (SRAM[5] < L.nbooks)
            last_book = SRAM[5];
    }
    set_palette();
    set_spine_palette();
    for (;;) {
        show_title();
        for (;;) {
            int sel;
            if (L.nbooks == 1) {    /* single-book ROM: no library screen */
                sel = 0;
            } else {
                sel = library();
                if (sel < 0)
                    break;          /* B: back to title */
            }
            last_book = (u32)sel;
            if (L.nbooks == 1)
                show_cover(lib_cover_off((u32)sel));
            book_init((u32)sel);
            reader((u32)sel, sram_valid() ? load_book_page((u32)sel) : 0);
            if (L.nbooks == 1)
                break;              /* B in reader: back to title */
        }
    }
}
