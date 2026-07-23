/* Image ops + title/cover screen builders + PNG previews.
 *
 * The two-pass Lanczos resample (PRECISION_BITS fixed point, clip8), the
 * alpha compositing, the RGB->L coefficients, and the div-255 mask blend
 * are adapted from Pillow's libImaging (HPND license) — proven integer
 * implementations of the standard algorithms.
 */
#include "bb.h"
#include "bb_art.h"

#ifndef BB_WASM
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "vendor/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "vendor/stb_image_write.h"

#include <jpeglib.h>
#endif

const u8 PAPER[3] = {0xF2, 0xEA, 0xD7};
const u8 INK[3] = {0x2B, 0x26, 0x20};

/* ---------------- basic images ---------------- */
ImgL *imgl_new(int w, int h, int fill)
{
    ImgL *im = malloc(sizeof(ImgL));
    im->w = w; im->h = h;
    im->px = malloc((size_t)w * h);
    memset(im->px, fill, (size_t)w * h);
    return im;
}

ImgRGB *imgrgb_new(int w, int h)
{
    ImgRGB *im = malloc(sizeof(ImgRGB));
    im->w = w; im->h = h;
    im->px = calloc((size_t)w * h, 3);
    return im;
}

#ifndef BB_WASM
u8 *load_png_rgba(const char *path, int *w, int *h)
{
    int n;
    return stbi_load(path, w, h, &n, 4);
}
#endif

#ifndef BB_WASM
#include <setjmp.h>

struct jerr_jmp {
    struct jpeg_error_mgr mgr;
    jmp_buf env;
};

static void jerr_exit(j_common_ptr cinfo)
{
    longjmp(((struct jerr_jmp *)cinfo->err)->env, 1);
}

/* returns NULL on any decode error (bad file, CMYK, truncation, ...) */
u8 *load_jpeg_rgb(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    struct jpeg_decompress_struct cinfo;
    struct jerr_jmp jerr;
    cinfo.err = jpeg_std_error(&jerr.mgr);
    jerr.mgr.error_exit = jerr_exit;
    u8 *out = NULL;
    if (setjmp(jerr.env)) {
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        free(out);
        return NULL;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;    /* decode straight to RGB */
    jpeg_start_decompress(&cinfo);
    *w = (int)cinfo.output_width;
    *h = (int)cinfo.output_height;
    out = malloc((size_t)*w * *h * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        u8 *row = out + (size_t)cinfo.output_scanline * *w * 3;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);
    return out;
}
#endif /* !BB_WASM */

/* ------- Lanczos resample, 8bpc (adapted from Pillow libImaging) ------- */
#define PRECISION_BITS (32 - 8 - 2)

static double sinc_filter(double x)
{
    if (x == 0.0) return 1.0;
    x = x * M_PI;
    return sin(x) / x;
}

static double lanczos_filter(double x)
{
    if (-3.0 <= x && x < 3.0)
        return sinc_filter(x) * sinc_filter(x / 3);
    return 0.0;
}

static u8 clip8(int in)
{
    int v = in >> PRECISION_BITS;
    return (u8)(v <= 0 ? 0 : v < 256 ? v : 255);
}

static int precompute_coeffs(int inSize, double in0, double in1, int outSize,
                             int **boundsp, double **kkp)
{
    double support, scale, filterscale, center, ww, ss;
    int xx, x, ksize, xmin, xmax;
    filterscale = scale = (in1 - in0) / outSize;
    if (filterscale < 1.0)
        filterscale = 1.0;
    support = 3.0 * filterscale;        /* LANCZOS support */
    ksize = (int)ceil(support) * 2 + 1;
    double *kk = malloc((size_t)outSize * ksize * sizeof(double));
    int *bounds = malloc((size_t)outSize * 2 * sizeof(int));
    for (xx = 0; xx < outSize; xx++) {
        center = in0 + (xx + 0.5) * scale;
        ww = 0.0;
        ss = 1.0 / filterscale;
        xmin = (int)(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        xmax = (int)(center + support + 0.5);
        if (xmax > inSize) xmax = inSize;
        xmax -= xmin;
        double *k = &kk[xx * ksize];
        for (x = 0; x < xmax; x++) {
            double w = lanczos_filter((x + xmin - center + 0.5) * ss);
            k[x] = w;
            ww += w;
        }
        for (x = 0; x < xmax; x++)
            if (ww != 0.0)
                k[x] /= ww;
        for (; x < ksize; x++)
            k[x] = 0;
        bounds[xx * 2 + 0] = xmin;
        bounds[xx * 2 + 1] = xmax;
    }
    *boundsp = bounds;
    *kkp = kk;
    return ksize;
}

static void normalize_coeffs_8bpc(int outSize, int ksize, double *prekk)
{
    s32 *kk = (s32 *)prekk;
    for (int x = 0; x < outSize * ksize; x++) {
        if (prekk[x] < 0)
            kk[x] = (int)(-0.5 + prekk[x] * (1 << PRECISION_BITS));
        else
            kk[x] = (int)(0.5 + prekk[x] * (1 << PRECISION_BITS));
    }
}

/* channels-interleaved resample; ch = 1 or 3 */
u8 *resize_lanczos(const u8 *src, int sw, int sh, int ch, int dw, int dh)
{
    int *bounds_h, *bounds_v;
    double *kk_h, *kk_v;
    int ksize_h = precompute_coeffs(sw, 0, sw, dw, &bounds_h, &kk_h);
    int ksize_v = precompute_coeffs(sh, 0, sh, dh, &bounds_v, &kk_v);

    int ybox_first = bounds_v[0];
    int ybox_last = bounds_v[dh * 2 - 2] + bounds_v[dh * 2 - 1];
    int need_h = dw != sw, need_v = dh != sh;

    const u8 *cur = src;
    int cur_w = sw, cur_h = sh;
    u8 *tmp = NULL;

    if (need_h) {
        for (int i = 0; i < dh; i++)
            bounds_v[i * 2] -= ybox_first;
        int th = ybox_last - ybox_first;
        tmp = malloc((size_t)dw * th * ch);
        normalize_coeffs_8bpc(dw, ksize_h, kk_h);
        s32 *kk = (s32 *)kk_h;
        for (int yy = 0; yy < th; yy++) {
            for (int xx = 0; xx < dw; xx++) {
                int xmin = bounds_h[xx * 2], xmax = bounds_h[xx * 2 + 1];
                s32 *k = &kk[xx * ksize_h];
                for (int b = 0; b < ch; b++) {
                    int ss0 = 1 << (PRECISION_BITS - 1);
                    for (int x = 0; x < xmax; x++)
                        ss0 += src[((size_t)(yy + ybox_first) * sw +
                                    (x + xmin)) * ch + b] * k[x];
                    tmp[((size_t)yy * dw + xx) * ch + b] = clip8(ss0);
                }
            }
        }
        cur = tmp;
        cur_w = dw;
        cur_h = th;
    }
    free(bounds_h);
    free(kk_h);

    u8 *out;
    if (need_v) {
        out = malloc((size_t)dw * dh * ch);
        normalize_coeffs_8bpc(dh, ksize_v, kk_v);
        s32 *kk = (s32 *)kk_v;
        for (int yy = 0; yy < dh; yy++) {
            s32 *k = &kk[yy * ksize_v];
            int ymin = bounds_v[yy * 2], ymax = bounds_v[yy * 2 + 1];
            for (int xx = 0; xx < cur_w; xx++) {
                for (int b = 0; b < ch; b++) {
                    int ss0 = 1 << (PRECISION_BITS - 1);
                    for (int y = 0; y < ymax; y++)
                        ss0 += cur[((size_t)(y + ymin) * cur_w + xx) * ch + b]
                               * k[y];
                    out[((size_t)yy * dw + xx) * ch + b] = clip8(ss0);
                }
            }
        }
    } else {
        out = malloc((size_t)cur_w * cur_h * ch);
        memcpy(out, cur, (size_t)cur_w * cur_h * ch);
    }
    free(bounds_v);
    free(kk_v);
    free(tmp);
    (void)cur_h;
    return out;
}

/* ---------------- composite + convert ---------------- */
#define SHIFTFORDIV255(a) ((((a) >> 8) + (a)) >> 8)
#define ACPRECISION 7

/* alpha_composite(white, src) then convert('L'); in place per pixel */
void rgba_over_white_to_L(u8 *rgba, int n, u8 *out_l)
{
    for (int i = 0; i < n; i++) {
        const u8 *s = rgba + i * 4;
        u32 r, g, b;
        if (s[3] == 0) {
            r = g = b = 255;            /* dst copied (white) */
        } else {
            u32 blend = 255 * (255 - s[3]);     /* dst->a = 255 */
            u32 outa255 = (u32)s[3] * 255 + blend;
            u32 coef1 = (u32)s[3] * 255 * 255 * (1 << ACPRECISION) / outa255;
            u32 coef2 = 255 * (1 << ACPRECISION) - coef1;
            u32 tr = s[0] * coef1 + 255 * coef2;
            u32 tg = s[1] * coef1 + 255 * coef2;
            u32 tb = s[2] * coef1 + 255 * coef2;
            r = SHIFTFORDIV255(tr + (0x80 << ACPRECISION)) >> ACPRECISION;
            g = SHIFTFORDIV255(tg + (0x80 << ACPRECISION)) >> ACPRECISION;
            b = SHIFTFORDIV255(tb + (0x80 << ACPRECISION)) >> ACPRECISION;
        }
        out_l[i] = (u8)((r * 19595 + g * 38470 + b * 7471 + 0x8000) >> 16);
    }
}

/* ---------------- duotone + RGB555 ---------------- */
void ink_ramp(const ImgL *gray, ImgRGB *rgb)
{
    u8 lut[256][3];
    for (int a = 0; a < 256; a++) {
        double t = a / 255.0;
        for (int c = 0; c < 3; c++)
            lut[a][c] = (u8)round_half_even(PAPER[c] + (INK[c] - PAPER[c]) * t);
    }
    int n = gray->w * gray->h;
    for (int i = 0; i < n; i++) {
        const u8 *l = lut[gray->px[i]];
        rgb->px[i * 3] = l[0];
        rgb->px[i * 3 + 1] = l[1];
        rgb->px[i * 3 + 2] = l[2];
    }
}

static const int BAYER4[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

void to_rgb555(const ImgRGB *img, u8 *out)
{
    int i = 0;
    for (int y = 0; y < img->h; y++)
        for (int x = 0; x < img->w; x++) {
            double d = (BAYER4[y & 3][x & 3] - 7.5) / 2.0;
            int rgb[3];
            for (int c = 0; c < 3; c++) {
                long v = round_half_even(img->px[(y * img->w + x) * 3 + c] + d);
                rgb[c] = v < 0 ? 0 : v > 255 ? 255 : (int)v;
            }
            unsigned v = (unsigned)((rgb[0] >> 3) | ((rgb[1] >> 3) << 5) |
                                    ((rgb[2] >> 3) << 10));
            out[i++] = (u8)(v & 0xFF);
            out[i++] = (u8)(v >> 8);
        }
}

#ifndef BB_WASM
void write_png_2x(const char *path, const ImgRGB *img)
{
    int W = img->w * 2, H = img->h * 2;
    u8 *big = malloc((size_t)W * H * 3);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            memcpy(big + ((size_t)y * W + x) * 3,
                   img->px + ((size_t)(y / 2) * img->w + x / 2) * 3, 3);
    stbi_write_png(path, W, H, 3, big, W * 3);
    free(big);
}
#endif

/* ---------------- title screen ---------------- */
#ifndef BB_WASM
void build_title_bin(const char *png_path, const char *out_path,
                     const char *preview_path)
{
    const int W = SCREEN_W, H = SCREEN_H;
    int aw, ah;
    u8 *rgba = load_png_rgba(png_path, &aw, &ah);
    if (!rgba || aw != W || ah != H) {
        fprintf(stderr, "title art must be 240x160: %s\n", png_path);
        exit(1);
    }
    /* composite over paper, duotone via luminance */
    ImgL *canvas = imgl_new(W, H, 0);
    for (int i = 0; i < W * H; i++) {
        const u8 *p = rgba + i * 4;
        u32 r, g, b;
        u32 blend = 255 * (255 - p[3]);
        u32 outa = (u32)p[3] * 255 + blend;
        u32 c1 = outa ? (u32)p[3] * 255 * 255 * 128 / outa : 0;
        u32 c2 = 255 * 128 - c1;
        r = SHIFTFORDIV255(p[0] * c1 + 255 * c2 + (0x80 << 7)) >> 7;
        g = SHIFTFORDIV255(p[1] * c1 + 255 * c2 + (0x80 << 7)) >> 7;
        b = SHIFTFORDIV255(p[2] * c1 + 255 * c2 + (0x80 << 7)) >> 7;
        u32 lum = (r * 19595 + g * 38470 + b * 7471 + 0x8000) >> 16;
        canvas->px[i] = (u8)(255 - lum);
    }
    free(rgba);

    /* keep a text-free copy so the blink-off strip restores the book art */
    ImgL *plain = imgl_new(W, H, 0);
    memcpy(plain->px, canvas->px, (size_t)W * H);

    /* PRESS START centered on the blank shelf book */
    const char *ps = "PRESS START";
    /* largest size+tracking that fits the blank book */
    int fsz = 10, sp = 2, pw = 0;
    for (;;) {
        pw = 0;
        for (const char *c = ps; *c; c++)
            pw += (int)round_half_even(title_char_adv66((u32)*c, fsz) / 64.0)
                  + sp;
        pw -= sp;
        if (pw <= AR_PSTART_W - 4 || (fsz == 8 && sp == 1))
            break;
        if (sp > 1) sp--;
        else { fsz--; sp = 2; }
    }
    int px0 = AR_PSTART_X + (AR_PSTART_W - pw) / 2;
    int py0 = AR_PSTART_Y + (AR_PSTART_H - fsz - 2) / 2;
    {
        int x = px0;
        for (const char *c = ps; *c; c++) {
            title_draw_char(canvas, x, py0, (u32)*c, fsz, 255);
            x += (int)round_half_even(title_char_adv66((u32)*c, fsz) / 64.0)
                 + sp;
        }
    }
    int sx = px0 - 2, sy = py0 - 1, sx1 = px0 + pw + 2, sy1 = py0 + 14;
    int sw = sx1 - sx, sh = sy1 - sy;

    ImgRGB *rgb = imgrgb_new(W, H);
    ink_ramp(canvas, rgb);
    u8 *raw = malloc((size_t)W * H * 2);
    to_rgb555(rgb, raw);
    ImgRGB *rgb_off = imgrgb_new(W, H);
    ink_ramp(plain, rgb_off);
    u8 *raw_off = malloc((size_t)W * H * 2);
    to_rgb555(rgb_off, raw_off);

    /* v2 title: header (magic 0xFFFF marks two-strip format), image with
     * text, strip_on rows, strip_off rows */
    FILE *f = fopen(out_path, "wb");
    w16(f, 0xFFFF); w16(f, (unsigned)sx); w16(f, (unsigned)sy);
    w16(f, (unsigned)sw); w16(f, (unsigned)sh); w16(f, 0);
    fwrite(raw, 1, (size_t)W * H * 2, f);
    for (int y = sy; y < sy1; y++)
        fwrite(raw + ((size_t)y * W + sx) * 2, 1, (size_t)sw * 2, f);
    for (int y = sy; y < sy1; y++)
        fwrite(raw_off + ((size_t)y * W + sx) * 2, 1, (size_t)sw * 2, f);
    fclose(f);

    write_png_2x(preview_path, rgb);
    free(canvas->px); free(canvas); free(plain->px); free(plain);
    free(rgb->px); free(rgb); free(rgb_off->px); free(rgb_off);
    free(raw); free(raw_off);
}
#endif

/* ---------------- cover screens ---------------- */
/* shared core: RGB pixels -> 240x160 RGB555 cover screen */
u8 *cover_screen_from_rgb(const u8 *cov, int cw0, int ch0)
{
    const int W = SCREEN_W, H = SCREEN_H;
    if (!cov || cw0 < 8 || ch0 < 8)
        return NULL;
    int ch = H - 10;
    int cw = (int)round_half_even((double)cw0 * ch / ch0);
    if (cw > W - 10) {
        cw = W - 10;
        ch = (int)round_half_even((double)ch0 * cw / cw0);
    }
    u8 *scaled = resize_lanczos(cov, cw0, ch0, 3, cw, ch);

    ImgL *zero = imgl_new(W, H, 0);
    ImgRGB *canvas = imgrgb_new(W, H);
    ink_ramp(zero, canvas);
    int x0 = (W - cw) / 2, y0 = (H - ch) / 2;
    int rx0 = x0 - 2, ry0 = y0 - 2, rx1 = x0 + cw + 1, ry1 = y0 + ch + 1;
    if (rx0 >= 0 && ry0 >= 0 && rx1 < W && ry1 < H) {
        for (int x = rx0; x <= rx1; x++) {
            memcpy(canvas->px + ((size_t)ry0 * W + x) * 3, INK, 3);
            memcpy(canvas->px + ((size_t)ry1 * W + x) * 3, INK, 3);
        }
        for (int y = ry0; y <= ry1; y++) {
            memcpy(canvas->px + ((size_t)y * W + rx0) * 3, INK, 3);
            memcpy(canvas->px + ((size_t)y * W + rx1) * 3, INK, 3);
        }
    }
    for (int y = 0; y < ch; y++)
        memcpy(canvas->px + ((size_t)(y0 + y) * W + x0) * 3,
               scaled + (size_t)y * cw * 3, (size_t)cw * 3);

    u8 *raw = malloc((size_t)W * H * 2);
    to_rgb555(canvas, raw);
    free(scaled); free(zero->px); free(zero);
    free(canvas->px); free(canvas);
    return raw;
}

/* held-book pose cover: fit into the show_hand pocket area, 128-color quantized */
u8 *cover2_encode(const u8 *rgb, int w, int h, int *out_len)
{
    if (!rgb || w < 8 || h < 8) { *out_len = 0; return NULL; }
    int dw = AR_COVER_W, dh = AR_COVER_H;
    /* fit inside, preserve aspect */
    if (w * dh > h * dw)
        dh = (int)round_half_even((double)h * dw / w);
    else
        dw = (int)round_half_even((double)w * dh / h);
    if (dw < 8) dw = 8;
    if (dh < 8) dh = 8;
    u8 *scaled = resize_lanczos(rgb, w, h, 3, dw, dh);
    int len = 4 + 256 + dw * dh;
    u8 *out = malloc((size_t)len);
    out[0] = (u8)dw; out[1] = (u8)dh; out[2] = 0; out[3] = 0;
    u16 pal[128];
    quantize_median_cut(scaled, dw * dh, 128, pal, out + 4 + 256);
    for (int i = 0; i < 128; i++) {
        out[4 + i * 2] = (u8)(pal[i] & 0xFF);
        out[4 + i * 2 + 1] = (u8)(pal[i] >> 8);
    }
    free(scaled);
    *out_len = len;
    return out;
}

#ifndef BB_WASM
u8 *load_cover_rgb_pub(const char *path, int *w, int *h);
static u8 *load_cover_rgb(const char *path, int *w, int *h)
{
    size_t n = strlen(path);
    if (n > 4 && (!strcmp(path + n - 4, ".png") ||
                  !strcmp(path + n - 4, ".PNG"))) {
        u8 *rgba = load_png_rgba(path, w, h);
        if (!rgba)
            return NULL;
        u8 *rgb = malloc((size_t)*w * *h * 3);
        for (int i = 0; i < *w * *h; i++) {
            const u8 *s = rgba + i * 4;      /* composite over white */
            for (int c = 0; c < 3; c++)
                rgb[i * 3 + c] =
                    (u8)((s[c] * s[3] + 255 * (255 - s[3]) + 127) / 255);
        }
        free(rgba);
        return rgb;
    }
    return load_jpeg_rgb(path, w, h);
}

u8 *load_cover_rgb_pub(const char *path, int *w, int *h)
{
    return load_cover_rgb(path, w, h);
}

/* native wrapper: decode an image file, then the shared RGB core */
u8 *cover_screen_raw(const char *img_path)
{
    int cw0, ch0;
    u8 *cov = load_cover_rgb(img_path, &cw0, &ch0);
    if (!cov)
        return NULL;
    u8 *raw = cover_screen_from_rgb(cov, cw0, ch0);
    free(cov);
    return raw;
}
#endif /* !BB_WASM */

/* ---------------- art sprites (level + alpha) ---------------- */
#include "bb_art.h"

/* RGBA canvas art -> trimmed sprite: 4bpp ink levels + 1bpp alpha.
 * White (lum 255) = level 0 so palette ramps tint the fill. */
u8 *sprite_encode(const u8 *rgba, int W, int H, int *out_len)
{
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (rgba[(y * W + x) * 4 + 3] > 128) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    if (x1 < 0) { *out_len = 0; return NULL; }
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    int lvl_rb = (w + 1) / 2, al_rb = (w + 7) / 8;
    int len = 8 + (lvl_rb + al_rb) * h;
    u8 *out = calloc(1, (size_t)len);
    out[0] = (u8)x0; out[1] = (u8)y0;   /* canvas placement of trim box */
    out[2] = (u8)w; out[3] = (u8)h;
    out[4] = (u8)(w >> 8); out[5] = (u8)(h >> 8);
    u8 *lvl = out + 8, *al = out + 8 + lvl_rb * h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const u8 *p = rgba + ((size_t)(y + y0) * W + (x + x0)) * 4;
            if (p[3] <= 128)
                continue;
            u32 lum = (p[0] * 19595 + p[1] * 38470 + p[2] * 7471 + 0x8000) >> 16;
            u32 v = ((255 - lum) * 15 + 127) / 255;
            lvl[y * lvl_rb + (x >> 1)] |= (u8)(v << ((x & 1) * 4));
            al[y * al_rb + (x >> 3)] |= (u8)(1 << (x & 7));
        }
    *out_len = len;
    return out;
}

/* ---------------- median-cut quantizer (cover2) ---------------- */
typedef struct { u8 r, g, b; u32 n; } QCol;

static int qc_cmp_r(const void *a, const void *b)
{ return ((const QCol *)a)->r - ((const QCol *)b)->r; }
static int qc_cmp_g(const void *a, const void *b)
{ return ((const QCol *)a)->g - ((const QCol *)b)->g; }
static int qc_cmp_b(const void *a, const void *b)
{ return ((const QCol *)a)->b - ((const QCol *)b)->b; }

/* rgb pixels -> ncolors palette (RGB555) + indexed pixels */
void quantize_median_cut(const u8 *rgb, int npx, int ncolors,
                         u16 *pal_out, u8 *idx_out)
{
    /* histogram in 15-bit space */
    static u32 hist[32768];
    memset(hist, 0, sizeof(hist));
    for (int i = 0; i < npx; i++) {
        int k = ((rgb[i * 3] >> 3) << 10) | ((rgb[i * 3 + 1] >> 3) << 5) |
                (rgb[i * 3 + 2] >> 3);
        hist[k]++;
    }
    QCol *cols = malloc(32768 * sizeof(QCol));
    int nc = 0;
    for (int k = 0; k < 32768; k++)
        if (hist[k]) {
            cols[nc].r = (u8)((k >> 10) & 31);
            cols[nc].g = (u8)((k >> 5) & 31);
            cols[nc].b = (u8)(k & 31);
            cols[nc].n = hist[k];
            nc++;
        }
    typedef struct { int start, len; } Box;
    Box boxes[256];
    int nboxes = 1;
    boxes[0] = (Box){0, nc};
    while (nboxes < ncolors) {
        /* split the box with the largest channel range */
        int bi = -1, bch = 0, bspread = 0;
        for (int b = 0; b < nboxes; b++) {
            if (boxes[b].len < 2)
                continue;
            int mn[3] = {31, 31, 31}, mx[3] = {0, 0, 0};
            for (int i = 0; i < boxes[b].len; i++) {
                QCol *c = &cols[boxes[b].start + i];
                int v[3] = {c->r, c->g, c->b};
                for (int k = 0; k < 3; k++) {
                    if (v[k] < mn[k]) mn[k] = v[k];
                    if (v[k] > mx[k]) mx[k] = v[k];
                }
            }
            for (int k = 0; k < 3; k++)
                if (mx[k] - mn[k] > bspread) {
                    bspread = mx[k] - mn[k];
                    bi = b;
                    bch = k;
                }
        }
        if (bi < 0)
            break;
        Box *bx = &boxes[bi];
        qsort(cols + bx->start, (size_t)bx->len, sizeof(QCol),
              bch == 0 ? qc_cmp_r : bch == 1 ? qc_cmp_g : qc_cmp_b);
        /* median by population */
        u32 total = 0, acc = 0;
        for (int i = 0; i < bx->len; i++)
            total += cols[bx->start + i].n;
        int cut = 1;
        for (int i = 0; i < bx->len - 1; i++) {
            acc += cols[bx->start + i].n;
            if (acc * 2 >= total) { cut = i + 1; break; }
        }
        boxes[nboxes] = (Box){bx->start + cut, bx->len - cut};
        bx->len = cut;
        nboxes++;
    }
    /* box averages -> palette */
    for (int b = 0; b < nboxes; b++) {
        u64 sr = 0, sg = 0, sb = 0, sn = 0;
        for (int i = 0; i < boxes[b].len; i++) {
            QCol *c = &cols[boxes[b].start + i];
            sr += (u64)c->r * c->n; sg += (u64)c->g * c->n;
            sb += (u64)c->b * c->n; sn += c->n;
        }
        if (!sn) sn = 1;
        pal_out[b] = (u16)((sr / sn) | ((sg / sn) << 5) | ((sb / sn) << 10));
    }
    for (int b = nboxes; b < ncolors; b++)
        pal_out[b] = 0;
    /* nearest-palette indexing */
    for (int i = 0; i < npx; i++) {
        int r = rgb[i * 3] >> 3, g = rgb[i * 3 + 1] >> 3, b2 = rgb[i * 3 + 2] >> 3;
        int best = 0, bd = 1 << 30;
        for (int p = 0; p < nboxes; p++) {
            int dr = r - (pal_out[p] & 31), dg = g - ((pal_out[p] >> 5) & 31),
                db = b2 - ((pal_out[p] >> 10) & 31);
            int d = dr * dr * 3 + dg * dg * 6 + db * db;
            if (d < bd) { bd = d; best = p; }
        }
        idx_out[i] = (u8)best;
    }
    free(cols);
}
