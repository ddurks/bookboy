/* FreeType rasterization + Charter.ttc kern extraction.
 *
 *  - bitmaps: FT_LOAD_DEFAULT + FT_RENDER_MODE_NORMAL, glyph placed at
 *    (bitmap_left, ascent - bitmap_top), zero borders cropped
 *  - advances: unhinted fractional, per the harfbuzz hb-ft convention
 *    (FT_Get_Advance with NO_HINTING, 16.16 -> 26.6 via (v + 512) >> 10),
 *    so letter spacing isn't quantized to hinted integer widths
 *  - kern: legacy 'kern' table pairs, scaled to fp4 (1/16 px)
 */
#include "bb.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H

#define CHARTER "/System/Library/Fonts/Supplemental/Charter.ttc"

/* font sources: slot 0 = regular, 1 = italic, 2 = bold (title screen).
 * Defaults to Apple's Charter.ttc; font_set_sources() swaps in per-file
 * faces (e.g. the freely-licensed XCharter OTFs for the web build). */
static struct {
    const char *path[3];
    int idx[3];
} SRC = {{CHARTER, CHARTER, CHARTER}, {0, 1, 3}};

void font_set_sources(const char *reg, int ri, const char *it, int ii,
                      const char *bold, int bi)
{
    SRC.path[0] = reg;  SRC.idx[0] = ri;
    SRC.path[1] = it;   SRC.idx[1] = ii;
    SRC.path[2] = bold; SRC.idx[2] = bi;
}

static FT_Library g_lib;

static FT_Library lib(void)
{
    if (!g_lib)
        FT_Init_FreeType(&g_lib);
    return g_lib;
}

static long hb_adv66(FT_Face face, FT_UInt gi)
{
    FT_Fixed a = 0;
    FT_Get_Advance(face, gi, FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING, &a);
    return (a + (1 << 9)) >> 10;
}

/* render one glyph; fills Glyph with cropped 4bpp-quantized bitmap and
 * baseline-relative bearings; adv left to caller */
static void raster_glyph(FT_Face face, u32 cp, double gamma, Glyph *out)
{
    FT_UInt gi = FT_Get_Char_Index(face, cp);
    FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT);
    FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
    FT_Bitmap *bm = &face->glyph->bitmap;
    int w = (int)bm->width, h = (int)bm->rows;
    int x0 = w, x1 = 0, y0 = h, y1 = 0;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            if (bm->buffer[r * bm->pitch + c]) {
                if (c < x0) x0 = c;
                if (c + 1 > x1) x1 = c + 1;
                if (r < y0) y0 = r;
                if (r + 1 > y1) y1 = r + 1;
            }
    if (x1 <= x0) {                 /* blank (space) */
        out->w = out->h = out->bx = out->by = 0;
        out->bm = NULL;
        return;
    }
    out->w = x1 - x0;
    out->h = y1 - y0;
    out->bx = face->glyph->bitmap_left + x0;
    out->by = -face->glyph->bitmap_top + y0;    /* baseline-relative */
    out->bm = malloc((size_t)out->w * out->h);
    for (int r = 0; r < out->h; r++)
        for (int c = 0; c < out->w; c++) {
            int a = bm->buffer[(r + y0) * bm->pitch + (c + x0)];
            double lifted = pow(a / 255.0, gamma);
            int q = (int)(lifted * 15.0 + 0.5);
            out->bm[r * out->w + c] = (u8)(q < 15 ? q : 15);
        }
}

/* ---------------- Charter.ttc 'kern' + 'head' parsing ---------------- */
static u16 be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

typedef struct { const u8 *data; size_t len; } Blob;

static Blob read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = malloc((size_t)n);
    fread(buf, 1, (size_t)n, f);
    fclose(f);
    return (Blob){buf, (size_t)n};
}

static const u8 *ttc_table(const Blob *tc, int font_idx, const char *tag,
                           u32 *out_len);

/* ---------------- GPOS PairPos kerning ----------------
 * Modern fonts (XCharter OTF among them) carry kerning in GPOS type-2
 * lookups instead of a legacy 'kern' table. We flatten every PairPos
 * subtable's XAdvance-on-first-glyph into the charset kern matrix. */

typedef struct { int gid2idx_size; const int *gid2idx; Font *f; int style;
                 int upm; int applied; } GposCtx;

static int popcount16(unsigned v)
{
    int n = 0;
    while (v) { n += v & 1; v >>= 1; }
    return n;
}

/* XAdvance offset (in u16s) within a value record, or -1 if absent */
static int xadv_slot(unsigned vf)
{
    if (!(vf & 0x0004))
        return -1;
    return popcount16(vf & 0x0003);
}

static void gpos_pair(GposCtx *c, int g1, int g2, int xadv_units)
{
    if (!xadv_units)
        return;
    int li = (g1 < c->gid2idx_size) ? c->gid2idx[g1] : -1;
    int ri = (g2 < c->gid2idx_size) ? c->gid2idx[g2] : -1;
    if (li < 0 || ri < 0)
        return;
    long fp = round_half_even((double)xadv_units * c->f->px / c->upm * 16);
    if (!fp)
        return;
    if (fp < -127) fp = -127;
    if (fp > 127) fp = 127;
    int n = c->f->nchars;
    s8 *cell = &c->f->kern[c->style][li * n + ri];
    if (*cell == 0) {
        c->f->kern_pairs[c->style]++;
        *cell = (s8)fp;
        c->applied++;
    }
}

/* coverage table -> malloc'd glyph list (caller frees), returns count */
static int read_coverage(const u8 *cov, int **out)
{
    int fmt = be16(cov);
    int n = 0;
    int *g = NULL;
    if (fmt == 1) {
        n = be16(cov + 2);
        g = malloc((size_t)n * sizeof(int));
        for (int i = 0; i < n; i++)
            g[i] = be16(cov + 4 + 2 * i);
    } else if (fmt == 2) {
        int ranges = be16(cov + 2);
        int cap = 0;
        for (int r = 0; r < ranges; r++)
            cap += be16(cov + 4 + 6 * r + 2) - be16(cov + 4 + 6 * r) + 1;
        g = malloc((size_t)(cap > 0 ? cap : 1) * sizeof(int));
        for (int r = 0; r < ranges; r++) {
            int s = be16(cov + 4 + 6 * r), e = be16(cov + 4 + 6 * r + 2);
            for (int gid = s; gid <= e && n < cap; gid++)
                g[n++] = gid;
        }
    }
    *out = g;
    return n;
}

static int classdef_of(const u8 *cd, int gid)
{
    int fmt = be16(cd);
    if (fmt == 1) {
        int start = be16(cd + 2), cnt = be16(cd + 4);
        if (gid >= start && gid < start + cnt)
            return be16(cd + 6 + 2 * (gid - start));
        return 0;
    }
    if (fmt == 2) {
        int ranges = be16(cd + 2);
        for (int r = 0; r < ranges; r++) {
            int s = be16(cd + 4 + 6 * r), e = be16(cd + 4 + 6 * r + 2);
            if (gid >= s && gid <= e)
                return be16(cd + 4 + 6 * r + 4);
        }
    }
    return 0;
}

static void gpos_pairpos_subtable(GposCtx *c, const u8 *st,
                                  const u32 *charset_gids, int ncs)
{
    int fmt = be16(st);
    unsigned vf1 = 0, vf2 = 0;
    if (fmt == 1) {
        vf1 = be16(st + 4);
        vf2 = be16(st + 6);
        int slot = xadv_slot(vf1);
        if (slot < 0)
            return;
        int v1n = popcount16(vf1), v2n = popcount16(vf2);
        int rec_sz = 2 + 2 * (v1n + v2n);
        int *cov;
        int ncov = read_coverage(st + be16(st + 2), &cov);
        int npairsets = be16(st + 8);
        for (int i = 0; i < ncov && i < npairsets; i++) {
            const u8 *ps = st + be16(st + 10 + 2 * i);
            int cnt = be16(ps);
            for (int k = 0; k < cnt; k++) {
                const u8 *rec = ps + 2 + k * rec_sz;
                int g2 = be16(rec);
                int xadv = (s16)be16(rec + 2 + 2 * slot);
                gpos_pair(c, cov[i], g2, xadv);
            }
        }
        free(cov);
    } else if (fmt == 2) {
        vf1 = be16(st + 4);
        vf2 = be16(st + 6);
        int slot = xadv_slot(vf1);
        if (slot < 0)
            return;
        int v1n = popcount16(vf1), v2n = popcount16(vf2);
        int rec_sz = 2 * (v1n + v2n);
        const u8 *cd1 = st + be16(st + 8);
        const u8 *cd2 = st + be16(st + 10);
        int c1n = be16(st + 12), c2n = be16(st + 14);
        const u8 *matrix = st + 16;
        int *cov;
        int ncov = read_coverage(st + be16(st + 2), &cov);
        /* pairs restricted to charset glyphs on both sides */
        for (int i = 0; i < ncov; i++) {
            int g1 = cov[i];
            int cls1 = classdef_of(cd1, g1);
            if (cls1 >= c1n)
                continue;
            for (int j = 0; j < ncs; j++) {
                int g2 = (int)charset_gids[j];
                if (!g2)
                    continue;
                int cls2 = classdef_of(cd2, g2);
                if (cls2 >= c2n)
                    continue;
                const u8 *rec = matrix + (size_t)(cls1 * c2n + cls2) * rec_sz;
                int xadv = (s16)be16(rec + 2 * slot);
                gpos_pair(c, g1, g2, xadv);
            }
        }
        free(cov);
    }
}

static void load_gpos_kern(Font *f, const Blob *tc, int font_idx,
                           FT_Face face, int style, const int *gid2idx,
                           int upm)
{
    u32 glen = 0;
    const u8 *gpos = ttc_table(tc, font_idx, "GPOS", &glen);
    if (!gpos)
        return;
    GposCtx c = {65536, gid2idx, f, style, upm, 0};
    /* charset glyph ids for the class-based walk */
    u32 charset_gids[MAX_GLYPHS];
    for (int i = 0; i < f->nchars; i++)
        charset_gids[i] = FT_Get_Char_Index(face, f->chars[i]);
    const u8 *ll = gpos + be16(gpos + 8);
    int nlookups = be16(ll);
    for (int i = 0; i < nlookups; i++) {
        const u8 *lk = ll + be16(ll + 2 + 2 * i);
        int type = be16(lk);
        int nsub = be16(lk + 4);
        for (int s = 0; s < nsub; s++) {
            const u8 *st = lk + be16(lk + 6 + 2 * s);
            if (type == 2) {
                gpos_pairpos_subtable(&c, st, charset_gids, f->nchars);
            } else if (type == 9 && be16(st) == 1 && be16(st + 2) == 2) {
                /* extension positioning wrapping a PairPos */
                gpos_pairpos_subtable(&c, st + be32(st + 4), charset_gids,
                                      f->nchars);
            }
        }
    }
}

static const u8 *ttc_table(const Blob *tc, int font_idx, const char *tag,
                           u32 *out_len)
{
    const u8 *p = tc->data;
    u32 font_off;
    if (!memcmp(p, "ttcf", 4)) {
        u32 nfonts = be32(p + 8);
        if ((u32)font_idx >= nfonts) return NULL;
        font_off = be32(p + 12 + 4 * (u32)font_idx);
    } else {
        font_off = 0;
    }
    const u8 *sf = p + font_off;
    u16 ntab = be16(sf + 4);
    for (u16 i = 0; i < ntab; i++) {
        const u8 *rec = sf + 12 + 16 * i;
        if (!memcmp(rec, tag, 4)) {
            if (out_len) *out_len = be32(rec + 12);
            return p + be32(rec + 8);
        }
    }
    return NULL;
}

/* load kern pairs for one face into the font's charset-index matrix:
 * legacy 'kern' table when present, GPOS PairPos otherwise */
static void load_kern(Font *f, const Blob *tc, int ttc_idx, FT_Face face,
                      int style)
{
    int n = f->nchars;
    f->kern[style] = calloc((size_t)n * n, 1);
    f->kern_pairs[style] = 0;

    const u8 *head = ttc_table(tc, ttc_idx, "head", NULL);
    if (!head) return;
    int upm = be16(head + 18);

    /* gid -> charset index, lowest codepoint wins (charset is sorted);
     * static: 256KB would overflow the wasm stack */
    static int gid2idx[65536];
    for (int i = 0; i < 65536; i++) gid2idx[i] = -1;
    for (int i = 0; i < n; i++) {
        FT_UInt gi = FT_Get_Char_Index(face, f->chars[i]);
        if (gi && gi < 65536 && gid2idx[gi] < 0)
            gid2idx[gi] = i;
    }

    /* legacy 'kern' first (keeps Apple Charter output stable with shipped
     * ROMs — its GPOS is the same 203-pair set with micro-different
     * values); GPOS PairPos as fallback for fonts without a legacy table
     * (XCharter and most modern OTFs) */
    u32 klen = 0;
    const u8 *k = ttc_table(tc, ttc_idx, "kern", &klen);
    if (!k) {
        load_gpos_kern(f, tc, ttc_idx, face, style, gid2idx, upm);
        return;
    }
    u16 ntables = be16(k + 2);
    const u8 *sub = k + 4;
    for (u16 t = 0; t < ntables; t++) {
        u16 sublen = be16(sub + 2);
        u16 coverage = be16(sub + 4);
        int format = coverage >> 8;
        if (format == 0) {
            u16 npairs = be16(sub + 6);
            const u8 *pair = sub + 14;
            for (u16 i = 0; i < npairs; i++, pair += 6) {
                int l = be16(pair), r = be16(pair + 2);
                int v = (s16)be16(pair + 4);
                if (!v) continue;
                int li = gid2idx[l & 0xFFFF], ri = gid2idx[r & 0xFFFF];
                if (li < 0 || ri < 0) continue;
                long fp = round_half_even((double)v * f->px / upm * 16);
                if (!fp) continue;
                if (fp < -127) fp = -127;
                if (fp > 127) fp = 127;
                if (f->kern[style][li * n + ri] == 0)
                    f->kern_pairs[style]++;
                f->kern[style][li * n + ri] = (s8)fp;
            }
        }
        sub += sublen;
    }
}

void font_build(Font *f, int px, double gamma, const u32 *charset, int n)
{
    f->px = px;
    f->gamma = gamma;
    f->nchars = n;
    memcpy(f->chars, charset, (size_t)n * sizeof(u32));
    f->question_idx = -1;
    for (int i = 0; i < n; i++)
        if (charset[i] == '?')
            f->question_idx = i;

    for (int st = 0; st < 2; st++) {
        Blob tc = read_file(SRC.path[st]);
        FT_Face face;
        if (FT_New_Face(lib(), SRC.path[st], SRC.idx[st], &face)) {
            fprintf(stderr, "cannot open font %s\n", SRC.path[st]);
            exit(1);
        }
        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px);
        for (int i = 0; i < n; i++) {
            raster_glyph(face, charset[i], gamma, &f->g[st][i]);
            FT_UInt gi = FT_Get_Char_Index(face, charset[i]);
            f->g[st][i].adv_fp = round_q4_even((int)hb_adv66(face, gi));
        }
        load_kern(f, &tc, SRC.idx[st], face, st);
        FT_Done_Face(face);
        free((void *)tc.data);
    }

    /* tight ink metrics across both styles (nominal FT ascent/descent
     * reserve accent room and read too loose on a 160px screen); measured
     * over ASCII only so accented capitals don't inflate the leading —
     * their accents may poke into the line gap, as in print */
    int top = 1 << 30, bot = -(1 << 30);
    for (int st = 0; st < 2; st++)
        for (int i = 0; i < n; i++) {
            const Glyph *g = &f->g[st][i];
            if (!g->h || charset[i] > 0x7E)
                continue;
            if (g->by < top) top = g->by;
            if (g->by + g->h > bot) bot = g->by + g->h;
        }
    f->ascent = -top;
    f->descent = bot;
    /* 1px inter-line leading. At 14px this makes line_height 16, which fits 9
     * body lines on the 160px screen; 2px left only 8 with an awkward gap. */
    f->line_height = f->ascent + f->descent + 1;
    f->space_fp = f->g[0][font_index(f, ' ')].adv_fp;
}

int font_index(const Font *f, u32 cp)
{
    int lo = 0, hi = f->nchars - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->chars[mid] == cp) return mid;
        if (f->chars[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return f->question_idx;
}

int font_kern_fp(const Font *f, int prev_idx, int idx, int style)
{
    if (prev_idx < 0) return 0;
    return f->kern[style][prev_idx * f->nchars + idx];
}

void mini_build(MiniFont *m)
{
    static const char chars[] = MINI_CHARS;
    FT_Face face;
    FT_New_Face(lib(), SRC.path[0], SRC.idx[0], &face);
    FT_Set_Pixel_Sizes(face, 0, 10);
    for (int i = 0; i < MINI_N; i++) {
        raster_glyph(face, (u32)chars[i], 0.72, &m->g[i]);
        FT_UInt gi = FT_Get_Char_Index(face, (u32)chars[i]);
        m->g[i].adv_fp = (int)round_half_even(hb_adv66(face, gi) / 64.0);
    }
    FT_Done_Face(face);
}

int charter_has_glyph(u32 cp)
{
    static FT_Face face;
    if (!face) {
        FT_New_Face(lib(), SRC.path[0], SRC.idx[0], &face);
        FT_Set_Pixel_Sizes(face, 0, 14);
    }
    return FT_Get_Char_Index(face, cp) != 0;
}

/* ------------- title-screen text (mask-blended onto the L canvas) ------ */
long title_char_adv66(u32 cp, int px_size)
{
    FT_Face face;
    FT_New_Face(lib(), SRC.path[2], SRC.idx[2], &face);
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px_size);
    long a = hb_adv66(face, FT_Get_Char_Index(face, cp));
    FT_Done_Face(face);
    return a;
}

int face_ascent(int px_size)
{
    FT_Face face;
    FT_New_Face(lib(), SRC.path[2], SRC.idx[2], &face);
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px_size);
    int a = (int)(face->size->metrics.ascender >> 6);
    FT_Done_Face(face);
    return a;
}

#define SHIFTFORDIV255(a) ((((a) >> 8) + (a)) >> 8)

void title_draw_char(ImgL *img, int x, int y, u32 cp, int px_size, int fill)
{
    FT_Face face;
    FT_New_Face(lib(), SRC.path[2], SRC.idx[2], &face);
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px_size);
    int asc = (int)(face->size->metrics.ascender >> 6);
    FT_Load_Glyph(face, FT_Get_Char_Index(face, cp), FT_LOAD_DEFAULT);
    FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
    FT_Bitmap *bm = &face->glyph->bitmap;
    int gx = x + face->glyph->bitmap_left;
    int gy = y + asc - face->glyph->bitmap_top;
    for (int r = 0; r < (int)bm->rows; r++) {
        int Y = gy + r;
        if (Y < 0 || Y >= img->h) continue;
        for (int c = 0; c < (int)bm->width; c++) {
            int X = gx + c;
            if (X < 0 || X >= img->w) continue;
            int mask = bm->buffer[r * bm->pitch + c];
            if (!mask) continue;
            int in1 = img->px[Y * img->w + X];
            int tmp = in1 * (255 - mask) + fill * mask + 128;
            img->px[Y * img->w + X] = (u8)SHIFTFORDIV255(tmp);
        }
    }
    FT_Done_Face(face);
}
