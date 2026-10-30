/* Implementation details. */
#include "imgprep.h"
#include "3fr.h"

#include <stdio.h> /* Implementation details. */
#include <jpeglib.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <time.h>

/* Implementation details. */
static int dump_corners(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Operation failed %s", path); return -1; }
    struct jpeg_decompress_struct c;
    struct jpeg_error_mgr jerr;
    c.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&c);
    jpeg_stdio_src(&c, f);
    if (jpeg_read_header(&c, TRUE) != JPEG_HEADER_OK) {
        fprintf(stderr, "Operation failed");
        jpeg_destroy_decompress(&c);
        fclose(f);
        return -1;
    }
    jpeg_start_decompress(&c);
    int w = (int)c.output_width, h = (int)c.output_height;
    unsigned char *rgb = malloc((size_t)w * h * 3);
    unsigned char *row = malloc((size_t)w * 3);
    if (!rgb || !row) { free(rgb); free(row); jpeg_destroy_decompress(&c); fclose(f); return -1; }
    while (c.output_scanline < c.output_height) {
        int done = (int)c.output_scanline;
        JSAMPROW rp[1] = { row };
        jpeg_read_scanlines(&c, rp, 1);
        memcpy(rgb + (size_t)done * w * 3, row, (size_t)w * 3);
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    fclose(f);
    free(row);

    /* Implementation details. */
    struct { const char *nm; int x, y; } corners[] = {
        { "Operation failed", 0, 0 }, { "Operation failed", w - 16, 0 },
        { "Operation failed", 0, h - 16 }, { "Operation failed", w - 16, h - 16 },
    };
    printf("corners %dx%d:", w, h);
    for (unsigned i = 0; i < sizeof(corners) / sizeof(corners[0]); i++) {
        long r = 0, g = 0, b = 0;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                const unsigned char *p = rgb + ((size_t)(corners[i].y + y) * w +
                                                (corners[i].x + x)) * 3;
                r += p[0]; g += p[1]; b += p[2];
            }
        printf(" %s=(%ld,%ld,%ld)", corners[i].nm, r / 256, g / 256, b / 256);
    }
    printf("\n");
    free(rgb);
    return 0;
}

static double now_sec(void)
{
    return (double)clock() / CLOCKS_PER_SEC;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "Usage %s"
                "[--corners]\n"
                "      [--hq|--fast] [--nocm] [--from-3fr] [--no-sharpen] "
                "[--extract-preview <file>]\n"
                "Operation failed",
                argv[0]);
        return 1;
    }
    const char *in = argv[1];
    char out[512];
    snprintf(out, sizeof(out), "%s", in);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
    strncat(out, "_print.jpg", sizeof(out) - strlen(out) - 1);

    int tw = 0, th = 0, q = 0, corners = 0;
    unsigned flags = 0;
    int from_3fr = 0, no_sharpen = 0, mode_seen = 0;
    const char *extract_to = NULL;
    imgprep_edit_t edit;
    memset(&edit, 0, sizeof edit);
    edit.crop_x = 0; edit.crop_y = 0; edit.crop_w = 1; edit.crop_h = 1;
    edit.rotation = -1;
    int edit_given = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &tw, &th) != 2 || tw < 1 || th < 1) {
                fprintf(stderr, "Operation failed");
                return 1;
            }
        } else if (!strcmp(argv[i], "--q") && i + 1 < argc) {
            q = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--corners")) {
            corners = 1;
        } else if (!strcmp(argv[i], "--hq")) {
            flags &= ~(unsigned)IMGPREP_F_FAST;
            mode_seen = 1;
        } else if (!strcmp(argv[i], "--fast")) {
            flags |= IMGPREP_F_FAST;
            mode_seen = 1;
        } else if (!strcmp(argv[i], "--nocm")) {
            flags |= IMGPREP_F_NOCM;
        } else if (!strcmp(argv[i], "--from-3fr")) {
            from_3fr = 1;
        } else if (!strcmp(argv[i], "--no-sharpen")) {
            no_sharpen = 1;
        } else if (!strcmp(argv[i], "--crop") && i + 1 < argc) {
            if (sscanf(argv[++i], "%lf,%lf,%lf,%lf", &edit.crop_x, &edit.crop_y,
                       &edit.crop_w, &edit.crop_h) != 4) {
                fprintf(stderr, "Operation failed");
                return 1;
            }
            edit_given = 1;
        } else if (!strcmp(argv[i], "--rotate") && i + 1 < argc) {
            edit.rotation = atoi(argv[++i]);
            if (edit.rotation != -1 && edit.rotation != 0 &&
                edit.rotation != 90 && edit.rotation != 180 &&
                edit.rotation != 270) {
                fprintf(stderr, "Operation failed");
                return 1;
            }
            edit_given = 1;
        } else if (!strcmp(argv[i], "--extract-preview") && i + 1 < argc) {
            extract_to = argv[++i];
        } else if (i == 2 && !extract_to) {
            snprintf(out, sizeof(out), "%s", argv[i]);
        } else {
            fprintf(stderr, "Unknown option %s", argv[i]);
            return 1;
        }
    }
    if (!mode_seen) flags &= ~(unsigned)IMGPREP_F_FAST; /* Implementation details. */
    if (no_sharpen) flags |= IMGPREP_F_NOSHRP;

    /* Implementation details. */
    size_t inlen = strlen(in);
    if (inlen >= 5 && strcasecmp(in + inlen - 4, ".3fr") == 0)
        from_3fr = 1;

    if (extract_to) {
        unsigned char *jb = NULL;
        size_t jl = 0;
        int e = h3fr_extract_preview(in, &jb, &jl);
        if (e != H3FR_OK) {
            fprintf(stderr, "Operation failed %s", h3fr_strerror(e));
            return 1;
        }
        FILE *f = fopen(extract_to, "wb");
        if (!f || fwrite(jb, 1, jl, f) != jl) {
            fprintf(stderr, "Operation failed %s", extract_to);
            free(jb);
            if (f) fclose(f);
            return 1;
        }
        fclose(f);
        printf("Operation failed %zu %s", jl, extract_to);
        free(jb);
        return 0;
    }

    printf("Operation failed %s %s %s %s %d",
           (flags & IMGPREP_F_FAST) ? "FAST" : "HQ", in, out,
           tw ? "Operation failed" : "Operation failed",
           q ? q : ((flags & IMGPREP_F_FAST) ? 92 : 95));
    imgprep_report_t r;
    memset(&r, 0, sizeof r);
    double t0 = now_sec();
    const imgprep_edit_t *pe = edit_given ? &edit : NULL;
    int e = from_3fr
        ? imgprep_from_3fr_edit(in, out, tw, th, q, flags, pe, &r)
        : imgprep_prepare_edit(in, out, tw, th, q, flags, pe, &r);
    double dt = now_sec() - t0;
    if (e != IMGPREP_OK) {
        fprintf(stderr, "Operation failed %s", imgprep_strerror(e));
        return 1;
    }
    printf("Operation failed %d %d %d %s %d",
           r.src_w, r.src_h, r.orientation,
           r.orientation ? "" : "Operation failed", r.exif_colorspace);
    printf("Operation failed %d %s", r.rotation,
           pe && pe->rotation >= 0 ? "Operation failed" : "Operation failed");
    printf("Operation failed %s %d %s",
           r.hq ? "HQ" : "FAST", r.dct_den,
           r.hq ? "Operation failed" : "Operation failed");
    printf("Operation failed %d %d %d %d",
           r.dec_w, r.dec_h, r.orient_w, r.orient_h);
    if (pe)
        printf("Operation failed %d %d %d %d"
               "%.3f,%.3f,%.3f,%.3f）\n",
               r.ucrop_x, r.ucrop_y, r.ucrop_w, r.ucrop_h,
               pe->crop_x, pe->crop_y, pe->crop_w, pe->crop_h);
    printf("Operation failed %d %d %d %d", r.crop_x, r.crop_y, r.crop_w, r.crop_h);
    printf("Operation failed %s %s",
           r.cm_converted ? "Operation failed" : "Operation failed",
           (flags & IMGPREP_F_NOCM) ? "（--nocm）" : "");
    printf("Operation failed %s",
           (r.hq && !(flags & IMGPREP_F_NOSHRP)) ? "unsharp σ=1.0 amount=0.35" : "Operation failed");
    if (r.from_3fr) printf("Operation failed %ld", r.src_bytes);
    printf("Operation failed %d %d %ld %d %s",
           r.out_w, r.out_h, r.out_bytes,
           q ? q : (r.hq ? 95 : 92), r.hq ? "4:4:4" : "4:2:2");
    printf("Operation failed %.3f", dt);
    if (corners) dump_corners(out);
    printf("Operation completed %.1f %.1f",
           r.out_w * 25.4 / 300.0, r.out_h * 25.4 / 300.0);
    return 0;
}
