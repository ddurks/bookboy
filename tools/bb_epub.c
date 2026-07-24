/* Generic EPUB spine + chapter HTML parsing.
 *
 *  - META-INF/container.xml locates the OPF; hrefs resolve against the OPF's
 *    directory (with %xx decoding)
 *  - spine order, skipping linear="no" items and non-XHTML media types
 *  - <p> is a body paragraph; <h1>/<h2> open chapters (K_TITLE), <h3> is
 *    K_SUBTITLE, <h4>-<h6> K_DISPLAY; the Wells class names (fmh/cst/ctag/
 *    pt/fmsh) are honored when present
 *  - paragraph indent is automatic (indent unless the previous paragraph
 *    was a heading), overridable by *tx / *tx1 class hints
 *  - <em>/<i>/<cite> nest; <br> flushes and reopens the paragraph
 *  - runs are cleaned per-run: exotic spaces -> space, BOM/SHY dropped,
 *    ligature codepoints expanded, whitespace runs collapsed
 */
#include "bb.h"

/* ---------------- unicode helpers ---------------- */
/* Unicode whitespace (incl. NEL and the general-punctuation spaces) */
static int is_uspace(u32 c)
{
    switch (c) {
    case ' ': case '\t': case '\n': case '\r': case '\f': case '\v':
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x85: case 0xA0:
    case 0x1680: case 0x2028: case 0x2029: case 0x202F: case 0x205F:
    case 0x3000:
        return 1;
    default:
        return (c >= 0x2000 && c <= 0x200A);
    }
}

static int utf8_decode(const u8 *s, int len, int *i, u32 *out)
{
    if (*i >= len) return 0;
    u8 c = s[*i];
    if (c < 0x80) { *out = c; (*i)++; return 1; }
    int n = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
    if (*i + n > len) {             /* truncated multibyte at buffer end */
        *out = '?';
        *i = len;
        return 1;
    }
    u32 v = c & (0x7F >> n);
    for (int k = 1; k < n; k++)
        v = (v << 6) | (s[*i + k] & 0x3F);
    *out = v;
    *i += n;
    return 1;
}

int utf8_encode(u32 cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static char *slurp(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    fread(buf, 1, (size_t)n, f);
    buf[n] = 0;
    fclose(f);
    if (out_len) *out_len = n;
    return buf;
}

/* ---------------- entities ---------------- */
static const struct { const char *name; u32 cp; } ENTITIES[] = {
    {"lt", '<'}, {"gt", '>'}, {"amp", '&'}, {"quot", '"'}, {"apos", '\''},
    {"nbsp", 0xA0}, {"shy", 0xAD}, {"mdash", 0x2014}, {"ndash", 0x2013},
    {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C},
    {"rdquo", 0x201D}, {"hellip", 0x2026}, {"eacute", 0xE9},
    {"egrave", 0xE8}, {"ecirc", 0xEA}, {"euml", 0xEB}, {"agrave", 0xE0},
    {"acirc", 0xE2}, {"aacute", 0xE1}, {"auml", 0xE4}, {"ccedil", 0xE7},
    {"ocirc", 0xF4}, {"oacute", 0xF3}, {"ouml", 0xF6}, {"ucirc", 0xFB},
    {"uuml", 0xFC}, {"uacute", 0xFA}, {"iuml", 0xEF}, {"iacute", 0xED},
    {"icirc", 0xEE}, {"ntilde", 0xF1}, {"aelig", 0xE6}, {"oelig", 0x153},
    {"szlig", 0xDF}, {"deg", 0xB0}, {"pound", 0xA3}, {"copy", 0xA9},
    {"sect", 0xA7}, {"middot", 0xB7}, {"laquo", 0xAB}, {"raquo", 0xBB},
    {"frac12", 0xBD}, {"frac14", 0xBC}, {"emsp", 0x2003}, {"ensp", 0x2002},
    {"thinsp", 0x2009}, {"trade", 0x2122}, {"dagger", 0x2020},
};

static u32 decode_entity(const char *s, int n)
{
    if (n >= 2 && s[0] == '#') {
        long v;
        if (s[1] == 'x' || s[1] == 'X')
            v = strtol(s + 2, NULL, 16);
        else
            v = strtol(s + 1, NULL, 10);
        return (u32)v;
    }
    for (unsigned i = 0; i < sizeof(ENTITIES) / sizeof(*ENTITIES); i++)
        if ((int)strlen(ENTITIES[i].name) == n &&
            !strncmp(s, ENTITIES[i].name, (size_t)n))
            return ENTITIES[i].cp;
    return '?';
}

/* decode HTML text to codepoints; writes at most outcap, returns count.
 * Output can only be shorter than the byte input, so callers passing a
 * buffer sized to `len` may pass outcap = len (or larger). */
static int decode_data_cap(const char *s, int len, u32 *out, int outcap)
{
    int n = 0, i = 0;
    while (i < len && n < outcap) {
        if (s[i] == '&') {
            int j = i + 1;
            while (j < len && j < i + 12 && s[j] != ';' && s[j] != '&' &&
                   s[j] != '<')
                j++;
            if (j < len && s[j] == ';') {
                out[n++] = decode_entity(s + i + 1, j - i - 1);
                i = j + 1;
                continue;
            }
        }
        u32 cp;
        int ii = i;
        utf8_decode((const u8 *)s, len, &ii, &cp);
        out[n++] = cp;
        i = ii;
    }
    return n;
}

/* ---------------- paragraph assembly ---------------- */
int para_textlen(const Para *p)
{
    int n = 0;
    for (int i = 0; i < p->runs.n; i++)
        n += p->runs.v[i].len;
    return n;
}

void para_text(const Para *p, u32 *out)
{
    int n = 0;
    for (int i = 0; i < p->runs.n; i++) {
        memcpy(out + n, p->runs.v[i].cp, (size_t)p->runs.v[i].len * 4);
        n += p->runs.v[i].len;
    }
}

/* like para_text but never writes more than cap codepoints */
void para_text_cap(const Para *p, u32 *out, int cap)
{
    int n = 0;
    for (int i = 0; i < p->runs.n && n < cap; i++) {
        int len = p->runs.v[i].len;
        if (len > cap - n)
            len = cap - n;
        memcpy(out + n, p->runs.v[i].cp, (size_t)len * 4);
        n += len;
    }
}

static int para_blank(const Para *p)
{
    for (int i = 0; i < p->runs.n; i++)
        for (int k = 0; k < p->runs.v[i].len; k++)
            if (!is_uspace(p->runs.v[i].cp[k]))
                return 0;
    return 1;
}

typedef struct {
    Para cur;
    int cur_hint;                   /* indent hint: -1 auto, 0 no, 1 yes */
    int have_cur;
    int italic;
    VEC(Para) out;
    VEC(int) hints;                 /* parallel to out */
} PState;

static void para_clear(Para *p)
{
    for (int i = 0; i < p->runs.n; i++)
        free(p->runs.v[i].cp);
    free(p->runs.v);
    memset(&p->runs, 0, sizeof(p->runs));
}

static void ps_flush(PState *ps, int keep)
{
    if (!ps->have_cur)
        return;
    Kind kind = ps->cur.kind;
    if (!para_blank(&ps->cur)) {
        VPUSH(ps->out, ps->cur);
        VPUSH(ps->hints, ps->cur_hint);
    } else {
        para_clear(&ps->cur);
    }
    if (keep) {
        memset(&ps->cur, 0, sizeof(Para));
        ps->cur.kind = kind;
        ps->cur.indent = 0;
        ps->have_cur = 1;
    } else {
        ps->have_cur = 0;
    }
}

static void ps_data(PState *ps, const u32 *cp, int len)
{
    if (!ps->have_cur || !len)
        return;
    Run r;
    r.italic = ps->italic > 0;
    r.cp = malloc((size_t)len * 4);
    memcpy(r.cp, cp, (size_t)len * 4);
    r.len = len;
    VPUSH(ps->cur.runs, r);
}

/* ---------------- tag handling ---------------- */
static int tag_attr(const char *tag, int taglen, const char *name, char *buf,
                    int bufsz)
{
    int nl = (int)strlen(name);
    for (int i = 0; i + nl + 2 < taglen; i++) {
        if (!strncmp(tag + i, name, (size_t)nl) && tag[i + nl] == '=' &&
            (i == 0 || tag[i - 1] == ' ' || tag[i - 1] == '\t' ||
             tag[i - 1] == '\n')) {
            char q = tag[i + nl + 1];
            if (q != '"' && q != '\'')
                return 0;
            int j = i + nl + 2, k = 0;
            while (j < taglen && tag[j] != q && k < bufsz - 1)
                buf[k++] = tag[j++];
            buf[k] = 0;
            return 1;
        }
    }
    return 0;
}

/* Wells (Random House) + common oeb class names; generic books fall
 * through */
static Kind class_kind(const char *cls, Kind dflt)
{
    if (!strcmp(cls, "fmh")) return K_TITLE;
    if (!strcmp(cls, "cst")) return K_SUBTITLE;
    if (!strcmp(cls, "ctag")) return K_BYLINE;
    if (!strcmp(cls, "pt")) return K_PART;
    if (!strcmp(cls, "fmsh")) return K_PARTSUB;
    if (!strcmp(cls, "center")) return K_DISPLAY;
    if (!strcmp(cls, "ct")) return K_TITLE;     /* oeb chapter title */
    return dflt;
}

static void open_para(PState *ps, Kind kind, const char *tag, int taglen)
{
    char cls[64] = "";
    tag_attr(tag, taglen, "class", cls, sizeof(cls));
    char *c = cls;
    while (*c == ' ') c++;
    char *e = c + strlen(c);
    while (e > c && e[-1] == ' ') *--e = 0;
    if (kind == K_BODY)
        kind = class_kind(c, K_BODY);
    memset(&ps->cur, 0, sizeof(Para));
    ps->cur.kind = kind;
    ps->cur.indent = 0;
    ps->cur_hint = -1;
    if (kind == K_BODY) {           /* *tx = force no indent, *tx1 = force */
        size_t cl = strlen(c);
        if (cl >= 3 && !strcmp(c + cl - 3, "tx1"))
            ps->cur_hint = 1;
        else if (cl >= 2 && !strcmp(c + cl - 2, "tx"))
            ps->cur_hint = 0;
    }
    ps->have_cur = 1;
    ps->italic = 0;
}

static void handle_starttag(PState *ps, const char *name, const char *tag,
                            int taglen)
{
    if (!strcmp(name, "p") || !strcmp(name, "div")) {
        /* divs open a paragraph too: oeb-era books put body text straight
         * in <div class="tx">; container divs are harmless because any
         * inner <p>/<h*> simply replaces the still-empty paragraph */
        open_para(ps, K_BODY, tag, taglen);
    } else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' &&
               !name[2]) {
        Kind k = (name[1] <= '2') ? K_TITLE
                 : (name[1] == '3') ? K_SUBTITLE : K_DISPLAY;
        open_para(ps, k, tag, taglen);
    } else if ((!strcmp(name, "em") || !strcmp(name, "i") ||
                !strcmp(name, "cite")) && ps->have_cur) {
        ps->italic++;
    } else if (!strcmp(name, "br") && ps->have_cur) {
        ps_flush(ps, 1);
    }
}

static void handle_endtag(PState *ps, const char *name)
{
    if (!strcmp(name, "p") || !strcmp(name, "div") ||
        (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]))
        ps_flush(ps, 0);
    else if ((!strcmp(name, "em") || !strcmp(name, "i") ||
              !strcmp(name, "cite")) && ps->italic)
        ps->italic--;
}

static void parse_html(const char *html, long len, PState *ps)
{
    long i = 0;
    u32 *dbuf = malloc((size_t)len * 4 + 4);
    while (i < len) {
        const char *lt = memchr(html + i, '<', (size_t)(len - i));
        long tag_at = lt ? lt - html : len;
        if (tag_at > i) {
            int seg = (int)(tag_at - i);
            int n = decode_data_cap(html + i, seg, dbuf, seg + 1);
            ps_data(ps, dbuf, n);
        }
        if (!lt)
            break;
        i = tag_at;
        if (!strncmp(html + i, "<!--", 4)) {
            const char *end = strstr(html + i, "-->");
            i = end ? (end - html) + 3 : len;
            continue;
        }
        if (html[i + 1] == '?' || html[i + 1] == '!') {
            const char *gt = memchr(html + i, '>', (size_t)(len - i));
            i = gt ? (gt - html) + 1 : len;
            continue;
        }
        const char *gt = memchr(html + i, '>', (size_t)(len - i));
        if (!gt)
            break;
        int taglen = (int)(gt - (html + i)) - 1;
        const char *t = html + i + 1;
        int closing = (t[0] == '/');
        if (closing) { t++; taglen--; }
        int selfclose = (taglen > 0 && t[taglen - 1] == '/');
        if (selfclose) taglen--;
        char name[16];
        int k = 0;
        while (k < taglen && k < 15 && t[k] != ' ' && t[k] != '\t' &&
               t[k] != '\n' && t[k] != '\r')
            { name[k] = (char)(t[k] | 0x20); k++; }
        name[k] = 0;
        if (closing) {
            handle_endtag(ps, name);
        } else if (!strcmp(name, "style") || !strcmp(name, "script")) {
            /* skip raw text content (matches the JS DOMParser, which never
             * surfaces <style>/<script> text) */
            char close[10];
            snprintf(close, sizeof(close), "</%s", name);
            const char *end = NULL;
            for (const char *p = gt + 1; p <= html + len - (long)strlen(close);
                 p++)
                if ((p[0] | 0) == '<' &&
                    !strncmp(p + 1, close + 1, strlen(close) - 1)) {
                    end = p;
                    break;
                }
            if (!end) break;
            const char *egt = memchr(end, '>', (size_t)(html + len - end));
            i = egt ? (egt - html) + 1 : len;
            continue;
        } else {
            handle_starttag(ps, name, t, taglen);
            if (selfclose)
                handle_endtag(ps, name);
        }
        i = (gt - html) + 1;
    }
    free(dbuf);
}

/* ---------------- per-run cleaning ---------------- */
static Run clean_run(const Run *r)
{
    Run out;
    out.italic = r->italic;
    /* ligature expansion can grow the text */
    u32 *tmp = malloc(((size_t)r->len * 3 + 4) * 4);
    int tn = 0;
    for (int i = 0; i < r->len; i++) {
        u32 c = r->cp[i];
        if (c == 0xA0 || c == 0x2009 || c == 0x2002 || c == 0x2003)
            c = ' ';
        if (c == 0xFEFF || c == 0xAD)
            continue;
        switch (c) {                /* ligature codepoints -> letters */
        case 0xFB00: tmp[tn++] = 'f'; tmp[tn++] = 'f'; continue;
        case 0xFB01: tmp[tn++] = 'f'; tmp[tn++] = 'i'; continue;
        case 0xFB02: tmp[tn++] = 'f'; tmp[tn++] = 'l'; continue;
        case 0xFB03: tmp[tn++] = 'f'; tmp[tn++] = 'f'; tmp[tn++] = 'i'; continue;
        case 0xFB04: tmp[tn++] = 'f'; tmp[tn++] = 'f'; tmp[tn++] = 'l'; continue;
        }
        tmp[tn++] = c;
    }
    out.cp = malloc((size_t)tn * 4 + 4);
    int n = 0;
    for (int i = 0; i < tn;) {
        if (is_uspace(tmp[i])) {
            out.cp[n++] = ' ';
            while (i < tn && is_uspace(tmp[i]))
                i++;
        } else {
            out.cp[n++] = tmp[i++];
        }
    }
    free(tmp);
    out.len = n;
    return out;
}

/* case-insensitive substring (strcasestr isn't in Emscripten's libc) */
static const char *ci_strstr(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl)
        return hay;
    for (; *hay; hay++) {
        size_t k = 0;
        while (k < nl && hay[k] &&
               (hay[k] | 0x20) == (needle[k] | 0x20))
            k++;
        if (k == nl)
            return hay;
    }
    return NULL;
}

/* ---------------- OPF / container ---------------- */
static void pct_decode(char *s)
{
    char *w = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            int hi = p[1] <= '9' ? p[1] - '0' : (p[1] | 0x20) - 'a' + 10;
            int lo = p[2] <= '9' ? p[2] - '0' : (p[2] | 0x20) - 'a' + 10;
            *w++ = (char)((hi << 4) | lo);
            p += 2;
        } else {
            *w++ = *p;
        }
    }
    *w = 0;
}

static int find_opf(const char *dir, char *out, int outsz)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/META-INF/container.xml", dir);
    long len;
    char *xml = slurp(path, &len);
    if (xml) {
        char *fp = strstr(xml, "full-path=\"");
        if (fp) {
            fp += 11;
            char *q = strchr(fp, '"');
            if (q && q - fp < outsz - 1) {
                snprintf(out, (size_t)outsz, "%.*s", (int)(q - fp), fp);
                free(xml);
                return 1;
            }
        }
        free(xml);
    }
    snprintf(out, (size_t)outsz, "content.opf");
    snprintf(path, sizeof(path), "%s/content.opf", dir);
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* dir-part of a path (empty string if none) */
static void path_dir(const char *path, char *out, int outsz)
{
    const char *slash = strrchr(path, '/');
    if (!slash) { out[0] = 0; return; }
    snprintf(out, (size_t)outsz, "%.*s/", (int)(slash - path), path);
}

typedef struct { char id[64]; char href[512]; int is_xhtml; } Item;
typedef struct { Item *v; int n, cap; } ItemVec;

static int opf_items(const char *opf, ItemVec *items)
{
    for (const char *p = opf; (p = strstr(p, "<item ")); p++) {
        const char *gt = strchr(p, '>');
        if (!gt) break;
        int tl = (int)(gt - p);
        Item it;
        char mt[128] = "";
        if (!tag_attr(p, tl, "id", it.id, sizeof(it.id)) ||
            !tag_attr(p, tl, "href", it.href, sizeof(it.href)))
            continue;
        tag_attr(p, tl, "media-type", mt, sizeof(mt));
        pct_decode(it.href);
        it.is_xhtml = strstr(mt, "xhtml") != NULL || strstr(mt, "html") != NULL
                      || strstr(mt, "oeb1-document") != NULL;
        if (!it.is_xhtml) {         /* fall back to the file extension */
            size_t hl = strlen(it.href);
            it.is_xhtml =
                (hl > 4 && (!strcmp(it.href + hl - 4, ".htm") ||
                            !strcmp(it.href + hl - 4, ".xml"))) ||
                (hl > 5 && (!strcmp(it.href + hl - 5, ".html") ||
                            !strcmp(it.href + hl - 5, ".xhtm"))) ||
                (hl > 6 && !strcmp(it.href + hl - 6, ".xhtml"));
        }
        VPUSH(*items, it);
    }
    return items->n;
}

static void meta_from_opf(const char *opf, const char *tagname, BookMeta *meta)
{
    meta->title_len = 0;
    char open[32];
    snprintf(open, sizeof(open), "<%s", tagname);
    const char *t = strstr(opf, open);
    if (!t)
        return;
    t = strchr(t, '>');
    if (!t)
        return;
    t++;
    const char *end = strchr(t, '<');
    if (!end)
        return;
    u32 buf[512];
    int n = decode_data_cap(t, (int)(end - t), buf, 512);
    /* titles can be double-encoded (&amp;amp;) — decode once more */
    for (int pass = 0; pass < 1; pass++) {
        /* collapse whitespace + strip */
        int m = 0;
        u32 out[512];
        for (int i = 0; i < n;) {
            if (is_uspace(buf[i])) {
                if (m > 0)
                    out[m++] = ' ';
                while (i < n && is_uspace(buf[i]))
                    i++;
            } else {
                out[m++] = buf[i++];
            }
        }
        while (m > 0 && out[m - 1] == ' ')
            m--;
        if (m > 127)
            m = 127;
        memcpy(meta->title, out, (size_t)m * 4);
        meta->title_len = m;
    }
    /* second-pass entity decode for &amp;amp; style double encoding */
    if (meta->title_len) {
        char enc[1024];
        int en = 0;
        for (int i = 0; i < meta->title_len; i++)
            en += utf8_encode(meta->title[i], enc + en);
        u32 dec[512];
        int dn = decode_data_cap(enc, en, dec, 512);
        if (dn > 127)
            dn = 127;
        memcpy(meta->title, dec, (size_t)dn * 4);
        meta->title_len = dn;
    }
}

/* ---------------- public API ---------------- */
int epub_parse(const char *epub_dir, Book *book, BookMeta *meta)
{
    int dbg = getenv("BB_DEBUG") != NULL;
    char opf_rel[512], opf_path[1200], base[600];
    if (!find_opf(epub_dir, opf_rel, sizeof(opf_rel))) {
        if (dbg) fprintf(stderr, "DBG %s: no opf\n", epub_dir);
        return 0;
    }
    snprintf(opf_path, sizeof(opf_path), "%s/%s", epub_dir, opf_rel);
    long olen;
    char *opf = slurp(opf_path, &olen);
    if (!opf) {
        if (dbg) fprintf(stderr, "DBG %s: opf unreadable: %s\n", epub_dir,
                         opf_rel);
        return 0;
    }
    if (dbg) fprintf(stderr, "DBG %s: opf=%s\n", epub_dir, opf_rel);
    path_dir(opf_rel, base, sizeof(base));
    meta_from_opf(opf, "dc:title", meta);

    ItemVec items = {0};
    opf_items(opf, &items);

    int nch = 0;
    for (const char *p = opf; (p = strstr(p, "<itemref ")); p++) {
        const char *gt = strchr(p, '>');
        if (!gt) break;
        int tl = (int)(gt - p);
        char id[64], linear[16] = "";
        if (!tag_attr(p, tl, "idref", id, sizeof(id)))
            continue;
        tag_attr(p, tl, "linear", linear, sizeof(linear));
        if (!strcmp(linear, "no"))
            continue;
        const Item *item = NULL;
        for (int i = 0; i < items.n; i++)
            if (!strcmp(items.v[i].id, id)) { item = &items.v[i]; break; }
        if (!item || !item->is_xhtml) {
            if (dbg) fprintf(stderr, "DBG   idref %s: %s\n", id,
                             item ? "not xhtml" : "no item");
            continue;
        }

        char hpath[2048];
        snprintf(hpath, sizeof(hpath), "%s/%s%s", epub_dir, base, item->href);
        long hlen;
        char *html = slurp(hpath, &hlen);
        if (!html) {
            if (dbg) fprintf(stderr, "DBG   unreadable: %s\n", hpath);
            continue;
        }
        PState ps = {0};
        parse_html(html, hlen, &ps);
        free(html);

        Chapter ch;
        memset(&ch, 0, sizeof(ch));
        snprintf(ch.idref, sizeof(ch.idref), "%.7s", id);
        Kind prev_kind = (Kind)-1;
        for (int i = 0; i < ps.out.n; i++) {
            Para *p0 = &ps.out.v[i];
            Para q;
            memset(&q, 0, sizeof(q));
            q.kind = p0->kind;
            int joined_nonblank = 0;
            for (int r = 0; r < p0->runs.n; r++) {
                Run cr = clean_run(&p0->runs.v[r]);
                if (cr.len == 0) { free(cr.cp); continue; }
                for (int k = 0; k < cr.len; k++)
                    if (!is_uspace(cr.cp[k]))
                        joined_nonblank = 1;
                VPUSH(q.runs, cr);
            }
            if (q.runs.n && joined_nonblank) {
                int hint = ps.hints.v[i];
                if (q.kind == K_BODY) {
                    if (hint >= 0)
                        q.indent = hint;
                    else            /* auto: indent unless after a heading */
                        q.indent = (prev_kind == K_BODY);
                }
                prev_kind = q.kind;
                VPUSH(ch.paras, q);
            } else {
                para_clear(&q);
            }
            para_clear(p0);
        }
        free(ps.out.v);
        free(ps.hints.v);
        if (ch.paras.n) {
            VPUSH(book->v, ch);
            nch++;
        }
    }
    free(items.v);
    free(opf);
    return nch;
}

/* locate a cover image (jpeg/png) inside the epub; returns 1 + path */
int epub_cover_path(const char *epub_dir, char *out, int outsz)
{
    char opf_rel[512], opf_path[1200], base[600];
    if (!find_opf(epub_dir, opf_rel, sizeof(opf_rel)))
        return 0;
    snprintf(opf_path, sizeof(opf_path), "%s/%s", epub_dir, opf_rel);
    char *opf = slurp(opf_path, NULL);
    if (!opf)
        return 0;
    path_dir(opf_rel, base, sizeof(base));

    ItemVec items = {0};
    opf_items(opf, &items);

    char cover_id[64] = "";
    const char *m = strstr(opf, "name=\"cover\"");
    if (m) {
        /* <meta name="cover" content="id"/> — search the whole tag */
        const char *tag = m;
        while (tag > opf && *tag != '<') tag--;
        const char *gt = strchr(m, '>');
        if (gt)
            tag_attr(tag, (int)(gt - tag), "content", cover_id,
                     sizeof(cover_id));
    }
    const Item *pick = NULL;
    for (int i = 0; i < items.n && !pick; i++)
        if (cover_id[0] && !strcmp(items.v[i].id, cover_id))
            pick = &items.v[i];
    for (int i = 0; i < items.n && !pick; i++) {
        const Item *it = &items.v[i];
        const char *h = it->href;
        int img = ci_strstr(h, ".jpg") || ci_strstr(h, ".jpeg") ||
                  ci_strstr(h, ".png");
        if (img && (ci_strstr(h, "cover") || ci_strstr(it->id, "cover")))
            pick = it;
    }
    int ok = 0;
    if (pick) {
        snprintf(out, (size_t)outsz, "%s/%s%s", epub_dir, base, pick->href);
        FILE *f = fopen(out, "rb");
        if (f) { fclose(f); ok = 1; }
    }
    free(items.v);
    free(opf);
    return ok;
}
