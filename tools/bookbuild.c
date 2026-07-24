/* bookboyadvance builder — main driver: layout, pagination, emission.
 *
 * Usage: bookbuild [project_root]   (default ".")
 * Env: FONT_PX (14), FONT_GAMMA (0.72)
 */
#include "bb.h"
#include "bb_art.h"

int utf8_encode(u32 cp, char *out);

static char ROOT[900] = ".";
static char PATHBUF[1024];

static const char *rootpath(const char *rel)
{
    snprintf(PATHBUF, sizeof(PATHBUF), "%s/%s", ROOT, rel);
    return PATHBUF;
}

/* ---------------- fragments ---------------- */
typedef enum { J_NONE, J_SPACE, J_SOFT, J_DASH, J_END } Join;

typedef struct {
    VEC(Run) pieces;
    Join join;
} Frag;

typedef VEC(Frag) FragVec;

static void frag_push_piece(Frag *f, u8 italic, const u32 *cp, int len)
{
    Run r;
    r.italic = italic;
    r.cp = malloc((size_t)len * 4 + 4);
    memcpy(r.cp, cp, (size_t)len * 4);
    r.len = len;
    VPUSH(f->pieces, r);
}

static void frag_free(Frag *f)
{
    for (int i = 0; i < f->pieces.n; i++)
        free(f->pieces.v[i].cp);
    free(f->pieces.v);
}

static int is_alpha(u32 c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

/* one word (piece list) -> frags with soft/dash joins (split_word_pieces) */
static void split_word_pieces(const Run *pieces, int npieces, FragVec *out)
{
    int first_out = out->n;
    for (int pi = 0; pi < npieces; pi++) {
        const Run *p = &pieces[pi];
        int last_piece = pi == npieces - 1;
        /* chunks: split after em-dash / hyphen */
        int cstart = 0;
        while (cstart < p->len) {
            int cend = cstart;
            while (cend < p->len) {
                u32 c = p->cp[cend++];
                if (c == 0x2014 || c == '-')
                    break;
            }
            const u32 *chunk = p->cp + cstart;
            int clen = cend - cstart;
            int last_chunk = cend >= p->len;
            cstart = cend;
            if (clen == 0)
                continue;
            u32 lastc = chunk[clen - 1];
            if (lastc == 0x2014 || lastc == '-') {
                Frag f = {{0}, J_DASH};
                frag_push_piece(&f, p->italic, chunk, clen);
                VPUSH(*out, f);
                continue;
            }
            /* ^([^A-Za-z]*)([A-Za-z’']+)([^A-Za-z]*)$ */
            int a = 0;
            while (a < clen && !is_alpha(chunk[a]))
                a++;
            int b = a;
            while (b < clen &&
                   (is_alpha(chunk[b]) || chunk[b] == 0x2019 || chunk[b] == '\''))
                b++;
            int post_ok = 1;
            for (int k = b; k < clen; k++)
                if (is_alpha(chunk[k]))
                    post_ok = 0;
            int core_len = b - a;
            Join endjoin = (last_piece && last_chunk) ? J_END : J_NONE;
            if (a < clen && post_ok && core_len >= 6) {
                /* hyphenate core (’ -> ') */
                u32 core[200];
                for (int k = 0; k < core_len && k < 200; k++)
                    core[k] = chunk[a + k] == 0x2019 ? '\'' : chunk[a + k];
                int pos[64];
                int npos_all = hyphen_positions(core, core_len, pos, 64);
                /* extra filter: p >= 2 and len - p >= 3 */
                int npos = 0;
                for (int k = 0; k < npos_all; k++)
                    if (pos[k] >= 2 && core_len - pos[k] >= 3)
                        pos[npos++] = pos[k];
                int prev = 0;
                for (int j = 0; j <= npos; j++) {
                    int end = (j < npos) ? pos[j] : core_len;
                    u32 txt[512];
                    int cap = (int)(sizeof(txt) / sizeof(txt[0]));
                    int tn = 0;
                    if (j == 0)
                        for (int k = 0; k < a && tn < cap; k++)
                            txt[tn++] = chunk[k];
                    for (int k = prev; k < end && tn < cap; k++)
                        txt[tn++] = chunk[a + k];
                    if (j == npos)
                        for (int k = b; k < clen && tn < cap; k++)
                            txt[tn++] = chunk[k];
                    prev = end;
                    Frag f = {{0}, (j < npos) ? J_SOFT : endjoin};
                    frag_push_piece(&f, p->italic, txt, tn);
                    VPUSH(*out, f);
                }
            } else {
                Frag f = {{0}, endjoin};
                frag_push_piece(&f, p->italic, chunk, clen);
                VPUSH(*out, f);
            }
        }
    }
    /* inner END markers -> NONE (all but the very last frag) */
    for (int i = first_out; i < out->n - 1; i++)
        if (out->v[i].join == J_END)
            out->v[i].join = J_NONE;
}

/* paragraph -> flat frag list (para_frags) */
static void para_frags(const Para *para, FragVec *out)
{
    VEC(Run) cur = {0};             /* borrowed slices, no copies */
    VEC(int) word_starts = {0};
    /* words: split runs on single spaces preserving style boundaries */
    typedef VEC(Run) Word;
    VEC(Word) words = {0};
    Word w = {0};
    for (int ri = 0; ri < para->runs.n; ri++) {
        const Run *r = &para->runs.v[ri];
        int start = 0;
        for (int i = 0; i <= r->len; i++) {
            if (i == r->len || r->cp[i] == ' ') {
                if (i > start) {
                    Run piece = {r->italic, r->cp + start, i - start};
                    VPUSH(w, piece);
                }
                if (i < r->len) {   /* the space token */
                    if (w.n) {
                        VPUSH(words, w);
                        memset(&w, 0, sizeof(w));
                    }
                }
                start = i + 1;
            }
        }
    }
    if (w.n)
        VPUSH(words, w);
    for (int wi = 0; wi < words.n; wi++) {
        int before = out->n;
        split_word_pieces(words.v[wi].v, words.v[wi].n, out);
        if (out->n > before)
            out->v[out->n - 1].join = (wi < words.n - 1) ? J_SPACE : J_END;
        free(words.v[wi].v);
    }
    free(words.v);
    free(cur.v);
    free(word_starts.v);
}

/* ---------------- pen simulator ---------------- */
typedef struct {
    const Font *f;
    int space_px;
    int fp;
    int prev;                       /* charset idx, -1 = none */
    int style;
    int gaps;
} PenSim;

static void sim_init(PenSim *s, const Font *f, int space_px)
{
    s->f = f;
    s->space_px = space_px;
    s->fp = 0;
    s->prev = -1;
    s->style = 0;
    s->gaps = 0;
}

static void sim_feed_piece(PenSim *s, u8 italic, const u32 *cp, int len)
{
    int st = italic ? 1 : 0;
    if (st != s->style) {
        s->style = st;
        s->prev = -1;
    }
    for (int i = 0; i < len; i++) {
        int idx = font_index(s->f, cp[i]);
        s->fp += font_kern_fp(s->f, s->prev, idx, st);
        s->fp += s->f->g[st][idx].adv_fp;
        s->prev = idx;
    }
}

static void sim_feed_frag(PenSim *s, const Frag *fr)
{
    for (int i = 0; i < fr->pieces.n; i++)
        sim_feed_piece(s, fr->pieces.v[i].italic, fr->pieces.v[i].cp,
                       fr->pieces.v[i].len);
}

static void sim_space(PenSim *s)
{
    s->fp = (snap_fp(s->fp) + s->space_px) << 4;
    s->prev = -1;
    s->gaps++;
}

static int sim_width_hyphen(const PenSim *s, int style)
{
    int hy = font_index(s->f, '-');
    int fp = s->fp + font_kern_fp(s->f, s->prev, hy, style) +
             s->f->g[style][hy].adv_fp;
    return snap_fp(fp);
}

/* ---------------- DP line breaking (break_para) ---------------- */
typedef struct { int i, j, soft; } BreakLine;

static int break_para(const Font *font, const Frag *frags, int n, int space_px,
                      int first_avail, int avail, BreakLine *out)
{
    double *dp = malloc((size_t)(n + 1) * sizeof(double));
    int *nj = malloc((size_t)(n + 1) * sizeof(int));
    int *ns = malloc((size_t)(n + 1) * sizeof(int));
    for (int i = 0; i <= n; i++) { dp[i] = INFINITY; nj[i] = -1; ns[i] = 0; }
    dp[n] = 0.0;
    for (int i = n - 1; i >= 0; i--) {
        int A = (i == 0) ? first_avail : avail;
        PenSim sim;
        sim_init(&sim, font, space_px);
        for (int j = i; j < n; j++) {
            sim_feed_frag(&sim, &frags[j]);
            Join jn = frags[j].join;
            int can_break = jn == J_SPACE || jn == J_SOFT || jn == J_DASH ||
                            jn == J_END;
            if (can_break) {
                int soft = jn == J_SOFT;
                int w;
                if (soft) {
                    const Run *lastp = &frags[j].pieces.v[frags[j].pieces.n - 1];
                    w = sim_width_hyphen(&sim, lastp->italic ? 1 : 0);
                } else {
                    w = snap_fp(sim.fp);
                }
                if (w > A && j > i)
                    break;
                int last = j == n - 1;
                double cost = last ? 0.0 : (double)(A - w) * (A - w);
                if (soft)
                    cost += HYPHEN_PENALTY;
                if (w > A)
                    cost += 10000000;
                double total = cost + dp[j + 1];
                if (total < dp[i]) {
                    dp[i] = total;
                    nj[i] = j;
                    ns[i] = soft;
                }
            }
            if (frags[j].join == J_SPACE)
                sim_space(&sim);
        }
        if (nj[i] < 0) {
            nj[i] = i;
            ns[i] = 0;
            dp[i] = 10000000 + dp[i + 1];
        }
    }
    int nlines = 0, i = 0;
    while (i < n) {
        out[nlines].i = i;
        out[nlines].j = nj[i];
        out[nlines].soft = ns[i];
        nlines++;
        i = nj[i] + 1;
    }
    free(dp); free(nj); free(ns);
    return nlines;
}

/* ---------------- lines & segs ---------------- */
typedef struct {
    u8 is_sp;
    u8 italic;
    int sp_px;
    u32 *cp;                        /* owned */
    int len;
} Seg;

typedef struct {
    int x;
    VEC(Seg) segs;
} Line;

static void line_add_text(Line *ln, u8 italic, const u32 *cp, int len)
{
    Seg s = {0, italic, 0, malloc((size_t)len * 4 + 4), len};
    memcpy(s.cp, cp, (size_t)len * 4);
    VPUSH(ln->segs, s);
}

static void line_add_sp(Line *ln, int px)
{
    Seg s = {1, 0, px, NULL, 0};
    VPUSH(ln->segs, s);
}

static void line_free(Line *ln)
{
    for (int i = 0; i < ln->segs.n; i++)
        free(ln->segs.v[i].cp);
    free(ln->segs.v);
}

/* build_line: assemble + justify */
static Line build_line(const Font *font, const Frag *frags, int i, int j,
                       int soft, int space_px, int avail, int x0, int justify)
{
    PenSim sim;
    sim_init(&sim, font, space_px);
    for (int k = i; k <= j; k++) {
        sim_feed_frag(&sim, &frags[k]);
        if (k < j && frags[k].join == J_SPACE)
            sim_space(&sim);
    }
    const Run *lastp = &frags[j].pieces.v[frags[j].pieces.n - 1];
    int width = soft ? sim_width_hyphen(&sim, lastp->italic ? 1 : 0)
                     : snap_fp(sim.fp);
    int gaps = sim.gaps;
    int gap_px[256];
    for (int g = 0; g < gaps && g < 256; g++)
        gap_px[g] = space_px;
    if (justify && gaps > 0) {
        int extra = avail - width;
        if (extra > 0) {
            int per = extra / gaps, rem = extra % gaps;
            for (int g = 0; g < gaps; g++)
                gap_px[g] = space_px + per + (g < rem ? 1 : 0);
        }
    }
    Line ln = {x0, {0}};
    int gi = 0;
    for (int k = i; k <= j; k++) {
        for (int pi = 0; pi < frags[k].pieces.n; pi++)
            line_add_text(&ln, frags[k].pieces.v[pi].italic,
                          frags[k].pieces.v[pi].cp, frags[k].pieces.v[pi].len);
        if (k < j && frags[k].join == J_SPACE)
            line_add_sp(&ln, gap_px[gi++]);
    }
    if (soft) {                     /* append '-' to final text seg */
        Seg *last = &ln.segs.v[ln.segs.n - 1];
        last->cp = realloc(last->cp, ((size_t)last->len + 1) * 4);
        last->cp[last->len++] = '-';
    }
    return ln;
}

/* measure_segs (heading path) */
static int measure_segs(const Font *font, const Seg *segs, int nsegs)
{
    PenSim sim;
    sim_init(&sim, font, 0);
    for (int i = 0; i < nsegs; i++) {
        if (segs[i].is_sp) {
            sim.fp = (snap_fp(sim.fp) + segs[i].sp_px) << 4;
            sim.prev = -1;
        } else {
            sim_feed_piece(&sim, segs[i].italic, segs[i].cp, segs[i].len);
        }
    }
    return snap_fp(sim.fp);
}

/* heading_segs into a Line-like seg buffer (no x yet) */
static void heading_segs(Line *ln, const u32 *text, int len, int italic,
                         int tracked, int space_px)
{
    if (!tracked) {
        line_add_text(ln, (u8)italic, text, len);
        return;
    }
    for (int i = 0; i < len; i++) {
        if (text[i] == ' ') {
            line_add_sp(ln, space_px + 1);
        } else {
            line_add_text(ln, (u8)italic, &text[i], 1);
            line_add_sp(ln, 1);
        }
    }
    if (ln->segs.n && ln->segs.v[ln->segs.n - 1].is_sp) {
        ln->segs.n--;               /* pop trailing sp */
    }
}

/* layout_heading -> appends Lines to out */
typedef VEC(Line) LineVec;

static void layout_heading(const Font *font, const Para *para, int space_px,
                           LineVec *out)
{
    int tracked = para->kind == K_TITLE || para->kind == K_PART;
    int italic = para->kind == K_BYLINE;
    int tl = para_textlen(para);
    u32 *text = malloc((size_t)tl * 4 + 4);
    para_text(para, text);
    /* strip */
    int s0 = 0, s1 = tl;
    while (s0 < s1 && text[s0] == ' ') s0++;
    while (s1 > s0 && text[s1 - 1] == ' ') s1--;
    text += 0;
    int style;
    if (italic) {
        style = 1;
    } else {
        style = 0;
        for (int i = 0; i < para->runs.n; i++)
            if (para->runs.v[i].italic)
                style = 1;
    }
    /* split words on single spaces */
    typedef struct { int start, len; } WSpan;
    VEC(WSpan) wsp = {0};
    int i = s0;
    while (i <= s1) {
        int st = i;
        while (i < s1 && text[i] != ' ')
            i++;
        WSpan sp = {st, i - st};
        VPUSH(wsp, sp);
        i++;
    }
    /* greedy wrap */
    VEC(int) cur = {0};             /* indices into wsp */
    u32 joinbuf[2048];
    const int JBCAP = 2048;
    VEC(WSpan) outlines = {0};      /* start/len in joinbuf-space: store spans of wsp indices */
    typedef struct { int wfirst, wcount; } LSpan;
    VEC(LSpan) lspans = {0};
    int wfirst = 0, wcount = 0;
    for (int wi2 = 0; wi2 < wsp.n; wi2++) {
        /* cand = cur + [w] joined */
        int jn = 0;
        for (int k = wfirst; k <= wi2 && jn < JBCAP; k++) {
            if (k > wfirst && jn < JBCAP)
                joinbuf[jn++] = ' ';
            for (int c = 0; c < wsp.v[k].len && jn < JBCAP; c++)
                joinbuf[jn++] = text[wsp.v[k].start + c];
        }
        Line probe = {0, {0}};
        heading_segs(&probe, joinbuf, jn, style, tracked, space_px);
        int w = measure_segs(font, probe.segs.v, probe.segs.n);
        line_free(&probe);
        if (wcount > 0 && w > TEXT_W) {
            LSpan ls = {wfirst, wcount};
            VPUSH(lspans, ls);
            wfirst = wi2;
            wcount = 1;
        } else {
            wcount++;
        }
    }
    if (wcount > 0) {
        LSpan ls = {wfirst, wcount};
        VPUSH(lspans, ls);
    }
    for (int li = 0; li < lspans.n; li++) {
        int jn = 0;
        for (int k = lspans.v[li].wfirst;
             k < lspans.v[li].wfirst + lspans.v[li].wcount && jn < JBCAP; k++) {
            if (k > lspans.v[li].wfirst && jn < JBCAP)
                joinbuf[jn++] = ' ';
            for (int c = 0; c < wsp.v[k].len && jn < JBCAP; c++)
                joinbuf[jn++] = text[wsp.v[k].start + c];
        }
        Line ln = {0, {0}};
        heading_segs(&ln, joinbuf, jn, style, tracked, space_px);
        int w = measure_segs(font, ln.segs.v, ln.segs.n);
        int off = (TEXT_W - w) / 2;
        if (off < 0)
            off = 0;
        ln.x = MARGIN_L + off;
        VPUSH(*out, ln);
    }
    free(wsp.v); free(cur.v); free(lspans.v); free(outlines.v);
    free(text);
}

/* layout_para -> appends Lines */
static void layout_para(const Font *font, const Para *para, int space_px,
                        LineVec *out)
{
    if (para->kind != K_BODY) {
        layout_heading(font, para, space_px, out);
        return;
    }
    int indent = para->indent ? font->px : 0;
    FragVec frags = {0};
    para_frags(para, &frags);
    if (!frags.n)
        return;
    BreakLine *breaks = malloc((size_t)frags.n * sizeof(BreakLine));
    int nb = break_para(font, frags.v, frags.n, space_px, TEXT_W - indent,
                        TEXT_W, breaks);
    for (int li = 0; li < nb; li++) {
        int first = li == 0, last = li == nb - 1;
        int avail = TEXT_W - (first ? indent : 0);
        int x0 = MARGIN_L + (first ? indent : 0);
        Line ln = build_line(font, frags.v, breaks[li].i, breaks[li].j,
                             breaks[li].soft, space_px, avail, x0, !last);
        VPUSH(*out, ln);
    }
    for (int i = 0; i < frags.n; i++)
        frag_free(&frags.v[i]);
    free(frags.v);
    free(breaks);
}

/* ---------------- title_case ---------------- */
static u32 cp_lower(u32 c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    return c;
}

static u32 cp_upper(u32 c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    return c;
}

/* title-case into out (capacity TocEnt.title = 128), returns length */
#define TITLE_CAP 127
static int title_case(const u32 *s, int len, u32 *out)
{
    static const char *small[] = {"a", "an", "the", "of", "and", "or", "in",
                                  "on", "at", "to", "under", "for", "with"};
    /* strip + lower + split on whitespace runs */
    u32 buf[512];
    int n = 0;
    for (int i = 0; i < len && n < 512; i++)
        buf[n++] = cp_lower(s[i]);
    /* words */
    typedef struct { int start, len; } Sp;
    Sp words[64];
    int nw = 0, i = 0;
    while (i < n) {
        while (i < n && buf[i] == ' ') i++;
        if (i >= n) break;
        int st = i;
        while (i < n && buf[i] != ' ') i++;
        if (nw < 64) { words[nw].start = st; words[nw].len = i - st; nw++; }
    }
    int on = 0;
    for (int w = 0; w < nw; w++) {
        int is_small = 0;
        if (w > 0 && w < nw - 1) {
            for (unsigned k = 0; k < sizeof(small) / sizeof(*small); k++) {
                int sl = (int)strlen(small[k]);
                if (sl == words[w].len) {
                    int eq = 1;
                    for (int c = 0; c < sl; c++)
                        if (buf[words[w].start + c] != (u32)small[k][c])
                            eq = 0;
                    if (eq) { is_small = 1; break; }
                }
            }
        }
        if (w > 0 && on < TITLE_CAP)
            out[on++] = ' ';
        for (int c = 0; c < words[w].len && on < TITLE_CAP; c++) {
            u32 ch = buf[words[w].start + c];
            if (!is_small && c == 0)
                ch = cp_upper(ch);
            out[on++] = ch;
        }
    }
    return on;
}

/* ---------------- pagination ---------------- */
typedef struct { int baseline; Line line; } Placed;
typedef struct { VEC(Placed) lines; } PageV;

typedef struct {
    VEC(PageV) pages;
    PageV cur;
    int row;
    int slots, top_base, line_h;
} Paginator;

typedef struct { u32 title[128]; int title_len; int page; } TocEnt;

static void pg_flush(Paginator *pg)
{
    if (pg->cur.lines.n) {
        VPUSH(pg->pages, pg->cur);
        memset(&pg->cur, 0, sizeof(pg->cur));
    }
    pg->cur.lines.n = 0;
    pg->row = 0;
}

static void pg_put(Paginator *pg, Line ln)
{
    if (pg->row >= pg->slots)
        pg_flush(pg);
    Placed p = {pg->top_base + pg->row * pg->line_h, ln};
    VPUSH(pg->cur.lines, p);
    pg->row++;
}

static void pg_skip(Paginator *pg, int nrows)
{
    pg->row += nrows;
    if (pg->row > pg->slots)
        pg->row = pg->slots;
}

static void paginate(const Font *font, const Book *book, Paginator *pg,
                     TocEnt *toc, int *ntoc)
{
    int space_px = snap_fp(font->space_fp);
    pg->line_h = font->line_height;
    pg->top_base = MARGIN_T + font->ascent;
    int max_baseline = SCREEN_H - FOOTER_H - font->descent;
    pg->slots = (max_baseline - pg->top_base) / pg->line_h + 1;
    *ntoc = 0;

    for (int ci = 0; ci < book->v.n; ci++) {
        const Chapter *ch = &book->v.v[ci];
        int standalone = 1;
        for (int i = 0; i < ch->paras.n; i++)
            if (ch->paras.v[i].kind != K_PART &&
                ch->paras.v[i].kind != K_PARTSUB)
                standalone = 0;
        if (standalone) {
            pg_flush(pg);
            int start = pg->pages.n;
            LineVec *groups = calloc((size_t)ch->paras.n, sizeof(LineVec));
            int total = ch->paras.n - 1;
            for (int i = 0; i < ch->paras.n; i++) {
                layout_para(font, &ch->paras.v[i], space_px, &groups[i]);
                total += groups[i].n;
            }
            int r = (pg->slots - total) / 2;
            pg->row = r > 0 ? r : 0;
            for (int i = 0; i < ch->paras.n; i++) {
                for (int l = 0; l < groups[i].n; l++)
                    pg_put(pg, groups[i].v[l]);
                if (i < ch->paras.n - 1)
                    pg_skip(pg, 1);
                free(groups[i].v);
            }
            free(groups);
            pg_flush(pg);
            const Para *p0 = &ch->paras.v[0];
            int tl = para_textlen(p0);
            u32 *txt = malloc((size_t)tl * 4 + 4);
            para_text(p0, txt);
            if (*ntoc < 599) {
                toc[*ntoc].title_len = title_case(txt, tl, toc[*ntoc].title);
                toc[*ntoc].page = start;
                (*ntoc)++;
            }
            free(txt);
            continue;
        }
        int prev_kind = -1;
        for (int pi = 0; pi < ch->paras.n; pi++) {
            const Para *p = &ch->paras.v[pi];
            if (p->kind == K_TITLE) {
                pg_flush(pg);
                pg_skip(pg, 1);
                /* toc title: fmh + following cst texts joined, clamped to
                 * the join buffer (title_case truncates to 127 anyway) */
                u32 joined[1024];
                int jl = para_textlen(p);
                if (jl > 1024) jl = 1024;
                para_text_cap(p, joined, jl);
                for (int qi = pi + 1; qi < ch->paras.n && jl < 1023; qi++) {
                    const Para *q = &ch->paras.v[qi];
                    if (q->kind != K_SUBTITLE)
                        break;
                    joined[jl++] = ' ';
                    int ql = para_textlen(q);
                    if (ql > 1024 - jl) ql = 1024 - jl;
                    para_text_cap(q, joined + jl, ql);
                    jl += ql;
                }
                if (*ntoc < 599) {
                    toc[*ntoc].title_len =
                        title_case(joined, jl, toc[*ntoc].title);
                    toc[*ntoc].page = pg->pages.n;
                    (*ntoc)++;
                }
            }
            if (prev_kind >= 0 && prev_kind != K_BODY && p->kind == K_BODY)
                pg_skip(pg, 1);
            if (p->kind == K_DISPLAY && prev_kind == K_BODY)
                pg_skip(pg, 1);
            LineVec lines = {0};
            layout_para(font, p, space_px, &lines);
            for (int l = 0; l < lines.n; l++)
                pg_put(pg, lines.v[l]);
            free(lines.v);
            prev_kind = (int)p->kind;
        }
        pg_flush(pg);
    }
}

/* ---------------- codepage (global across all books) ---------------- */
#define MAX_CODEPAGE 252            /* GID_BASE + n must stay <= 255 */
static u32 charset[MAX_GLYPHS];
static int ncharset;
static u64 *cp_counts;              /* [0x10000] BMP frequency */

static void count_book_chars(const Book *book)
{
    if (!cp_counts)
        cp_counts = calloc(0x10000, sizeof(u64));
    for (int ci = 0; ci < book->v.n; ci++)
        for (int pi = 0; pi < book->v.v[ci].paras.n; pi++) {
            const Para *p = &book->v.v[ci].paras.v[pi];
            for (int ri = 0; ri < p->runs.n; ri++)
                for (int k = 0; k < p->runs.v[ri].len; k++) {
                    u32 c = p->runs.v[ri].cp[k];
                    if (c < 0x10000 && c != '\n')
                        cp_counts[c]++;
                }
        }
}

static int cmp_u32(const void *a, const void *b)
{
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y;
}

static int is_seed(u32 c)
{
    return strchr(" ?-/0123456789", (int)c) != NULL || c == 0x2026;
}

static void build_codepage(void)
{
    ncharset = 0;                   /* idempotent if called again */
    /* seeds always present (layout + footer + title ellipsis) */
    const char *init = " ?-/0123456789";
    for (const char *c = init; *c; c++)
        charset[ncharset++] = (u32)*c;
    charset[ncharset++] = 0x2026;   /* … for truncated library titles */
    /* candidates by frequency, only chars Charter can draw */
    typedef struct { u32 cp; u64 n; } Cand;
    Cand *cands = malloc(0x10000 * sizeof(Cand));
    int nc = 0;
    u64 dropped_chars = 0;
    for (u32 c = 1; c < 0x10000; c++) {
        if (!cp_counts[c] || is_seed(c))
            continue;
        if (!charter_has_glyph(c)) {
            dropped_chars += cp_counts[c];
#ifndef BB_WASM
            fprintf(stderr, "  no glyph for U+%04X (x%llu) -> '?'\n", c,
                    (unsigned long long)cp_counts[c]);
#endif
            continue;
        }
        cands[nc].cp = c;
        cands[nc].n = cp_counts[c];
        nc++;
    }
    /* rarest first out if over budget */
    for (int i = 0; i < nc; i++)        /* insertion sort by count desc */
        for (int j = i; j > 0 && cands[j].n > cands[j - 1].n; j--) {
            Cand t = cands[j]; cands[j] = cands[j - 1]; cands[j - 1] = t;
        }
    for (int i = 0; i < nc; i++) {
        if (ncharset >= MAX_CODEPAGE) {
            fprintf(stderr, "  codepage full: dropping U+%04X (x%llu)\n",
                    cands[i].cp, (unsigned long long)cands[i].n);
            continue;
        }
        charset[ncharset++] = cands[i].cp;
    }
    free(cands);
    qsort(charset, (size_t)ncharset, 4, cmp_u32);
    (void)dropped_chars;
}

/* encode a line to at most `cap` bytes; the on-device length field is u8,
 * so cap must be <= 255. Layout keeps lines under the screen width, so this
 * only clamps pathological input rather than truncating real text. */
static int encode_line(const Font *font, const Line *ln, u8 *out, int cap)
{
    int n = 0, italic = 0;
    for (int si = 0; si < ln->segs.n; si++) {
        const Seg *s = &ln->segs.v[si];
        if (s->is_sp) {
            if (n + 2 > cap) break;
            int px = s->sp_px;
            if (px < 0) px = 0;
            if (px > 255) px = 255;
            out[n++] = OP_SPACE;
            out[n++] = (u8)px;
        } else {
            int it = s->italic ? 1 : 0;
            if (it != italic) {
                if (n + 1 > cap) break;
                out[n++] = OP_ITALIC;
                italic = it;
            }
            for (int k = 0; k < s->len && n < cap; k++)
                out[n++] = (u8)(GID_BASE + font_index(font, s->cp[k]));
        }
    }
    return n;
}

static long emit_font_bin(const Font *f, const char *path)
{
    FILE *fp = xfopen(path, "wb");
    int n = f->nchars;
    fwrite("GFN2", 1, 4, fp);
    w16(fp, (unsigned)n);
    w8(fp, (unsigned)f->line_height);
    w8(fp, (unsigned)f->ascent);
    w8(fp, (unsigned)snap_fp(f->space_fp));
    w8(fp, 0); w8(fp, 0); w8(fp, 0);
    u8 ascii_map[128] = {0};
    for (int i = 0; i < n; i++)
        if (f->chars[i] < 128)
            ascii_map[f->chars[i]] = (u8)(GID_BASE + i);
    fwrite(ascii_map, 1, 128, fp);
    for (int st = 0; st < 2; st++)
        for (int i = 0; i < n; i++)
            w16(fp, (unsigned)(f->g[st][i].adv_fp & 0xFFFF));
    for (int st = 0; st < 2; st++)
        fwrite(f->kern[st], 1, (size_t)n * n, fp);
    int pad = (4 - (2 * n * n) % 4) % 4;
    for (int i = 0; i < pad; i++)
        w8(fp, 0);
    long off = 0;
    for (int st = 0; st < 2; st++)
        for (int i = 0; i < n; i++) {
            w32(fp, (unsigned)off);
            const Glyph *g = &f->g[st][i];
            off += 4 + (size_t)((g->w + 1) / 2) * g->h;
        }
    for (int st = 0; st < 2; st++)
        for (int i = 0; i < n; i++) {
            const Glyph *g = &f->g[st][i];
            w8(fp, (unsigned)g->w);
            w8(fp, (unsigned)g->h);
            w8(fp, (unsigned)(g->bx & 0xFF));
            w8(fp, (unsigned)(g->by & 0xFF));
            int rb = (g->w + 1) / 2;
            for (int r = 0; r < g->h; r++) {
                for (int cbyte = 0; cbyte < rb; cbyte++) {
                    int c0 = cbyte * 2, c1 = c0 + 1;
                    unsigned v = g->bm[r * g->w + c0];
                    if (c1 < g->w)
                        v |= (unsigned)g->bm[r * g->w + c1] << 4;
                    w8(fp, v);
                }
            }
        }
    long sz = ftell(fp);
    fclose(fp);
    return sz;
}

static void emit_book_stream(const Font *font, const Paginator *pg,
                             const TocEnt *toc, int ntoc, FILE *f)
{
    int npages = pg->pages.n;
    /* encode pages first */
    u8 **blobs = malloc((size_t)npages * sizeof(u8 *));
    int *blens = malloc((size_t)npages * sizeof(int));
    u8 tmp[8192];
    for (int p = 0; p < npages; p++) {
        int n = 0;
        const PageV *page = &pg->pages.v[p];
        tmp[n++] = (u8)page->lines.n;
        for (int l = 0; l < page->lines.n; l++) {
            u8 enc[255];
            if (n + 3 + (int)sizeof(enc) > (int)sizeof(tmp))
                break;              /* page blob full (unreachable at 240px) */
            int el = encode_line(font, &page->lines.v[l].line, enc,
                                 sizeof(enc));
            tmp[n++] = (u8)page->lines.v[l].baseline;
            tmp[n++] = (u8)page->lines.v[l].line.x;
            tmp[n++] = (u8)el;
            memcpy(tmp + n, enc, (size_t)el);
            n += el;
        }
        blobs[p] = malloc((size_t)n);
        memcpy(blobs[p], tmp, (size_t)n);
        blens[p] = n;
    }
    int ntoc_e = ntoc < 512 ? ntoc : 512;
    fwrite("GBK1", 1, 4, f);
    w32(f, (unsigned)npages);
    w32(f, (unsigned)ntoc_e);
    w32(f, 16);
    for (int t = 0; t < ntoc_e; t++) {
        w16(f, (unsigned)toc[t].page);
        u8 tb[46] = {0};
        int bn = 0;
        for (int k = 0; k < toc[t].title_len && bn < 45; k++) {
            u32 c = toc[t].title[k];
            tb[bn++] = (u8)(c <= 255 ? c : '?');
        }
        fwrite(tb, 1, 46, f);
    }
    long offs_off = 16 + (long)ntoc_e * 48 + 4;
    w32(f, (unsigned)offs_off);
    long base = offs_off + 4 * ((long)npages + 1);
    long acc = 0;
    for (int p = 0; p < npages; p++) {
        w32(f, (unsigned)(base + acc));
        acc += blens[p];
    }
    w32(f, (unsigned)(base + acc));
    for (int p = 0; p < npages; p++) {
        fwrite(blobs[p], 1, (size_t)blens[p], f);
        free(blobs[p]);
    }
    free(blobs); free(blens);
}

static long emit_mini_bin(const MiniFont *m, const char *path)
{
    FILE *f = xfopen(path, "wb");
    w8(f, MINI_N); w8(f, 0);
    int off = 2 + 2 * MINI_N;
    for (int i = 0; i < MINI_N; i++) {
        w16(f, (unsigned)off);
        off += 5 + (size_t)((m->g[i].w + 1) / 2) * m->g[i].h;
    }
    for (int i = 0; i < MINI_N; i++) {
        const Glyph *g = &m->g[i];
        w8(f, (unsigned)g->w); w8(f, (unsigned)g->h);
        w8(f, (unsigned)(g->bx & 0xFF)); w8(f, (unsigned)(g->by & 0xFF));
        w8(f, (unsigned)g->adv_fp);
        int rb = (g->w + 1) / 2;
        for (int r = 0; r < g->h; r++)
            for (int cb = 0; cb < rb; cb++) {
                int c0 = cb * 2, c1 = c0 + 1;
                unsigned v = g->bm[r * g->w + c0];
                if (c1 < g->w)
                    v |= (unsigned)g->bm[r * g->w + c1] << 4;
                w8(f, v);
            }
    }
    long sz = ftell(f);
    fclose(f);
    return sz;
}

/* ---------------- page previews (replay like the GBA runtime) ------- */
static void mini_text(u8 *canvas, const MiniFont *mini, const char *text,
                      int x, int baseline, int dim)
{
    static const char mc[] = MINI_CHARS;
    for (const char *c = text; *c; c++) {
        const char *at = strchr(mc, *c);
        if (!at)
            continue;
        const Glyph *g = &mini->g[at - mc];
        for (int r = 0; r < g->h; r++)
            for (int cc = 0; cc < g->w; cc++) {
                int a = g->bm[r * g->w + cc];
                if (dim)
                    a = (a * 3) >> 2;
                int X = x + g->bx + cc, Y = baseline + g->by + r;
                if (a && X >= 0 && X < SCREEN_W && Y >= 0 && Y < SCREEN_H &&
                    a > canvas[Y * SCREEN_W + X])
                    canvas[Y * SCREEN_W + X] = (u8)a;
            }
        x += g->adv_fp;
    }
}

static int mini_text_width(const MiniFont *mini, const char *text)
{
    static const char mc[] = MINI_CHARS;
    int w = 0;
    for (const char *c = text; *c; c++) {
        const char *at = strchr(mc, *c);
        if (at)
            w += mini->g[at - mc].adv_fp;
    }
    return w;
}

#ifndef BB_WASM
static void save_canvas_png(const u8 *canvas, const char *path)
{
    ImgL lv = {malloc(SCREEN_W * SCREEN_H), SCREEN_W, SCREEN_H};
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
        lv.px[i] = (u8)(canvas[i] * 17);
    ImgRGB *rgb = imgrgb_new(SCREEN_W, SCREEN_H);
    ink_ramp(&lv, rgb);
    write_png_2x(path, rgb);
    free(lv.px); free(rgb->px); free(rgb);
}

static void render_preview(const Font *font, const MiniFont *mini,
                           const Paginator *pg, const TocEnt *toc, int ntoc,
                           int idx, const char *path)
{
    if (idx < 0 || idx >= pg->pages.n)
        return;
    u8 *canvas = calloc(SCREEN_W * SCREEN_H, 1);
    const PageV *page = &pg->pages.v[idx];
    for (int l = 0; l < page->lines.n; l++) {
        const Line *ln = &page->lines.v[l].line;
        int baseline = page->lines.v[l].baseline;
        PenSim sim;
        sim_init(&sim, font, 0);
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
                int gi = font_index(font, s->cp[k]);
                sim.fp += font_kern_fp(font, sim.prev, gi, st);
                int x = snap_fp(sim.fp);
                const Glyph *g = &font->g[st][gi];
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
    /* footer: book percent left, page-in-chapter right */
    char left[16], right[24];
    u32 pct = ((u32)(idx + 1) * 100) / (u32)pg->pages.n;
    if (pct > 100) pct = 100;
    snprintf(left, sizeof(left), "%u%%", pct);
    int story = 0;
    for (int t = 0; t < ntoc; t++)
        if (toc[t].page <= idx)
            story = t;
    int cstart = ntoc ? toc[story].page : 0;
    int cend = (story + 1 < ntoc) ? toc[story + 1].page : pg->pages.n;
    snprintf(right, sizeof(right), "%d / %d", idx - cstart + 1, cend - cstart);
    mini_text(canvas, mini, left, MARGIN_L, FOOTER_BASE, 1);
    mini_text(canvas, mini, right,
              SCREEN_W - MARGIN_R - mini_text_width(mini, right), FOOTER_BASE,
              1);
    save_canvas_png(canvas, path);
    free(canvas);
}

#endif /* !BB_WASM (previews) */

/* ---------------- multi-book driver ---------------- */
#include <ctype.h>
#ifndef BB_WASM
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* run a program with an explicit argv — no shell, so filenames can't inject.
 * Returns the child exit status, or -1 on spawn failure. */
static int run_argv(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);                 /* exec failed */
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        ;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* recursive rmdir without shelling out to `rm -rf` */
static void rm_rf(const char *path)
{
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        char child[2048];
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            struct stat st;
            if (!lstat(child, &st) && S_ISDIR(st.st_mode))
                rm_rf(child);
            else
                unlink(child);
        }
        closedir(d);
        rmdir(path);
    }
}
#endif

#define ROM_LIMIT (32 * 1024 * 1024)
#define COVER_BYTES (SCREEN_W * SCREEN_H * 2)
#define TITLE_MAX_PX 168            /* uses more of the spine; the % badge,
                                       when present, draws over the tail */

typedef struct {
    char epub[700];
    char dir[700];
    BookMeta meta;
    Book book;
    u8 *blob;
    size_t blob_size;
    u8 *cover;
    u8 *cover2;
    int cover2_len;
    int npages, nchapters, slots;
    TocEnt *toc;
    int ntoc;
    u8 enc_title[80];
    int enc_len;
    long cost;
    int selected, forced, excluded;
    u32 book_off, cover_off, cover2_off;   /* filled writing books.bin */
} BookRec;

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static int title_contains(const BookMeta *m, const char *needle)
{
    char low[512];
    int n = 0;
    for (int i = 0; i < m->title_len && n < 500; i++) {
        u32 c = m->title[i];
        low[n++] = (c < 128) ? (char)tolower((int)c) : '?';
    }
    low[n] = 0;
    return strstr(low, needle) != NULL;
}

/* encode a title into GID_BASE glyph bytes, truncated with … to fit */
static int encode_title(const Font *font, const BookMeta *m, u8 *out)
{
    int ell = font_index(font, 0x2026);
    int n = 0, fp = 0, prev = -1;
    int fit_n = 0;
    for (int i = 0; i < m->title_len && n < 79; i++) {
        int idx = font_index(font, m->title[i]);
        fp += font_kern_fp(font, prev, idx, 0) + font->g[0][idx].adv_fp;
        prev = idx;
        /* width if we stopped here and added an ellipsis */
        int fp_ell = fp + font_kern_fp(font, prev, ell, 0) +
                     font->g[0][ell].adv_fp;
        if (snap_fp(fp) <= TITLE_MAX_PX)
            fit_n = n + 1;
        out[n++] = (u8)(GID_BASE + idx);
        if (snap_fp(fp_ell) > TITLE_MAX_PX && i < m->title_len - 1) {
            /* truncate: keep the last prefix that fits with … */
            n = fit_n > 0 ? fit_n - 1 : 0;
            out[n++] = (u8)(GID_BASE + ell);
            return n;
        }
    }
    if (snap_fp(fp) > TITLE_MAX_PX && n > 1) {
        n--;
        out[n++] = (u8)(GID_BASE + ell);
    }
    return n;
}

/* draw a GID_BASE byte stream (regular style) onto a level canvas */
static int draw_gid_stream(u8 *canvas, const Font *font, int x, int baseline,
                           const u8 *bytes, int n)
{
    int fp = x << 4, prev = -1;
    for (int i = 0; i < n; i++) {
        int idx = bytes[i] - GID_BASE;
        fp += font_kern_fp(font, prev, idx, 0);
        int px = snap_fp(fp);
        const Glyph *g = &font->g[0][idx];
        for (int r = 0; r < g->h; r++)
            for (int c = 0; c < g->w; c++) {
                int a = g->bm[r * g->w + c];
                int X = px + g->bx + c, Y = baseline + g->by + r;
                if (a && X >= 0 && X < SCREEN_W && Y >= 0 && Y < SCREEN_H &&
                    a > canvas[Y * SCREEN_W + X])
                    canvas[Y * SCREEN_W + X] = (u8)a;
            }
        fp += g->adv_fp;
        prev = idx;
    }
    return snap_fp(fp);
}

#ifndef BB_WASM
/* RGB mock of the on-device book-spine stack (device draws the same
 * geometry with palette ramps) */
static const u8 SPINE_COLORS[4][3] = {
    {0xD4, 0xCC, 0xBB}, {0xBB, 0xB3, 0xA4}, {0xA2, 0x9B, 0x8D},
    {0x90, 0x88, 0x7C},
};

static u32 spine_hash_enc(const u8 *t, int n)
{
    u32 h = 2166136261u;
    for (int k = 0; k < n; k++) { h ^= t[k]; h *= 16777619u; }
    return h;
}

static void spine_px(ImgRGB *rgb, int x, int y, const u8 *base, int lvl)
{
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H)
        return;
    u8 *p = rgb->px + ((size_t)y * SCREEN_W + x) * 3;
    for (int c = 0; c < 3; c++)
        p[c] = (u8)(base[c] + (INK[c] - (int)base[c]) * lvl / 15);
}

static void spine_rect(ImgRGB *rgb, int x, int y, int w, int h,
                       const u8 *base, int lvl)
{
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            spine_px(rgb, x + c, y + r, base, lvl);
}

static void library_preview(const Font *font, const MiniFont *mini,
                            BookRec **shelf, int nshelf, const char *path)
{
    ImgL *zero = imgl_new(SCREEN_W, SCREEN_H, 0);
    ImgRGB *rgb = imgrgb_new(SCREEN_W, SCREEN_H);
    ink_ramp(zero, rgb);
    /* header via level canvas overlaid in ink */
    u8 *hdr = calloc(SCREEN_W * SCREEN_H, 1);
    {
        int x = MARGIN_L + 2;
        for (const char *c = "LIBRARY"; *c; c++) {
            u8 b = (u8)(GID_BASE + font_index(font, (u32)*c));
            x = draw_gid_stream(hdr, font, x, 12, &b, 1) + 1;
        }
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
            if (hdr[i])
                spine_px(rgb, i % SCREEN_W, i / SCREEN_W,
                         (const u8 *)PAPER, hdr[i]);
    }
    int rows = nshelf < 7 ? nshelf : 7;
    for (int r = 0; r < rows; r++) {
        BookRec *b = shelf[r];
        u32 h = spine_hash_enc(b->enc_title, b->enc_len) ^
                ((u32)b->npages * 2654435761u);
        int w = 196 + (int)(h % 4) * 10;
        int x = (SCREEN_W - w) / 2 + (int)((h >> 4) % 9) - 4;
        int y0 = 18 + r * 20;
        int sel = (r == 1);
        if (sel)
            x += 10;
        const u8 *col = SPINE_COLORS[h % 4];
        spine_rect(rgb, x, y0, w, 19, col, 0);
        spine_rect(rgb, x, y0 + 18, w, 1, col, 5);
        spine_rect(rgb, x, y0, 2, 19, col, 3);
        spine_rect(rgb, x + w - 2, y0, 2, 19, col, 3);
        if (sel) {
            spine_rect(rgb, x - 1, y0 - 1, w + 2, 1, col, 15);
            spine_rect(rgb, x - 1, y0 + 19, w + 2, 1, col, 15);
            spine_rect(rgb, x - 1, y0, 1, 19, col, 15);
            spine_rect(rgb, x + w, y0, 1, 19, col, 15);
        }
        u8 *lv = calloc(SCREEN_W * SCREEN_H, 1);
        draw_gid_stream(lv, font, x + 10, y0 + 5 + font->ascent,
                        b->enc_title, b->enc_len);
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%", (r * 23 + 8) % 100);
        mini_text(lv, mini, pct, x + w - 8 - mini_text_width(mini, pct),
                  y0 + 5 + font->ascent, 1);
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
            if (lv[i])
                spine_px(rgb, i % SCREEN_W, i / SCREEN_W, col, lv[i]);
        free(lv);
    }
    free(hdr);
    write_png_2x(path, rgb);
    free(zero->px); free(zero); free(rgb->px); free(rgb);
}
#endif /* !BB_WASM */

static void free_book(Book *b)
{
    for (int c = 0; c < b->v.n; c++) {
        for (int p = 0; p < b->v.v[c].paras.n; p++) {
            Para *pa = &b->v.v[c].paras.v[p];
            for (int r = 0; r < pa->runs.n; r++)
                free(pa->runs.v[r].cp);
            free(pa->runs.v);
        }
        free(b->v.v[c].paras.v);
    }
    free(b->v.v);
    memset(b, 0, sizeof(*b));
}

static void free_paginator(Paginator *pg)
{
    for (int p = 0; p < pg->pages.n; p++) {
        for (int l = 0; l < pg->pages.v[p].lines.n; l++)
            line_free(&pg->pages.v[p].lines.v[l].line);
        free(pg->pages.v[p].lines.v);
    }
    free(pg->pages.v);
    memset(pg, 0, sizeof(*pg));
}

#ifndef BB_WASM
int main(int argc, char **argv)
{
    if (argc > 1)
        snprintf(ROOT, sizeof(ROOT), "%s", argv[1]);
    int font_px = getenv("FONT_PX") ? atoi(getenv("FONT_PX")) : 14;
    double gamma = getenv("FONT_GAMMA") ? atof(getenv("FONT_GAMMA")) : 0.72;
    /* Font source: FONT_DIR/XCharter-*.otf if set, else the bundled fonts
     * in web/assets. (Apple's Charter.ttc, the author's original build
     * input, is selectable by pointing FONT_DIR at it — but it is never
     * redistributed, so the default is the freely-licensed XCharter.) */
    const char *font_dir = getenv("FONT_DIR");
    if (!font_dir)
        font_dir = "web/assets";
    {
        static char fr[800], fi[800], fb[800];
        snprintf(fr, sizeof(fr), "%s/XCharter-Roman.otf", font_dir);
        snprintf(fi, sizeof(fi), "%s/XCharter-Italic.otf", font_dir);
        snprintf(fb, sizeof(fb), "%s/XCharter-Bold.otf", font_dir);
        font_set_sources(fr, 0, fi, 0, fb, 0);
    }
    char books_dir[700];
    snprintf(books_dir, sizeof(books_dir), "%s",
             getenv("BOOKS_DIR") ? getenv("BOOKS_DIR") : "books");
    const char *force_titles = getenv("BB_FORCE");   /* keep-first substrings */
    const char *excl_titles = getenv("BB_EXCLUDE");  /* drop-these substrings */
    int preview_book = getenv("BB_PREVIEW") != NULL; /* render sample pages */

    char pbuf[1100];
    const char *subdirs[] = {"build", "build/data", "build/preview",
                             "build/books"};
    for (unsigned si = 0; si < sizeof(subdirs) / sizeof(*subdirs); si++) {
        snprintf(pbuf, sizeof(pbuf), "%s/%s", ROOT, subdirs[si]);
        mkdir(pbuf, 0755);
    }

    /* ---- scan + extract epubs ---- */
    static char names[80][512];
    int nnames = 0;
    DIR *dd = opendir(books_dir);
    if (!dd) { fprintf(stderr, "no dir %s\n", books_dir); return 1; }
    struct dirent *de;
    while ((de = readdir(dd)) && nnames < 80) {
        size_t l = strlen(de->d_name);
        if (l > 5 && !strcmp(de->d_name + l - 5, ".epub"))
            snprintf(names[nnames++], 512, "%s", de->d_name);
    }
    closedir(dd);
    qsort(names, (size_t)nnames, 512, cmp_name);
    printf("found %d epubs in %s\n", nnames, books_dir);

    static BookRec recs[80];
    int nrecs = 0;
    for (int i = 0; i < nnames; i++) {
        BookRec *r = &recs[nrecs];
        memset(r, 0, sizeof(*r));
        snprintf(r->epub, sizeof(r->epub), "%s/%s", books_dir, names[i]);
        snprintf(r->dir, sizeof(r->dir), "%s/build/books/b_%02d", ROOT, i);
        /* extract if the marker doesn't match */
        char marker[800], have[600] = "";
        snprintf(marker, sizeof(marker), "%s/.src", r->dir);
        FILE *mf = fopen(marker, "rb");
        if (mf) {
            fgets(have, sizeof(have), mf);
            fclose(mf);
        }
        if (strcmp(have, names[i])) {
            rm_rf(r->dir);
            mkdir(r->dir, 0755);
            char *argv[] = {(char *)"unzip", (char *)"-o", (char *)"-qq",
                            r->epub, (char *)"-d", r->dir, NULL};
            if (run_argv(argv) != 0) {
                fprintf(stderr, "  skip (unzip failed): %s\n", names[i]);
                continue;
            }
            mf = fopen(marker, "wb");
            if (mf) { fputs(names[i], mf); fclose(mf); }
        }
        if (!epub_parse(r->dir, &r->book, &r->meta)) {
            fprintf(stderr, "  skip (unparseable): %s\n", names[i]);
            continue;
        }
        long raw = 0;               /* rough text volume for sanity check */
        for (int c = 0; c < r->book.v.n; c++)
            for (int p = 0; p < r->book.v.v[c].paras.n; p++)
                for (int rr = 0; rr < r->book.v.v[c].paras.v[p].runs.n; rr++)
                    raw += r->book.v.v[c].paras.v[p].runs.v[rr].len;
        if (raw < 4000) {           /* defective epub: near-empty conversion */
            fprintf(stderr, "  skip (defective, %ld chars): %s\n", raw,
                    names[i]);
            free_book(&r->book);
            continue;
        }
        if (r->meta.title_len == 0) {   /* fallback: filename stem */
            for (const char *c = names[i];
                 *c && *c != '.' && r->meta.title_len < 60; c++) {
                if (!strncmp(c, " -- ", 4))
                    break;
                r->meta.title[r->meta.title_len++] =
                    (*c == '_') ? ' ' : (u32)(u8)*c;
            }
        }
        count_book_chars(&r->book);
        nrecs++;
    }

    build_codepage();
    printf("charset: %d glyphs\n", ncharset);
    hyphen_load(rootpath("tools/data/hyph_en_US.dic"));

    static Font font;
    font_build(&font, font_px, gamma, charset, ncharset);
    printf("Charter %dpx: line_h=%d ascent=%d kern %d/%d\n", font_px,
           font.line_height, font.ascent, font.kern_pairs[0],
           font.kern_pairs[1]);
    MiniFont mini;
    mini_build(&mini);

    /* ---- per-book: layout, emit blob, cover, previews ---- */
    for (int i = 0; i < nrecs; i++) {
        BookRec *r = &recs[i];
        static Paginator pg;
        memset(&pg, 0, sizeof(pg));
        r->toc = calloc(600, sizeof(TocEnt));
        paginate(&font, &r->book, &pg, r->toc, &r->ntoc);
        if (r->ntoc == 0) {         /* implicit single chapter */
            r->toc[0].page = 0;
            r->toc[0].title_len =
                r->meta.title_len < 127 ? r->meta.title_len : 127;
            memcpy(r->toc[0].title, r->meta.title,
                   (size_t)r->toc[0].title_len * 4);
            r->ntoc = 1;
        }
        r->npages = pg.pages.n;
        r->nchapters = r->book.v.n;
        r->slots = pg.slots;

        FILE *ms = open_memstream((char **)&r->blob, &r->blob_size);
        emit_book_stream(&font, &pg, r->toc, r->ntoc, ms);
        fclose(ms);

        char cover_path[2100];
        if (epub_cover_path(r->dir, cover_path, sizeof(cover_path))) {
            int cw0, ch0;
            u8 *crgb = load_cover_rgb_pub(cover_path, &cw0, &ch0);
            if (crgb) {
                r->cover = cover_screen_from_rgb(crgb, cw0, ch0);
                r->cover2 = cover2_encode(crgb, cw0, ch0, &r->cover2_len);
                free(crgb);
            }
        }

        r->enc_len = encode_title(&font, &r->meta, r->enc_title);
        /* BB_FORCE: comma-separated title substrings to keep first when the
         * shelf overflows 32MB (case-insensitive). Empty = pure smallest-
         * first packing. */
        r->forced = 0;
        if (force_titles)
            for (const char *t = force_titles; *t; ) {
                char needle[128];
                int nl = 0;
                while (*t && *t != ',' && nl < 127)
                    needle[nl++] = (char)tolower((unsigned char)*t++);
                needle[nl] = 0;
                while (*t == ',') t++;
                if (nl && title_contains(&r->meta, needle))
                    r->forced = 1;
            }
        /* BB_EXCLUDE: comma-separated substrings to drop outright (wins over
         * BB_FORCE). For pruning textbooks or corrupt-titled scans. */
        r->excluded = 0;
        if (excl_titles)
            for (const char *t = excl_titles; *t; ) {
                char needle[128];
                int nl = 0;
                while (*t && *t != ',' && nl < 127)
                    needle[nl++] = (char)tolower((unsigned char)*t++);
                needle[nl] = 0;
                while (*t == ',') t++;
                if (nl && title_contains(&r->meta, needle))
                    r->excluded = 1;
            }
        if (r->excluded)
            r->forced = 0;
        r->cost = (long)((r->blob_size + 3) & ~3ul) +
                  (r->cover ? COVER_BYTES : 0) + r->cover2_len + 96;

        if (preview_book) {         /* BB_PREVIEW: render sample pages */
            for (int k = 0; k < 2; k++) {
                char pp[1100];
                snprintf(pp, sizeof(pp), "%s/build/preview/bk%02d_p%04d.png",
                         ROOT, i, r->npages / 3 + k);
                render_preview(&font, &mini, &pg, r->toc, r->ntoc,
                               r->npages / 3 + k, pp);
            }
        }
        printf("  [%2d] %-46.46s %5d pg  %7zu B  %s\n", i,
               ({ static char t[200]; int n = 0;
                  for (int k = 0; k < r->meta.title_len && n < 180; k++)
                      n += utf8_encode(r->meta.title[k], t + n);
                  t[n] = 0; t; }),
               r->npages, r->blob_size, r->cover ? "cover" : "-");
        free_paginator(&pg);
        free_book(&r->book);
    }

    /* ---- emit font/mini/title, then pack ---- */
    long fs = emit_font_bin(&font, rootpath("build/data/font.bin"));
    long msz = emit_mini_bin(&mini, rootpath("build/data/mini.bin"));
    /* art.bin: static sprites for the library scene */
    {
        static const char *art_files[ART_COUNT] = {
            "book.png", "hands.png", "grab_book1.png", "grab_book2.png",
            "grab_book3.png", "grab_book4.png", "show_hand.png",
            "show_hand.png", "speak.png"};
        u8 *sp[ART_COUNT];
        int sl[ART_COUNT];
        for (int i = 0; i < ART_COUNT; i++) {
            char ap[1100];
            snprintf(ap, sizeof(ap), "%s/assets/art/%s", ROOT, art_files[i]);
            int w0, h0;
            u8 *rgba = load_png_rgba(ap, &w0, &h0);
            if (!rgba) {
                fprintf(stderr, "missing art: %s\n", ap);
                exit(1);
            }
            /* full-canvas frames stay canvas-placed; the held-book hands
             * are standalone sprites (ART_SHOWR gets mirrored here) */
            if (i == ART_SHOWR) {
                u8 *mir = malloc((size_t)w0 * h0 * 4);
                for (int y = 0; y < h0; y++)
                    for (int x = 0; x < w0; x++)
                        memcpy(mir + ((size_t)y * w0 + x) * 4,
                               rgba + ((size_t)y * w0 + (w0 - 1 - x)) * 4, 4);
                free(rgba);
                rgba = mir;
            }
            sp[i] = sprite_encode(rgba, w0, h0, &sl[i]);
            free(rgba);
        }
        FILE *af = xfopen(rootpath("build/data/art.bin"), "wb");
        fwrite("BART", 1, 4, af);
        w32(af, ART_COUNT);
        long aoff = 8 + 8 * ART_COUNT;
        for (int i = 0; i < ART_COUNT; i++) {
            w32(af, (unsigned)aoff);
            w32(af, (unsigned)sl[i]);
            aoff += (sl[i] + 3) & ~3;
        }
        for (int i = 0; i < ART_COUNT; i++) {
            fwrite(sp[i], 1, (size_t)sl[i], af);
            for (int p = sl[i]; p & 3; p++)
                fputc(0, af);
            free(sp[i]);
        }
        fclose(af);
    }

    char png[1000], t1[1100], t2[1100];
    snprintf(png, sizeof(png), "%s/assets/art/book_boy_advance.png", ROOT);
    snprintf(t1, sizeof(t1), "%s/build/data/title.bin", ROOT);
    snprintf(t2, sizeof(t2), "%s/build/preview/title.png", ROOT);
    build_title_bin(png, t1, t2);
    FILE *tf = fopen(t1, "rb");
    fseek(tf, 0, SEEK_END);
    long ts = ftell(tf);
    fclose(tf);

    long budget = ROM_LIMIT - fs - msz - ts - 300 * 1024;
    long used = 8;
    int nsel = 0;
    for (int i = 0; i < nrecs; i++)     /* forced books first */
        if (recs[i].forced && used + recs[i].cost <= budget) {
            recs[i].selected = 1;
            used += recs[i].cost;
            nsel++;
        }
    for (;;) {                          /* then smallest-first */
        int best = -1;
        for (int i = 0; i < nrecs; i++)
            if (!recs[i].selected && !recs[i].excluded &&
                (best < 0 || recs[i].cost < recs[best].cost))
                best = i;
        if (best < 0 || used + recs[best].cost > budget)
            break;
        recs[best].selected = 1;
        used += recs[best].cost;
        nsel++;
    }

    /* shelf order: alphabetical by title */
    static BookRec *shelf[80];
    int nshelf = 0;
    for (int i = 0; i < nrecs; i++)
        if (recs[i].selected)
            shelf[nshelf++] = &recs[i];
    for (int i = 0; i < nshelf; i++)
        for (int j = i + 1; j < nshelf; j++) {
            u32 a[512], b[512];
            int an = shelf[i]->meta.title_len, bn = shelf[j]->meta.title_len;
            for (int k = 0; k < an; k++)
                a[k] = shelf[i]->meta.title[k] |
                       (shelf[i]->meta.title[k] < 128 ? 0x20 : 0);
            for (int k = 0; k < bn; k++)
                b[k] = shelf[j]->meta.title[k] |
                       (shelf[j]->meta.title[k] < 128 ? 0x20 : 0);
            int cmp = 0;
            for (int k = 0; k < an && k < bn && !cmp; k++)
                cmp = (int)a[k] - (int)b[k];
            if (!cmp)
                cmp = an - bn;
            if (cmp > 0) {
                BookRec *t = shelf[i];
                shelf[i] = shelf[j];
                shelf[j] = t;
            }
        }

    /* ---- books.bin + library.bin ---- */
    FILE *bf = xfopen(rootpath("build/data/books.bin"), "wb");
    long pos = 0;
    for (int i = 0; i < nshelf; i++) {
        BookRec *r = shelf[i];
        r->book_off = (u32)pos;
        fwrite(r->blob, 1, r->blob_size, bf);
        pos += (long)r->blob_size;
        while (pos & 3) { fputc(0, bf); pos++; }
        if (r->cover) {
            r->cover_off = (u32)pos;
            fwrite(r->cover, 1, COVER_BYTES, bf);
            pos += COVER_BYTES;
        }
        if (r->cover2) {
            r->cover2_off = (u32)pos;
            fwrite(r->cover2, 1, (size_t)r->cover2_len, bf);
            pos += r->cover2_len;
            while (pos & 3) { fputc(0, bf); pos++; }
        }
    }
    fclose(bf);

    FILE *lf = xfopen(rootpath("build/data/library.bin"), "wb");
    fwrite("BLB2", 1, 4, lf);
    w32(lf, (unsigned)nshelf);
    for (int i = 0; i < nshelf; i++) {
        BookRec *r = shelf[i];
        w32(lf, r->book_off);
        w32(lf, r->cover ? r->cover_off : 0);
        w32(lf, r->cover2 ? r->cover2_off : 0);
        w16(lf, (unsigned)r->npages);
        w8(lf, (unsigned)r->enc_len);
        w8(lf, 0);
        u8 tb[80] = {0};
        memcpy(tb, r->enc_title, (size_t)r->enc_len);
        fwrite(tb, 1, 80, lf);
    }
    long ls = ftell(lf);
    fclose(lf);

    library_preview(&font, &mini, shelf, nshelf,
                    rootpath("build/preview/library.png"));

    /* ---- toc.txt ---- */
    FILE *tt = xfopen(rootpath("build/data/toc.txt"), "wb");
    for (int i = 0; i < nshelf; i++) {
        BookRec *r = shelf[i];
        char line[700];
        int n = 0;
        for (int k = 0; k < r->meta.title_len && n < 500; k++)
            n += utf8_encode(r->meta.title[k], line + n);
        n += snprintf(line + n, sizeof(line) - (size_t)n,
                      "  (%d pages, %d chapters)\n", r->npages, r->ntoc);
        fwrite(line, 1, (size_t)n, tt);
        for (int t = 0; t < r->ntoc; t++) {
            n = snprintf(line, sizeof(line), "%7d  ", r->toc[t].page + 1);
            for (int k = 0; k < r->toc[t].title_len && n < 600; k++)
                n += utf8_encode(r->toc[t].title[k], line + n);
            line[n++] = '\n';
            fwrite(line, 1, (size_t)n, tt);
        }
    }
    fclose(tt);

    printf("selected %d/%d books, books.bin %ld B, library.bin %ld B\n",
           nsel, nrecs, pos, ls);
    printf("dropped:");
    for (int i = 0; i < nrecs; i++)
        if (!recs[i].selected) {
            char t[120];
            int n = 0;
            for (int k = 0; k < recs[i].meta.title_len && n < 100; k++)
                n += utf8_encode(recs[i].meta.title[k], t + n);
            t[n] = 0;
            printf(" [%s]", t);
        }
    printf("\nest ROM: %ld B (limit %d)\n",
           fs + msz + ts + pos + ls + 60 * 1024, ROM_LIMIT);
    return 0;
}
#endif /* !BB_WASM (native driver) */
