/* Liang hyphenation over hunspell hyph_*.dic patterns.
 *
 * Nonstandard "/x=y,i,c" alternative patterns carry replacement metadata
 * (for spelling changes at the break, e.g. Hungarian) that we don't use —
 * the integer break positions are unchanged — so patterns are truncated at
 * '/'. Returned positions respect left/right minimums of 2.
 */
#include "bb.h"

#define TBL_SIZE 32768              /* power of two, ~11k patterns */
#define MAX_PAT 24

typedef struct {
    char key[MAX_PAT];
    u8 klen;
    u8 start;
    u8 nvals;
    u8 vals[MAX_PAT + 1];
} Pat;

static Pat *tbl;
static int maxlen;

static u32 hash_bytes(const char *s, int n)
{
    u32 h = 2166136261u;
    for (int i = 0; i < n; i++) {
        h ^= (u8)s[i];
        h *= 16777619u;
    }
    return h;
}

static Pat *lookup(const char *key, int n, int insert)
{
    u32 i = hash_bytes(key, n) & (TBL_SIZE - 1);
    for (;;) {
        Pat *p = &tbl[i];
        if (p->klen == 0) {
            if (!insert)
                return NULL;
            memcpy(p->key, key, (size_t)n);
            p->klen = (u8)n;
            return p;
        }
        if (p->klen == n && !memcmp(p->key, key, (size_t)n))
            return p;
        i = (i + 1) & (TBL_SIZE - 1);
    }
}

void hyphen_load(const char *dic_path)
{
    tbl = calloc(TBL_SIZE, sizeof(Pat));
    FILE *f = fopen(dic_path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", dic_path); exit(1); }
    char line[256];
    int first = 1;
    while (fgets(line, sizeof(line), f)) {
        if (first) { first = 0; continue; }         /* encoding line */
        int n = (int)strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                     line[n - 1] == ' ' || line[n - 1] == '\t'))
            line[--n] = 0;
        if (!n || line[0] == '%' || line[0] == '#')
            continue;
        if (!strncmp(line, "LEFTHYPHENMIN", 13) ||
            !strncmp(line, "RIGHTHYPHENMIN", 14) ||
            !strncmp(line, "COMPOUNDLEFTHYPHENMIN", 21) ||
            !strncmp(line, "COMPOUNDRIGHTHYPHENMIN", 22))
            continue;
        /* ^^hh hex escapes */
        char buf[256];
        int bn = 0;
        for (int i = 0; i < n;) {
            if (i + 3 < n && line[i] == '^' && line[i + 1] == '^') {
                int hi = line[i + 2], lo = line[i + 3];
                int hv = (hi <= '9' ? hi - '0' : hi - 'a' + 10);
                int lv = (lo <= '9' ? lo - '0' : lo - 'a' + 10);
                buf[bn++] = (char)((hv << 4) | lv);
                i += 4;
            } else {
                buf[bn++] = line[i++];
            }
        }
        buf[bn] = 0;
        /* nonstandard alternative: keep left of '/' */
        char *slash = strchr(buf, '/');
        if (slash && strchr(buf, '='))
            bn = (int)(slash - buf);
        /* parse (\d?)(\D?) pairs */
        char tags[MAX_PAT];
        u8 vals[MAX_PAT + 1];
        int nt = 0, nv = 0, maxv = 0;
        for (int i = 0; i <= bn;) {
            int v = 0;
            if (i < bn && buf[i] >= '0' && buf[i] <= '9')
                v = buf[i++] - '0';
            vals[nv++] = (u8)v;
            if (v > maxv) maxv = v;
            if (i < bn)
                tags[nt++] = buf[i++];
            else
                break;
        }
        if (maxv == 0)
            continue;
        int start = 0, end = nv;
        while (!vals[start]) start++;
        while (!vals[end - 1]) end--;
        Pat *p = lookup(tags, nt, 1);
        p->start = (u8)start;
        p->nvals = (u8)(end - start);
        memcpy(p->vals, vals + start, (size_t)(end - start));
        if (nt > maxlen)
            maxlen = nt;
    }
    fclose(f);
}

/* word: lowercase-able ascii codepoints; returns break positions
 * (already filtered to 2 <= p <= len-2) */
int hyphen_positions(const u32 *word, int len, int *out, int maxout)
{
    if (len + 2 >= 200)
        return 0;
    char pointed[204];
    pointed[0] = '.';
    for (int i = 0; i < len; i++) {
        u32 c = word[i];
        if (c > 127)
            return 0;               /* non-ascii: no patterns apply */
        if (c >= 'A' && c <= 'Z')
            c += 32;
        pointed[i + 1] = (char)c;
    }
    int plen = len + 2;
    pointed[plen - 1] = '.';
    u8 refs[204] = {0};
    for (int i = 0; i < plen - 1; i++) {
        int stop = i + maxlen;
        if (stop > plen) stop = plen;
        for (int j = i + 1; j <= stop; j++) {
            Pat *p = lookup(pointed + i, j - i, 0);
            if (!p)
                continue;
            for (int k = 0; k < p->nvals; k++) {
                int idx = i + p->start + k;
                if (refs[idx] < p->vals[k])
                    refs[idx] = p->vals[k];
            }
        }
    }
    int n = 0;
    for (int i = 0; i <= plen && n < maxout; i++)
        if (refs[i] & 1) {
            int pos = i - 1;
            if (pos >= 2 && pos <= len - 2)
                out[n++] = pos;
        }
    return n;
}
