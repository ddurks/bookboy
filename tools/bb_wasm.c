/* bookboyadvance wasm interface.
 *
 * Unity build: pulls in the epub text model + the full typesetting driver
 * with native-only pieces fenced off by BB_WASM. The browser feeds
 * paragraphs (it owns zip + DOMParser); this module owns cleaning,
 * typesetting, pagination, covers, previews, and blob emission.
 *
 * JS call order:
 *   FS.writeFile('/fonts/...', ...); FS.writeFile('/hyph_en_US.dic', ...)
 *   bw_reset()
 *   per book:  bw_book_begin(title) ; per spine file: bw_chapter_break(),
 *              then per paragraph: bw_para_begin(kind, indent_hint),
 *              bw_para_run(italic, utf8, len)..., bw_para_end()
 *              ...finally bw_book_end()
 *   bw_prepare(font_px, gamma_x100)          -> glyph count
 *   bw_layout(book, coverRGB|0, cw, ch)      -> npages
 *   bw_render_page(book, page, rgbaOut)      -> 1 (240*160*4 bytes)
 *   bw_finalize()                            -> books+library built
 *   bw_part_size(i) / bw_part_data(i)        -> 0 font 1 books 2 library 3 mini
 */
#define BB_WASM
#include "bb_epub.c"
#include "bookbuild.c"

#include <emscripten/emscripten.h>

#define MAX_WBOOKS 64

static struct {
    Book book;
    BookMeta meta;
    Paginator pg;
    TocEnt *toc;
    int ntoc;
    u8 *blob;
    size_t blob_size;
    u8 *cover;
    u8 *cover2;
    int cover2_len;
    u8 enc_title[80];
    int enc_len;
    int npages;
    int laid_out;
} W[MAX_WBOOKS];
static int nW;

static Chapter cur_ch;
static Para cur_pa;
static int have_pa;
static Kind prev_kind_in_ch;

static Font wfont;
static MiniFont wmini;
static int prepared;

static u8 *out_books;
static size_t out_books_size;
static u8 *out_library;
static size_t out_library_size;
static u8 *out_font;
static size_t out_font_size;
static u8 *out_mini;
static size_t out_mini_size;

static void flush_chapter(void)
{
    if (cur_ch.paras.n) {
        VPUSH(W[nW].book.v, cur_ch);
        memset(&cur_ch, 0, sizeof(cur_ch));
    }
    prev_kind_in_ch = (Kind)-1;
}

EMSCRIPTEN_KEEPALIVE
void bw_reset(void)
{
    /* leak-tolerant reset: the page reloads the module for a fresh session */
    memset(W, 0, sizeof(W));
    nW = 0;
    memset(&cur_ch, 0, sizeof(cur_ch));
    memset(&cur_pa, 0, sizeof(cur_pa));
    have_pa = 0;
    prepared = 0;
    ncharset = 0;
    if (cp_counts)
        memset(cp_counts, 0, 0x10000 * sizeof(u64));
}

EMSCRIPTEN_KEEPALIVE
int bw_book_begin(const char *title_utf8)
{
    if (nW >= MAX_WBOOKS)
        return -1;
    BookMeta *m = &W[nW].meta;
    u32 buf[512];
    int n = decode_data_cap(title_utf8, (int)strlen(title_utf8), buf, 512);
    int mlen = 0;
    for (int i = 0; i < n && mlen < 127; i++) {
        u32 c = buf[i];
        if (is_uspace(c)) {
            if (mlen > 0 && m->title[mlen - 1] != ' ')
                m->title[mlen++] = ' ';
        } else {
            m->title[mlen++] = c;
        }
    }
    while (mlen > 0 && m->title[mlen - 1] == ' ')
        mlen--;
    m->title_len = mlen;
    prev_kind_in_ch = (Kind)-1;
    return nW;
}

EMSCRIPTEN_KEEPALIVE
void bw_chapter_break(void)
{
    flush_chapter();
}

EMSCRIPTEN_KEEPALIVE
void bw_para_begin(int kind, int indent_hint)
{
    memset(&cur_pa, 0, sizeof(cur_pa));
    cur_pa.kind = (Kind)kind;
    cur_pa.indent = indent_hint;    /* resolved in bw_para_end */
    have_pa = 1;
}

EMSCRIPTEN_KEEPALIVE
void bw_para_run(int italic, const char *utf8, int len)
{
    if (!have_pa || len <= 0)
        return;
    u32 *buf = malloc((size_t)len * 4 + 4);
    int n = 0, i = 0;
    while (i < len) {
        u32 cp;
        utf8_decode((const u8 *)utf8, len, &i, &cp);
        buf[n++] = cp;
    }
    Run raw = {(u8)(italic != 0), buf, n};
    Run cleaned = clean_run(&raw);
    free(buf);
    if (cleaned.len)
        VPUSH(cur_pa.runs, cleaned);
    else
        free(cleaned.cp);
}

EMSCRIPTEN_KEEPALIVE
void bw_para_end(void)
{
    if (!have_pa)
        return;
    have_pa = 0;
    int nonblank = 0;
    for (int r = 0; r < cur_pa.runs.n && !nonblank; r++)
        for (int k = 0; k < cur_pa.runs.v[r].len; k++)
            if (!is_uspace(cur_pa.runs.v[r].cp[k])) {
                nonblank = 1;
                break;
            }
    if (!cur_pa.runs.n || !nonblank) {
        para_clear(&cur_pa);
        return;
    }
    if (cur_pa.kind == K_BODY) {
        if (cur_pa.indent < 0)      /* auto: indent unless after heading */
            cur_pa.indent = (prev_kind_in_ch == K_BODY);
    } else {
        cur_pa.indent = 0;
    }
    prev_kind_in_ch = cur_pa.kind;
    VPUSH(cur_ch.paras, cur_pa);
    memset(&cur_pa, 0, sizeof(cur_pa));
}

EMSCRIPTEN_KEEPALIVE
int bw_book_end(void)
{
    flush_chapter();
    if (!W[nW].book.v.n)
        return -1;
    count_book_chars(&W[nW].book);
    nW++;
    return nW - 1;
}

EMSCRIPTEN_KEEPALIVE
int bw_prepare(int font_px, int gamma_x100)
{
    if (prepared)                   /* idempotent: re-running would overrun
                                       charset[] and leak the font/hyphen */
        return ncharset;
    font_set_sources("/fonts/XCharter-Roman.otf", 0,
                     "/fonts/XCharter-Italic.otf", 0,
                     "/fonts/XCharter-Bold.otf", 0);
    build_codepage();
    hyphen_load("/hyph_en_US.dic");
    font_build(&wfont, font_px, gamma_x100 / 100.0, charset, ncharset);
    mini_build(&wmini);
    prepared = 1;
    return ncharset;
}

EMSCRIPTEN_KEEPALIVE
int bw_layout(int bi, const u8 *cover_rgb, int cw, int ch)
{
    if (!prepared || bi < 0 || bi >= nW || W[bi].laid_out)
        return -1;
    W[bi].toc = calloc(600, sizeof(TocEnt));
    paginate(&wfont, &W[bi].book, &W[bi].pg, W[bi].toc, &W[bi].ntoc);
    if (W[bi].ntoc == 0) {
        W[bi].toc[0].page = 0;
        W[bi].toc[0].title_len =
            W[bi].meta.title_len < 127 ? W[bi].meta.title_len : 127;
        memcpy(W[bi].toc[0].title, W[bi].meta.title,
               (size_t)W[bi].toc[0].title_len * 4);
        W[bi].ntoc = 1;
    }
    W[bi].npages = W[bi].pg.pages.n;
    FILE *ms = open_memstream((char **)&W[bi].blob, &W[bi].blob_size);
    emit_book_stream(&wfont, &W[bi].pg, W[bi].toc, W[bi].ntoc, ms);
    fclose(ms);
    if (cover_rgb && cw > 0 && ch > 0) {
        W[bi].cover = cover_screen_from_rgb(cover_rgb, cw, ch);
        W[bi].cover2 = cover2_encode(cover_rgb, cw, ch, &W[bi].cover2_len);
    }
    W[bi].enc_len = encode_title(&wfont, &W[bi].meta, W[bi].enc_title);
    W[bi].laid_out = 1;
    return W[bi].npages;
}

/* replay one page into an RGBA canvas (paperback palette) */
EMSCRIPTEN_KEEPALIVE
int bw_render_page(int bi, int page, u8 *rgba_out)
{
    if (bi < 0 || bi >= nW || !W[bi].laid_out || page < 0 ||
        page >= W[bi].npages)
        return 0;
    u8 *canvas = calloc(SCREEN_W * SCREEN_H, 1);
    const PageV *pv = &W[bi].pg.pages.v[page];
    for (int l = 0; l < pv->lines.n; l++) {
        const Line *ln = &pv->lines.v[l].line;
        int baseline = pv->lines.v[l].baseline;
        PenSim sim;
        sim_init(&sim, &wfont, 0);
        sim.fp = ln->x << 4;
        for (int si = 0; si < ln->segs.n; si++) {
            const Seg *s = &ln->segs.v[si];
            if (s->is_sp) {
                sim.fp = (snap_fp(sim.fp) + s->sp_px) << 4;
                sim.prev = -1;
                continue;
            }
            int st = s->italic ? 1 : 0;
            if (st != sim.style) { sim.style = st; sim.prev = -1; }
            for (int k = 0; k < s->len; k++) {
                int gi = font_index(&wfont, s->cp[k]);
                sim.fp += font_kern_fp(&wfont, sim.prev, gi, st);
                int x = snap_fp(sim.fp);
                const Glyph *g = &wfont.g[st][gi];
                for (int r = 0; r < g->h; r++)
                    for (int c = 0; c < g->w; c++) {
                        int a = g->bm[r * g->w + c];
                        int X = x + g->bx + c, Y = baseline + g->by + r;
                        if (a && X >= 0 && X < SCREEN_W && Y >= 0 &&
                            Y < SCREEN_H && a > canvas[Y * SCREEN_W + X])
                            canvas[Y * SCREEN_W + X] = (u8)a;
                    }
                sim.fp += g->adv_fp;
                sim.prev = gi;
            }
        }
    }
    /* footer, same as the device */
    char left[16], right[24];
    u32 pct = ((u32)(page + 1) * 100) / (u32)W[bi].npages;
    if (pct > 100) pct = 100;
    snprintf(left, sizeof(left), "%u%%", pct);
    int story = 0;
    for (int t = 0; t < W[bi].ntoc; t++)
        if (W[bi].toc[t].page <= page)
            story = t;
    int cstart = W[bi].toc[story].page;
    int cend = (story + 1 < W[bi].ntoc) ? W[bi].toc[story + 1].page
                                        : W[bi].npages;
    snprintf(right, sizeof(right), "%d / %d", page - cstart + 1,
             cend - cstart);
    mini_text(canvas, &wmini, left, MARGIN_L, FOOTER_BASE, 1);
    mini_text(canvas, &wmini, right,
              SCREEN_W - MARGIN_R - mini_text_width(&wmini, right),
              FOOTER_BASE, 1);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        double t = canvas[i] * 17 / 255.0;
        for (int c = 0; c < 3; c++)
            rgba_out[i * 4 + c] =
                (u8)round_half_even(PAPER[c] + (INK[c] - PAPER[c]) * t);
        rgba_out[i * 4 + 3] = 255;
    }
    free(canvas);
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int bw_finalize(void)
{
    if (!prepared)
        return -1;
    /* books.bin: blobs + covers, 4-aligned, insertion order (JS curates) */
    FILE *bf = open_memstream((char **)&out_books, &out_books_size);
    long pos = 0;
    u32 book_off[MAX_WBOOKS], cover_off[MAX_WBOOKS], cover2_off[MAX_WBOOKS];
    for (int i = 0; i < nW; i++) {
        if (!W[i].laid_out)
            return -2;
        book_off[i] = (u32)pos;
        fwrite(W[i].blob, 1, W[i].blob_size, bf);
        pos += (long)W[i].blob_size;
        while (pos & 3) { fputc(0, bf); pos++; }
        cover_off[i] = 0;
        if (W[i].cover) {
            cover_off[i] = (u32)pos;
            fwrite(W[i].cover, 1, COVER_BYTES, bf);
            pos += COVER_BYTES;
        }
        cover2_off[i] = 0;
        if (W[i].cover2) {
            cover2_off[i] = (u32)pos;
            fwrite(W[i].cover2, 1, (size_t)W[i].cover2_len, bf);
            pos += W[i].cover2_len;
            while (pos & 3) { fputc(0, bf); pos++; }
        }
    }
    fclose(bf);

    FILE *lf = open_memstream((char **)&out_library, &out_library_size);
    fwrite("BLB2", 1, 4, lf);
    w32(lf, (unsigned)nW);
    for (int i = 0; i < nW; i++) {
        w32(lf, book_off[i]);
        w32(lf, cover_off[i]);
        w32(lf, cover2_off[i]);
        w16(lf, (unsigned)W[i].npages);
        w8(lf, (unsigned)W[i].enc_len);
        w8(lf, 0);
        u8 tb[80] = {0};
        memcpy(tb, W[i].enc_title, (size_t)W[i].enc_len);
        fwrite(tb, 1, 80, lf);
    }
    fclose(lf);

    /* emit_font_bin/emit_mini_bin write to paths; MEMFS makes that cheap */
    emit_font_bin(&wfont, "/font.bin");
    emit_mini_bin(&wmini, "/mini.bin");
    FILE *rf = fopen("/font.bin", "rb");
    fseek(rf, 0, SEEK_END);
    out_font_size = (size_t)ftell(rf);
    fseek(rf, 0, SEEK_SET);
    out_font = malloc(out_font_size);
    fread(out_font, 1, out_font_size, rf);
    fclose(rf);
    rf = fopen("/mini.bin", "rb");
    fseek(rf, 0, SEEK_END);
    out_mini_size = (size_t)ftell(rf);
    fseek(rf, 0, SEEK_SET);
    out_mini = malloc(out_mini_size);
    fread(out_mini, 1, out_mini_size, rf);
    fclose(rf);
    return (int)(out_books_size + out_library_size + out_font_size +
                 out_mini_size);
}

EMSCRIPTEN_KEEPALIVE
int bw_part_size(int which)
{
    switch (which) {
    case 0: return (int)out_font_size;
    case 1: return (int)out_books_size;
    case 2: return (int)out_library_size;
    case 3: return (int)out_mini_size;
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE
const u8 *bw_part_data(int which)
{
    switch (which) {
    case 0: return out_font;
    case 1: return out_books;
    case 2: return out_library;
    case 3: return out_mini;
    }
    return NULL;
}

EMSCRIPTEN_KEEPALIVE
int bw_book_pages(int bi)
{
    return (bi >= 0 && bi < nW) ? W[bi].npages : 0;
}

EMSCRIPTEN_KEEPALIVE
int bw_book_blob_size(int bi)
{
    return (bi >= 0 && bi < nW) ? (int)W[bi].blob_size : 0;
}
