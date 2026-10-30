/* imgprep.c — High-quality print image preprocessing (libjpeg-turbo + 3FR)
 *
 * HQ pipeline (default, two-stage scaling, memory-efficient; never expand a 100MP original
 * to full size):
 *   1) Before jpeg_read_header, save APP1/Exif markers (Orientation + ColorSpace), APP2/ICC,
 *      and APP14/Adobe.
 *   2) Choose DCT scale: the largest scale where both rotated dimensions are ≥2× target
 *      (first of two scaling stages; second uses Lanczos3 for exact target size + anti-alias
 *      prefiltering). Note: §18 cropping does not change this rule (scale based on full
 *      image). Choosing scale based on crop window could force a narrow crop to den=1
 *      full-resolution decode (100MP≈300MB RGB), which is unsafe; Lanczos upsampling handles
 *      resolution loss from heavy crops.
 *   3) Decode to RGB → rotate (EXIF auto or explicit 0/90/180/270 override, §18) → apply
 *      user crop window (normalized, rotated coordinate system) → center-crop within window
 *      to target aspect ratio.
 *   4) Lanczos3 (a=3) resampling: widen kernel according to scale factor (equivalent to
 *      prefiltering, reduces downsampling aliasing/moiré), clamp edges, two-stage float
 *      precision (see resize_lanczos3).
 *   5) Color: pass through as sRGB for EXIF ColorSpace=1; for =2/untagged with AdobeRGB
 *      indicators (ICC "Adobe RGB" or APP14 Adobe marker), convert AdobeRGB→sRGB using a
 *      matrix (formula source in adobergb_to_srgb).
 *   6) Apply unsharp mask (σ=1.0px@300dpi, amount 0.35, threshold 0).
 *   7) Encode JPEG: default q95, 4:4:4 (no chroma subsampling), 300dpi metadata.
 *
 * FAST pipeline (IMGPREP_F_FAST, legacy behavior): stop at the first DCT scale ≥ target +
 *   Catmull-Rom bicubic, default q92, 4:2:2, no sharpening or color conversion.
 *
 * Target dimensions are always parameterized: caller passes media physical size × printer
 * resolution; 0,0 uses the 1200×1800 family as fallback only (general-purpose printer
 * program; no hard-coded printer attributes).
 */
#include "imgprep.h"
#include "3fr.h"

#include <stdio.h> /* jpeglib.h requires FILE to be declared first */
#include <jpeglib.h>
#include <jerror.h>
#include <math.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp (extension dispatch in imgprep_probe) */

/* Implementation details. */
#define IMGPREP_IF_FROM3FR 8u

/* Implementation details. */

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
} jerr_t;

static void jerr_exit(j_common_ptr c)
{
    jerr_t *e = (jerr_t *)c->err;
    longjmp(e->jb, 1);
}

static void jerr_noop(j_common_ptr c) { (void)c; }

const char *imgprep_strerror(int err)
{
    switch (err) {
    case IMGPREP_OK:       return "success";
    case IMGPREP_E_IO:     return "file read/write failed";
    case IMGPREP_E_DECODE: return "JPEG decode failed (corrupt file or not JPEG)";
    case IMGPREP_E_MEM:    return "out of memory";
    case IMGPREP_E_ENCODE: return "JPEG encoding failed";
    case IMGPREP_E_ARG:    return "invalid argument";
    case IMGPREP_E_SRC:    return "input source unavailable (e.g. no JPEG preview in 3FR)";
    default:               return "unknown error";
    }
}

/* ---------------- EXIF parsing (Orientation + ColorSpace) ---------------- */

/* Parse IFD0 tags 0x0112 Orientation and 0xA001 ColorSpace from raw APP1 (Exif) data
 * (1=sRGB, 2=uncalibrated; often 2 when Hasselblad/Photoshop exports AdobeRGB).
 * Set output to 0 when a tag is not found. */
static void exif_parse(const unsigned char *d, unsigned len,
                       int *orientation, int *colorspace)
{
    *orientation = 0;
    *colorspace = 0;
    if (len < 12 || memcmp(d, "Exif\0\0", 6) != 0) return;
    const unsigned char *t = d + 6;             /* TIFF header */
    unsigned tlen = len - 6;
    int be;                                      /* Byte order */
    if (t[0] == 'I' && t[1] == 'I') be = 0;
    else if (t[0] == 'M' && t[1] == 'M') be = 1;
    else return;
    #define RD16(p) (be ? ((p)[0] << 8) | (p)[1] : ((p)[1] << 8) | (p)[0])
    #define RD32(p) (be ? (RD16(p) << 16) | RD16((p) + 2) : (RD16((p) + 2) << 16) | RD16(p))
    if (RD16(t + 2) != 42) return;
    unsigned off = (unsigned)RD32(t + 4);
    if (off + 2 > tlen) return;
    int n = RD16(t + off);                       /* Number of IFD0 entries */
    for (int i = 0; i < n; i++) {
        unsigned e = off + 2 + (unsigned)i * 12; /* 12 bytes per entry */
        if (e + 12 > tlen) break;
        int tag = RD16(t + e);
        int typ = RD16(t + e + 2);
        int cnt = RD32(t + e + 4);
        if (typ != 3 || cnt < 1) continue;       /* SHORT only */
        int v = RD16(t + e + 8);                 /* Value is inline in first 2 bytes */
        if (tag == 0x0112 && v >= 1 && v <= 8) *orientation = v;
        if (tag == 0xA001) {
            /* 1=sRGB; 2=uncalibrated; 0xFFFF=unspecified → all become "not 1" for detection */
            *colorspace = (v == 1) ? 1 : ((v == 2) ? 2 : 0);
        }
    }
    #undef RD16
    #undef RD32
}

typedef struct {
    int orient;      /* EXIF Orientation 1..8; 0=none */
    int colorspace;  /* EXIF ColorSpace: 1=sRGB, 2=uncalibrated, 0=no tag */
} exif_info_t;

static exif_info_t find_exif(struct jpeg_decompress_struct *c)
{
    exif_info_t ei = { 0, 0 };
    for (jpeg_saved_marker_ptr m = c->marker_list; m; m = m->next)
        if (m->marker == JPEG_APP0 + 1 && m->data_length >= 12) {
            exif_parse(m->data, m->data_length, &ei.orient, &ei.colorspace);
            if (ei.orient) return ei;
        }
    return ei;
}

/* Detect AdobeRGB indicators (weak signals combined with EXIF ColorSpace):
 *  - APP2 ICC profile ("ICC_PROFILE\0") contains "Adobe RGB"/"AdobeRGB"
 *    (the desc tag in a real Adobe RGB (1998) ICC contains this string)
 *  - APP14 "Adobe" marker (written by Photoshop; sRGB files generally also have
 *    EXIF ColorSpace=1 and bypass this via cs==1; --nocm is an escape hatch). */
static int find_adobe_marker(struct jpeg_decompress_struct *c)
{
    static const char *keys[] = { "Adobe RGB", "AdobeRGB", NULL };
    for (jpeg_saved_marker_ptr m = c->marker_list; m; m = m->next) {
        if (m->marker == JPEG_APP0 + 2 && m->data_length > 16
            && memcmp(m->data, "ICC_PROFILE", 11) == 0) {
            unsigned n = m->data_length;
            if (n > 1024) n = 1024;              /* Search only the profile header */
            for (int k = 0; keys[k]; k++) {
                size_t kl = strlen(keys[k]);
                for (unsigned i = 0; i + kl <= n; i++)
                    if (memcmp(m->data + i, keys[k], kl) == 0) return 1;
            }
        } else if (m->marker == JPEG_APP0 + 14 && m->data_length >= 12
                   && memcmp(m->data, "Adobe", 5) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---------------- EXIF orientation transforms ---------------- */

/* Coordinate map: orient 1..8 → src(x,y,w,h) → dst(dx,dy), swap=swap output width/height */
static void orient_map(int o, int x, int y, int w, int h,
                       int *dx, int *dy)
{
    switch (o) {
    case 2:  *dx = w - 1 - x; *dy = y;         break; /* Mirror left/right */
    case 3:  *dx = w - 1 - x; *dy = h - 1 - y; break; /* 180° */
    case 4:  *dx = x;         *dy = h - 1 - y; break; /* Mirror top/bottom */
    case 5:  *dx = y;         *dy = x;         break; /* Transpose */
    case 6:  *dx = h - 1 - y; *dy = x;         break; /* Rotate 90° clockwise */
    case 7:  *dx = h - 1 - y; *dy = w - 1 - x; break; /* Transverse */
    case 8:  *dx = y;         *dy = w - 1 - x; break; /* Rotate 90° counterclockwise */
    default: *dx = x;         *dy = y;         break; /* 1: unchanged */
    }
}

/* Apply orientation transform to the (already scaled) buffer; return new buffer (free old
 * buffer) and fill output dimensions. */
static unsigned char *apply_orientation(unsigned char *src, int w, int h,
                                        int o, int *ow, int *oh)
{
    if (o <= 1) { /* No transform needed; reuse buffer */
        *ow = w;
        *oh = h;
        return src;
    }
    int swap = (o >= 5);
    int dw = swap ? h : w, dh = swap ? w : h;
    unsigned char *dst = (unsigned char *)malloc((size_t)dw * dh * 3);
    if (!dst) { free(src); *ow = *oh = 0; return NULL; }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int dx, dy;
            orient_map(o, x, y, w, h, &dx, &dy);
            memcpy(dst + ((size_t)dy * dw + dx) * 3,
                   src + ((size_t)y * w + x) * 3, 3);
        }
    free(src);
    *ow = dw;
    *oh = dh;
    return dst;
}

/* Implementation details. */

static void cubic_w(double f, double w[4])
{
    double f2 = f * f, f3 = f2 * f;
    w[0] = -0.5 * f3 + f2 - 0.5 * f;
    w[1] =  1.5 * f3 - 2.5 * f2 + 1.0;
    w[2] = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
    w[3] =  0.5 * f3 - 0.5 * f2;
}

static int clampei(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Implementation details. */
static void resize_bicubic(const unsigned char *src, int w, int h,
                           int cx, int cy, int cw, int ch,
                           unsigned char *dst, int tw, int th)
{
    double xr = (double)cw / tw, yr = (double)ch / th;
    for (int ty = 0; ty < th; ty++) {
        double sy0 = cy + (ty + 0.5) * yr - 0.5; /* Implementation details. */
        int syi = (int)floor(sy0);
        double fy = sy0 - syi;
        double wy[4];
        cubic_w(fy, wy);
        for (int tx = 0; tx < tw; tx++) {
            double sx0 = cx + (tx + 0.5) * xr - 0.5;
            int sxi = (int)floor(sx0);
            double fx = sx0 - sxi;
            double wx[4];
            cubic_w(fx, wx);
            double acc[3] = { 0, 0, 0 };
            for (int j = 0; j < 4; j++) {
                int syy = clampei(syi + j - 1, 0, h - 1);
                for (int i = 0; i < 4; i++) {
                    int sxx = clampei(sxi + i - 1, 0, w - 1);
                    double wt = wx[i] * wy[j];
                    const unsigned char *p = src + ((size_t)syy * w + sxx) * 3;
                    acc[0] += wt * p[0];
                    acc[1] += wt * p[1];
                    acc[2] += wt * p[2];
                }
            }
            unsigned char *q = dst + ((size_t)ty * tw + tx) * 3;
            for (int c = 0; c < 3; c++)
                q[c] = (unsigned char)clampei((int)(acc[c] + 0.5), 0, 255);
        }
    }
}

/* Implementation details. */

/* Implementation details. */
#define LANCZOS_A 3.0
#define LANCZOS_MAX_FS 16.0   /* Implementation details. */

static double lanczos_kernel(double t)
{
    if (t <= -LANCZOS_A || t >= LANCZOS_A) return 0.0;
    if (t == 0.0) return 1.0;
    double pi_t = M_PI * t;
    /* L(t) = sinc(t)·sinc(t/a)，sinc(x)=sin(πx)/(πx) */
    return LANCZOS_A * sin(pi_t) * sin(pi_t / LANCZOS_A)
           / (M_PI * M_PI * t * t);
}

/* Implementation details. */
typedef struct {
    int start;      /* Implementation details. */
    int taps;
    float *w;       /* Implementation details. */
} lrow_t;

/* Implementation details. */
static float *build_lweights(int src_len, int dst_len, lrow_t *rows)
{
    double scale = (double)src_len / dst_len;
    double fs = scale > 1.0 ? scale : 1.0;       /* Implementation details. */
    if (fs > LANCZOS_MAX_FS) fs = LANCZOS_MAX_FS;
    int max_taps = (int)(2.0 * LANCZOS_A * fs) + 3;
    float *pool = (float *)malloc(sizeof(float) * (size_t)dst_len * (size_t)max_taps);
    if (!pool) return NULL;
    for (int d = 0; d < dst_len; d++) {
        double center = (d + 0.5) * scale - 0.5; /* Implementation details. */
        int i0 = (int)floor(center - LANCZOS_A * fs);
        int i1 = (int)ceil(center + LANCZOS_A * fs);
        if (i1 - i0 + 1 > max_taps) i1 = i0 + max_taps - 1;
        float *w = pool + (size_t)d * max_taps;
        double sum = 0.0;
        for (int i = i0; i <= i1; i++) {
            double k = lanczos_kernel((i - center) / fs) / fs; /* Implementation details. */
            w[i - i0] = (float)k;
            sum += k;
        }
        if (sum != 0.0)
            for (int i = i0; i <= i1; i++) w[i - i0] = (float)(w[i - i0] / sum);
        rows[d].start = i0;
        rows[d].taps = i1 - i0 + 1;
        rows[d].w = w;
    }
    return pool;
}

/* Implementation details. */
static int resize_lanczos3(const unsigned char *src, int w,
                           int cx, int cy, int cw, int ch,
                           unsigned char *dst, int tw, int th)
{
    lrow_t *xr = (lrow_t *)malloc(sizeof(lrow_t) * (size_t)(tw + th));
    if (!xr) return IMGPREP_E_MEM;
    lrow_t *yr = xr + tw;
    float *xpool = build_lweights(cw, tw, xr);
    float *ypool = build_lweights(ch, th, yr);
    unsigned char *tmp = (unsigned char *)malloc((size_t)tw * ch * 3);
    if (!xpool || !ypool || !tmp) {
        free(xpool); free(ypool); free(tmp); free(xr);
        return IMGPREP_E_MEM;
    }

    /* Implementation details. */
    for (int y = 0; y < ch; y++) {
        const unsigned char *srow = src + ((size_t)(cy + y) * w) * 3;
        unsigned char *drow = tmp + (size_t)y * tw * 3;
        for (int dx = 0; dx < tw; dx++) {
            const lrow_t *r = &xr[dx];
            float acc0 = 0, acc1 = 0, acc2 = 0;
            for (int i = 0; i < r->taps; i++) {
                int sx = r->start + i;
                if (sx < 0) sx = 0;
                if (sx >= cw) sx = cw - 1;
                const unsigned char *p = srow + ((size_t)(cx + sx)) * 3;
                float wt = r->w[i];
                acc0 += wt * p[0];
                acc1 += wt * p[1];
                acc2 += wt * p[2];
            }
            unsigned char *q = drow + (size_t)dx * 3;
            q[0] = (unsigned char)clampei((int)(acc0 + 0.5f), 0, 255);
            q[1] = (unsigned char)clampei((int)(acc1 + 0.5f), 0, 255);
            q[2] = (unsigned char)clampei((int)(acc2 + 0.5f), 0, 255);
        }
    }

    /* Implementation details. */
    for (int ty = 0; ty < th; ty++) {
        const lrow_t *r = &yr[ty];
        unsigned char *drow = dst + (size_t)ty * tw * 3;
        for (int x = 0; x < tw; x++) {
            float acc0 = 0, acc1 = 0, acc2 = 0;
            for (int i = 0; i < r->taps; i++) {
                int sy = r->start + i;
                if (sy < 0) sy = 0;
                if (sy >= ch) sy = ch - 1;
                const unsigned char *p = tmp + ((size_t)sy * tw + x) * 3;
                float wt = r->w[i];
                acc0 += wt * p[0];
                acc1 += wt * p[1];
                acc2 += wt * p[2];
            }
            unsigned char *q = drow + (size_t)x * 3;
            q[0] = (unsigned char)clampei((int)(acc0 + 0.5f), 0, 255);
            q[1] = (unsigned char)clampei((int)(acc1 + 0.5f), 0, 255);
            q[2] = (unsigned char)clampei((int)(acc2 + 0.5f), 0, 255);
        }
    }

    free(xpool); free(ypool); free(tmp); free(xr);
    return IMGPREP_OK;
}

/* Implementation details. */

/* Implementation details. */
#define USHARP_SIGMA   1.0
#define USHARP_AMOUNT  0.35

static void unsharp_mask(unsigned char *img, int w, int h)
{
    const int R = 3;                             /* ceil(3σ) */
    double g[2 * 3 + 1];
    double gsum = 0;
    for (int i = -R; i <= R; i++) {
        g[i + R] = exp(-(double)(i * i) / (2.0 * USHARP_SIGMA * USHARP_SIGMA));
        gsum += g[i + R];
    }
    for (int i = 0; i < 2 * R + 1; i++) g[i] /= gsum;

    unsigned char *tmp  = (unsigned char *)malloc((size_t)w * h * 3);
    unsigned char *blur = (unsigned char *)malloc((size_t)w * h * 3);
    if (!tmp || !blur) { free(tmp); free(blur); return; }

    /* Implementation details. */
    for (int y = 0; y < h; y++) {
        const unsigned char *s = img + (size_t)y * w * 3;
        unsigned char *d = tmp + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            double a0 = 0, a1 = 0, a2 = 0;
            for (int i = -R; i <= R; i++) {
                int xx = clampei(x + i, 0, w - 1);
                double wt = g[i + R];
                const unsigned char *p = s + (size_t)xx * 3;
                a0 += wt * p[0]; a1 += wt * p[1]; a2 += wt * p[2];
            }
            d[0] = (unsigned char)clampei((int)(a0 + 0.5), 0, 255);
            d[1] = (unsigned char)clampei((int)(a1 + 0.5), 0, 255);
            d[2] = (unsigned char)clampei((int)(a2 + 0.5), 0, 255);
            d += 3;
        }
    }
    /* Implementation details. */
    for (int y = 0; y < h; y++) {
        unsigned char *d = blur + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            double a0 = 0, a1 = 0, a2 = 0;
            for (int i = -R; i <= R; i++) {
                int yy = clampei(y + i, 0, h - 1);
                double wt = g[i + R];
                const unsigned char *p = tmp + ((size_t)yy * w + x) * 3;
                a0 += wt * p[0]; a1 += wt * p[1]; a2 += wt * p[2];
            }
            d[0] = (unsigned char)clampei((int)(a0 + 0.5), 0, 255);
            d[1] = (unsigned char)clampei((int)(a1 + 0.5), 0, 255);
            d[2] = (unsigned char)clampei((int)(a2 + 0.5), 0, 255);
            d += 3;
        }
    }
    /* Implementation details. */
    size_t n = (size_t)w * h * 3;
    for (size_t i = 0; i < n; i++) {
        int d = (int)img[i] - (int)blur[i];
        int inc = (int)(USHARP_AMOUNT * d + (d < 0 ? -0.5 : 0.5));
        img[i] = (unsigned char)clampei((int)img[i] + inc, 0, 255);
    }
    free(tmp);
    free(blur);
}

/* ---------------- AdobeRGB(1998) → sRGB（D65，HQ） ---------------- */

/* Implementation details. */
static void adobergb_to_srgb(unsigned char *img, size_t npx)
{
    static double lin[256];                     /* Implementation details. */
    static float  enc[65536];                   /* Implementation details. */
    static double M[3][3];                      /* M = M_srgb · M_adobe */
    static int inited = 0;
    if (!inited) {
        static const double Ma[3][3] = {
            { 0.5767309, 0.1855540, 0.1881852 },
            { 0.2973769, 0.6273491, 0.0752751 },
            { 0.0270343, 0.0706872, 0.9911085 },
        };
        static const double Ms[3][3] = {
            {  3.2404542, -1.5371385, -0.4985314 },
            { -0.9692660,  1.8760108,  0.0415560 },
            {  0.0556434, -0.2040259,  1.0572252 },
        };
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                double s = 0;
                for (int k = 0; k < 3; k++) s += Ms[i][k] * Ma[k][j];
                M[i][j] = s;
            }
        for (int v = 0; v < 256; v++)
            lin[v] = pow((double)v / 255.0, 2.19921875);
        for (int i = 0; i < 65536; i++) {
            double y = (double)i / 65535.0;
            double o = (y <= 0.0031308) ? 12.92 * y
                                        : 1.055 * pow(y, 1.0 / 2.4) - 0.055;
            enc[i] = (float)(o * 255.0);
        }
        inited = 1;
    }
    for (size_t p = 0; p < npx; p++) {
        unsigned char *q = img + p * 3;
        double r = lin[q[0]], g = lin[q[1]], b = lin[q[2]];
        double y0 = M[0][0] * r + M[0][1] * g + M[0][2] * b;
        double y1 = M[1][0] * r + M[1][1] * g + M[1][2] * b;
        double y2 = M[2][0] * r + M[2][1] * g + M[2][2] * b;
        int i0 = clampei((int)(y0 * 65535.0 + 0.5), 0, 65535);
        int i1 = clampei((int)(y1 * 65535.0 + 0.5), 0, 65535);
        int i2 = clampei((int)(y2 * 65535.0 + 0.5), 0, 65535);
        q[0] = (unsigned char)clampei((int)(enc[i0] + 0.5f), 0, 255);
        q[1] = (unsigned char)clampei((int)(enc[i1] + 0.5f), 0, 255);
        q[2] = (unsigned char)clampei((int)(enc[i2] + 0.5f), 0, 255);
    }
}

/* Implementation details. */

static unsigned char *read_file_all(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc(sz > 0 ? (size_t)sz : 1);
    if (!buf) { fclose(f); return NULL; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

/* Implementation details. */

/* Implementation details. */
static int rotation_to_orient(int deg)
{
    switch (deg) {
    case 90:  return 6;
    case 180: return 3;
    case 270: return 8;
    default:  return 1;   /* 0 */
    }
}

/* Implementation details. */
static int effective_rotation_deg(int orient_override, int exif_orient)
{
    if (orient_override >= 0) return orient_override;
    switch (exif_orient) {
    case 3: case 4:   return 180;
    case 5: case 6:   return 90;
    case 7: case 8:   return 270;
    default:          return 0;   /* Implementation details. */
    }
}

int imgprep_edit_validate(imgprep_edit_t *e, int rotated_w, int rotated_h)
{
    if (!e) return IMGPREP_E_ARG;
    if (!(e->crop_x >= 0.0) || !(e->crop_y >= 0.0) ||
        !(e->crop_w > 0.0)  || !(e->crop_h > 0.0) ||
        !(e->crop_x <= 1.0) || !(e->crop_y <= 1.0))
        return IMGPREP_E_ARG;                       /* Implementation details. */
    if (e->crop_x + e->crop_w > 1.0) e->crop_w = 1.0 - e->crop_x;   /* Implementation details. */
    if (e->crop_y + e->crop_h > 1.0) e->crop_h = 1.0 - e->crop_y;
    if (!(e->crop_w > 0.0) || !(e->crop_h > 0.0)) return IMGPREP_E_ARG;
    /* Implementation details. */
    if (rotated_w > 0 && rotated_h > 0 &&
        (e->crop_w * rotated_w < IMGPREP_MIN_CROP_PX ||
         e->crop_h * rotated_h < IMGPREP_MIN_CROP_PX))
        return IMGPREP_E_ARG;
    if (e->rotation != -1 && e->rotation != 0 && e->rotation != 90 &&
        e->rotation != 180 && e->rotation != 270)
        return IMGPREP_E_ARG;
    return IMGPREP_OK;
}

/* Implementation details. */

static int prepare_core(const unsigned char *jpeg, size_t jpeg_len,
                        const char *out_path,
                        int target_w, int target_h, int quality,
                        unsigned flags, const imgprep_edit_t *edit,
                        imgprep_report_t *rep)
{
    if (!jpeg || !out_path || jpeg_len < 4) return IMGPREP_E_ARG;
    int fast = (flags & IMGPREP_F_FAST) ? 1 : 0;
    volatile int q = (quality > 0 && quality <= 100)
                     ? quality : (fast ? 92 : 95);     /* Implementation details. */
    volatile int fl = flags;
    /* Implementation details. */
    volatile double e_cx = 0, e_cy = 0, e_cw = 1, e_ch = 1;
    volatile int e_rot = -1;
    volatile int has_edit = 0;
    if (edit) {
        e_cx = edit->crop_x; e_cy = edit->crop_y;
        e_cw = edit->crop_w; e_ch = edit->crop_h;
        e_rot = edit->rotation;
        has_edit = 1;
    }

    /* Implementation details. */
    struct jpeg_decompress_struct dc;
    jerr_t jerr;
    dc.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jerr_exit;
    jerr.pub.output_message = jerr_noop;
    unsigned char *rgb = NULL;
    volatile int w = 0, h = 0;
    volatile int tw = target_w, th = target_h;

    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&dc);
        free(rgb);
        return IMGPREP_E_DECODE;
    }
    jpeg_create_decompress(&dc);
    jpeg_mem_src(&dc, jpeg, (unsigned long)jpeg_len);
    jpeg_save_markers(&dc, JPEG_APP0 + 1, 0xFFFF); /* Implementation details. */
    jpeg_save_markers(&dc, JPEG_APP0 + 2, 0xFFFF); /* Implementation details. */
    jpeg_save_markers(&dc, JPEG_APP0 + 14, 0xFFFF);/* Implementation details. */
    if (jpeg_read_header(&dc, TRUE) != JPEG_HEADER_OK)
        longjmp(jerr.jb, 1);

    int src_w = (int)dc.image_width, src_h = (int)dc.image_height;
    exif_info_t ei = find_exif(&dc);
    int cs = ei.colorspace;
    int adobe = find_adobe_marker(&dc);
    /* Implementation details. */
    int orient = has_edit && e_rot >= 0 ? rotation_to_orient(e_rot) : ei.orient;
    int ow = (orient >= 5) ? src_h : src_w; /* Implementation details. */
    int oh = (orient >= 5) ? src_w : src_h;

    /* Implementation details. */
    if (has_edit) {
        imgprep_edit_t ev;
        ev.crop_x = e_cx; ev.crop_y = e_cy;
        ev.crop_w = e_cw; ev.crop_h = e_ch;
        ev.rotation = e_rot;
        if (imgprep_edit_validate(&ev, ow, oh) != IMGPREP_OK) {
            jpeg_destroy_decompress(&dc);
            return IMGPREP_E_ARG;
        }
    }

    /* Implementation details. */
    if (tw <= 0 || th <= 0) {
        if (oh >= ow) { tw = IMGPREP_FALLBACK_W; th = IMGPREP_FALLBACK_H; }
        else          { tw = IMGPREP_FALLBACK_H; th = IMGPREP_FALLBACK_W; }
    }

    /* Implementation details. */
    static const int dens[] = { 8, 4, 2, 1 };
    int den = 1;
    for (unsigned i = 0; i < sizeof(dens) / sizeof(dens[0]); i++) {
        int sw = (src_w + dens[i] - 1) / dens[i];
        int sh = (src_h + dens[i] - 1) / dens[i];
        int rw = (orient >= 5) ? sh : sw, rh = (orient >= 5) ? sw : sh;
        if (fast) {
            if (rw >= tw && rh >= th) { den = dens[i]; break; }
        } else {
            if (rw >= 2 * tw && rh >= 2 * th) { den = dens[i]; break; }
        }
    }
    dc.scale_num = 1;
    dc.scale_denom = den;
    dc.out_color_space = JCS_RGB;
    jpeg_calc_output_dimensions(&dc);
    w = (int)dc.output_width;
    h = (int)dc.output_height;

    if (rep) {
        rep->src_w = src_w;   rep->src_h = src_h;
        rep->orientation = orient;
        rep->orient_w = ow;   rep->orient_h = oh;
        rep->dct_num = 1;     rep->dct_den = den;
        rep->dec_w = w;       rep->dec_h = h;
        rep->hq = !fast;
        rep->exif_colorspace = cs;
        rep->cm_converted = 0;
        rep->from_3fr = (flags & IMGPREP_IF_FROM3FR) ? 1 : 0;
        rep->src_bytes = (long)jpeg_len;
        rep->rotation = effective_rotation_deg(e_rot, ei.orient);
    }

    jpeg_start_decompress(&dc);
    rgb = (unsigned char *)malloc((size_t)w * h * 3);
    JSAMPROW row = (JSAMPROW)malloc((size_t)w * 3);
    if (!rgb || !row) {
        free(rgb); free(row);
        jpeg_destroy_decompress(&dc);
        return IMGPREP_E_MEM;
    }
    while (dc.output_scanline < dc.output_height) {
        int done = (int)dc.output_scanline; /* Implementation details. */
        JSAMPROW rp[1] = { row };
        jpeg_read_scanlines(&dc, rp, 1);
        memcpy(rgb + (size_t)done * w * 3, row, (size_t)w * 3);
    }
    free(row);
    jpeg_finish_decompress(&dc);
    jpeg_destroy_decompress(&dc);

    /* Implementation details. */
    int rw2 = w, rh2 = h;
    rgb = apply_orientation(rgb, w, h, orient, &rw2, &rh2);
    if (!rgb) return IMGPREP_E_MEM;

    /* Implementation details. */
    int ux = 0, uy = 0, uw = rw2, uh = rh2;
    if (has_edit) {
        ux = (int)(e_cx * rw2 + 0.5);
        uy = (int)(e_cy * rh2 + 0.5);
        uw = (int)(e_cw * rw2 + 0.5);
        uh = (int)(e_ch * rh2 + 0.5);
        /* Implementation details. */
        if (ux < 0) ux = 0;
        if (uy < 0) uy = 0;
        if (ux > rw2) ux = rw2;
        if (uy > rh2) uy = rh2;
        if (ux + uw > rw2) uw = rw2 - ux;
        if (uy + uh > rh2) uh = rh2 - uy;
        if (uw < 1) uw = 1;
        if (uh < 1) uh = 1;
    }

    /* Implementation details. */
    double ta = (double)tw / th;
    int cw, ch;
    if ((double)uw / uh > ta) { ch = uh; cw = (int)((double)uh * ta); }
    else                      { cw = uw; ch = (int)((double)uw / ta); }
    if (cw > uw) cw = uw;
    if (ch > uh) ch = uh;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    int cx = ux + (uw - cw) / 2, cy = uy + (uh - ch) / 2;
    if (rep) {
        rep->crop_x = cx; rep->crop_y = cy;
        rep->crop_w = cw; rep->crop_h = ch;
        rep->out_w = tw;  rep->out_h = th;
        rep->ucrop_x = ux; rep->ucrop_y = uy;
        rep->ucrop_w = uw; rep->ucrop_h = uh;
    }

    /* Implementation details. */
    unsigned char *out = (unsigned char *)malloc((size_t)tw * th * 3);
    if (!out) { free(rgb); return IMGPREP_E_MEM; }
    int rret;
    if (fast) {
        resize_bicubic(rgb, rw2, rh2, cx, cy, cw, ch, out, tw, th);
        rret = 0;
    } else {
        rret = resize_lanczos3(rgb, rw2, cx, cy, cw, ch, out, tw, th);
    }
    free(rgb);
    if (rret != 0) { free(out); return rret; }

    /* Implementation details. */
    if (!fast && !(fl & IMGPREP_F_NOCM) && cs != 1 && adobe) {
        adobergb_to_srgb(out, (size_t)tw * th);
        if (rep) rep->cm_converted = 1;
    }

    /* Implementation details. */
    if (!fast && !(fl & IMGPREP_F_NOSHRP))
        unsharp_mask(out, tw, th);

    /* Implementation details. */
    FILE *fout = fopen(out_path, "wb");
    if (!fout) { free(out); return IMGPREP_E_IO; }
    struct jpeg_compress_struct co;
    jerr_t cerr;
    co.err = jpeg_std_error(&cerr.pub);
    cerr.pub.error_exit = jerr_exit;
    cerr.pub.output_message = jerr_noop;
    volatile int ret = IMGPREP_OK; /* Implementation details. */
    if (setjmp(cerr.jb)) {
        ret = IMGPREP_E_ENCODE;
        goto enc_done;
    }
    jpeg_create_compress(&co);
    jpeg_stdio_dest(&co, fout);
    co.image_width = (JDIMENSION)tw;
    co.image_height = (JDIMENSION)th;
    co.input_components = 3;
    co.in_color_space = JCS_RGB;
    jpeg_set_defaults(&co);
    jpeg_set_quality(&co, q, TRUE);
    if (fast) {
        co.comp_info[0].h_samp_factor = 2; /* Implementation details. */
        co.comp_info[0].v_samp_factor = 1;
    } else {
        co.comp_info[0].h_samp_factor = 1; /* Implementation details. */
        co.comp_info[0].v_samp_factor = 1;
    }
    co.comp_info[1].h_samp_factor = 1;
    co.comp_info[1].v_samp_factor = 1;
    co.comp_info[2].h_samp_factor = 1;
    co.comp_info[2].v_samp_factor = 1;
    co.density_unit = 1;                /* Implementation details. */
    co.X_density = 300;
    co.Y_density = 300;
    jpeg_start_compress(&co, TRUE);
    while (co.next_scanline < co.image_height) {
        JSAMPROW rp[1] = { out + (size_t)co.next_scanline * tw * 3 };
        jpeg_write_scanlines(&co, rp, 1);
    }
    jpeg_finish_compress(&co);
enc_done:
    jpeg_destroy_compress(&co);
    if (ret == IMGPREP_OK && rep) {
        rep->out_bytes = ftell(fout);
    }
    fclose(fout);
    free(out);
    return ret;
}

/* Implementation details. */

int imgprep_prepare(const char *in_path, const char *out_path,
                    int target_w, int target_h, int quality,
                    imgprep_report_t *rep)
{
    return imgprep_prepare_ex(in_path, out_path, target_w, target_h,
                              quality, 0u, rep);
}

int imgprep_prepare_ex(const char *in_path, const char *out_path,
                       int target_w, int target_h, int quality,
                       unsigned flags, imgprep_report_t *rep)
{
    return imgprep_prepare_edit(in_path, out_path, target_w, target_h,
                                quality, flags, NULL, rep);
}

int imgprep_prepare_edit(const char *in_path, const char *out_path,
                         int target_w, int target_h, int quality,
                         unsigned flags, const imgprep_edit_t *edit,
                         imgprep_report_t *rep)
{
    if (!in_path || !out_path) return IMGPREP_E_ARG;
    size_t len = 0;
    unsigned char *buf = read_file_all(in_path, &len);
    if (!buf) return IMGPREP_E_IO;
    int e = prepare_core(buf, len, out_path, target_w, target_h,
                         quality, flags, edit, rep);
    free(buf);
    return e;
}

int imgprep_prepare_mem(const unsigned char *jpeg, size_t jpeg_len,
                        const char *out_path, int target_w, int target_h,
                        int quality, unsigned flags, imgprep_report_t *rep)
{
    return prepare_core(jpeg, jpeg_len, out_path, target_w, target_h,
                        quality, flags, NULL, rep);
}

int imgprep_prepare_mem_edit(const unsigned char *jpeg, size_t jpeg_len,
                             const char *out_path, int target_w, int target_h,
                             int quality, unsigned flags,
                             const imgprep_edit_t *edit, imgprep_report_t *rep)
{
    return prepare_core(jpeg, jpeg_len, out_path, target_w, target_h,
                        quality, flags, edit, rep);
}

int imgprep_from_3fr(const char *path, const char *out_path,
                     int target_w, int target_h, int quality,
                     unsigned flags, imgprep_report_t *rep)
{
    return imgprep_from_3fr_edit(path, out_path, target_w, target_h,
                                 quality, flags, NULL, rep);
}

int imgprep_from_3fr_edit(const char *path, const char *out_path,
                          int target_w, int target_h, int quality,
                          unsigned flags, const imgprep_edit_t *edit,
                          imgprep_report_t *rep)
{
    if (!path || !out_path) return IMGPREP_E_ARG;
    unsigned char *jb = NULL;
    size_t jl = 0;
    int he = h3fr_extract_preview(path, &jb, &jl);
    if (he != H3FR_OK) return IMGPREP_E_SRC;
    int e = prepare_core(jb, jl, out_path, target_w, target_h,
                         quality, flags | IMGPREP_IF_FROM3FR, edit, rep);
    free(jb);
    return e;
}

/* Implementation details. */

static int probe_mem(const unsigned char *jpeg, size_t len, imgprep_info_t *info)
{
    struct jpeg_decompress_struct dc;
    jerr_t jerr;
    dc.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jerr_exit;
    jerr.pub.output_message = jerr_noop;
    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&dc);
        return IMGPREP_E_DECODE;
    }
    jpeg_create_decompress(&dc);
    jpeg_mem_src(&dc, jpeg, (unsigned long)len);
    jpeg_save_markers(&dc, JPEG_APP0 + 1, 0xFFFF);      /* Exif(Orientation) */
    if (jpeg_read_header(&dc, TRUE) != JPEG_HEADER_OK)
        longjmp(jerr.jb, 1);
    exif_info_t ei = find_exif(&dc);
    info->width = (int)dc.image_width;
    info->height = (int)dc.image_height;
    info->orientation = ei.orient;
    int sw = (ei.orient >= 5);
    info->rotated_width = sw ? info->height : info->width;
    info->rotated_height = sw ? info->width : info->height;
    jpeg_destroy_decompress(&dc);
    return IMGPREP_OK;
}

int imgprep_probe(const char *path, imgprep_info_t *info)
{
    if (!path || !info) return IMGPREP_E_ARG;
    memset(info, 0, sizeof *info);

    size_t plen = strlen(path);
    int is_3fr = (plen >= 4 &&
                  (strcasecmp(path + plen - 4, ".3fr") == 0));
    unsigned char *jb = NULL;
    size_t jl = 0;
    int e;
    if (is_3fr) {
        int he = h3fr_extract_preview(path, &jb, &jl);
        if (he != H3FR_OK) return IMGPREP_E_SRC;
        e = probe_mem(jb, jl, info);
        free(jb);
        if (e != IMGPREP_OK) return e;
        info->is_3fr = 1;
    } else {
        jb = read_file_all(path, &jl);
        if (!jb) return IMGPREP_E_IO;
        e = probe_mem(jb, jl, info);
        free(jb);
        if (e != IMGPREP_OK) return e;
        info->has_jpeg = 1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return IMGPREP_E_IO;
    int rc = fseek(f, 0, SEEK_END);
    long sz = rc == 0 ? ftell(f) : -1;
    fclose(f);
    if (sz < 0) return IMGPREP_E_IO;
    info->file_size = sz;
    return IMGPREP_OK;
}
