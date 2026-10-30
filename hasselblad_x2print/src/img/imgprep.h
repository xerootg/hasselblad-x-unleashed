/* imgprep.h — High-quality print image preprocessing (HQ pipeline + 3FR support)
 *
 * Goal: convert any JPEG or Hasselblad .3FR RAW (embedded preview) into a
 *       printer-ready JPEG of the target dimensions. This is a general-purpose
 *       printer program: callers explicitly pass target dimensions (px), calculated
 *       from the detected physical media size × printer resolution.
 *       Passing 0,0 uses fallback dimensions only (1200×1800 / 1800×1200, based on EXIF).
 *
 * HQ pipeline (default, phase §11):
 *   1) Decode with libjpeg-turbo DCT scaling, choosing the smallest scale at least
 *      2× the target (first of two scaling stages; avoids expanding a 100MP image fully).
 *   2) Rotate according to EXIF Orientation 1–8, then center-crop to the target aspect ratio.
 *   3) Lanczos3 (a=3, widen the kernel with the scale factor for prefiltering, clamp edges)
 *      to the exact target dimensions (second scaling stage).
 *   4) Apply an unsharp mask (Gaussian σ=1.0px@300dpi, amount 0.35, threshold 0).
 *   5) Color: pass through as sRGB for EXIF ColorSpace=1; convert AdobeRGB to sRGB with
 *      a matrix for ColorSpace=2/untagged images with AdobeRGB indicators
 *      (ICC "Adobe RGB" / APP14 Adobe).
 *   6) Encode JPEG: default q95, 4:4:4 (no chroma subsampling), 300dpi metadata.
 *
 * FAST pipeline (backward compatibility): stop at the first DCT scale at least the target
 *   + Catmull-Rom bicubic, default q92, 4:2:2, no sharpening or color conversion.
 *
 * Free crop/rotation (§18, GUI editor):
 *   Fixed pipeline order: decode → rotate (EXIF auto or explicit 0/90/180/270 override) →
 *   crop (normalized rectangle defined on the **rotated** image = GUI preview coordinates) →
 *   center-fit to media aspect ratio → scale → sharpen → encode.
 *   Explicit rotation overrides EXIF (no stacking); crop bounds are clamped and minimum
 *   dimensions validated (if either crop dimension is < IMGPREP_MIN_CROP_PX pixels,
 *   return IMGPREP_E_ARG).
 */
#ifndef X2P_IMGPREP_H
#define X2P_IMGPREP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default fallback dimensions when capabilities are unavailable (4x6in @ 300dpi;
   use WxH or HxW depending on orientation) */
#define IMGPREP_FALLBACK_W 1200
#define IMGPREP_FALLBACK_H 1800

/* Pipeline flags (bits for imgprep_prepare_ex) */
#define IMGPREP_F_FAST 1u   /* Legacy pipeline: direct DCT + bicubic + q92 4:2:2, no sharpening/CM */
#define IMGPREP_F_NOCM 2u   /* Skip AdobeRGB→sRGB in HQ mode (escape hatch) */
/* Note: the internal imgprep test hook IMGPREP_F_NOSHRP (4u) disables HQ sharpening
 * for sharpness comparison tests; it is not part of the public interface. */
#define IMGPREP_F_NOSHRP 4u

/* User crop/rotation edit parameters (imgprep_edit_t; NULL = no edits = full frame + EXIF auto).
 * Coordinate contract for the GUI editor (see INTERFACE.md v2.1 §3b):
 *   - Four normalized crop values in 0..1, defined on the **rotated** image (= GUI preview);
 *     x/y are measured from the top-left, w/h are width/height fractions; default/full frame
 *     is 0,0,1,1.
 *   - rotation: -1 = EXIF auto (current behavior); 0/90/180/270 = explicit clockwise
 *     rotation that **overrides** EXIF Orientation (no stacking).
 *   - Clamp bounds: if x+w>1, set w=1-x (same for y); clamp negative x/y to 0.
 *     After clamping, return IMGPREP_E_ARG if either source crop dimension is
 *     < IMGPREP_MIN_CROP_PX pixels. */
/* Normalize imgprep_edit for callers to validate/clamp values before decoding.
 * Returns 0 if valid (clamps the structure in place), or IMGPREP_E_ARG if invalid
 * (NaN/Inf, w/h≤0, zero-size window after clamping, etc.). */
#define IMGPREP_MIN_CROP_PX 100

typedef struct {
    double crop_x, crop_y, crop_w, crop_h;   /* Normalized crop window (rotated coordinates) */
    int     rotation;                        /* -1=EXIF auto; explicit 0/90/180/270 */
} imgprep_edit_t;

int imgprep_edit_validate(imgprep_edit_t *e, int rotated_w, int rotated_h);

/* Processing report (for selftest output and service logs); fields may only be added, never removed. */
typedef struct {
    int src_w, src_h;         /* Source image dimensions */
    int orientation;          /* EXIF Orientation 1..8; 0 = no EXIF */
    int orient_w, orient_h;   /* Logical dimensions after EXIF rotation */
    int dct_num, dct_den;     /* DCT scaled-decode ratio (e.g. 1/4) */
    int dec_w, dec_h;         /* Actual decoded DCT dimensions (unrotated) */
    int crop_x, crop_y, crop_w, crop_h; /* Center crop window (rotated coordinates) */
    int out_w, out_h;         /* Output dimensions */
    long out_bytes;           /* Output JPEG size in bytes */
    /* §11 additions */
    int hq;                   /* 1 = HQ pipeline (Lanczos3 + sharpening + q95 4:4:4) */
    int exif_colorspace;      /* EXIF ColorSpace: 1=sRGB, 2=untagged, 0=tag absent */
    int cm_converted;         /* 1 = AdobeRGB→sRGB conversion performed */
    int from_3fr;             /* 1 = input extracted from a 3FR preview */
    long src_bytes;           /* Input JPEG/preview size in bytes */
    /* §18 additions (free crop/rotation) */
    int rotation;             /* Effective rotation in degrees: 0/90/180/270 (EXIF auto resolved) */
    int ucrop_x, ucrop_y;     /* User crop window (rotated pixel coordinates; 0,0 if uncropped) */
    int ucrop_w, ucrop_h;     /* User crop dimensions (full frame if uncropped) */
} imgprep_report_t;

enum {
    IMGPREP_OK      = 0,
    IMGPREP_E_IO    = -1,  /* File open/read/write failed */
    IMGPREP_E_DECODE= -2,  /* Invalid JPEG / decode error */
    IMGPREP_E_MEM   = -3,  /* Out of memory */
    IMGPREP_E_ENCODE= -4,  /* JPEG encoding error */
    IMGPREP_E_ARG   = -5,  /* Invalid argument */
    IMGPREP_E_SRC   = -6,  /* Input source unavailable (e.g. no 3FR preview) */
};

const char *imgprep_strerror(int err);

/* Compatibility entry point (phase 2 API unchanged): pass target dimensions explicitly;
 * 0,0 = fallback dimensions in the 1200×1800 family. Quality 0 = default (HQ:95 / FAST:92).
 * rep may be NULL. */
int imgprep_prepare(const char *in_path, const char *out_path,
                    int target_w, int target_h, int quality,
                    imgprep_report_t *rep);

/* Entry point with pipeline flags (see IMGPREP_F_*). */
int imgprep_prepare_ex(const char *in_path, const char *out_path,
                       int target_w, int target_h, int quality,
                       unsigned flags, imgprep_report_t *rep);

/* In-memory JPEG source (reuse the full pipeline after extracting a 3FR preview).
 * jpeg is valid only for the duration of the call. */
int imgprep_prepare_mem(const unsigned char *jpeg, size_t jpeg_len,
                        const char *out_path, int target_w, int target_h,
                        int quality, unsigned flags, imgprep_report_t *rep);

/* Hasselblad .3FR RAW: extract the largest embedded JPEG preview (3fr.c), then use
 * the prepare_mem pipeline. Returns IMGPREP_E_SRC if no preview exists or the file is not TIFF. */
int imgprep_from_3fr(const char *path, const char *out_path,
                     int target_w, int target_h, int quality,
                     unsigned flags, imgprep_report_t *rep);

/* ---------------- §18 Entry points with edit parameters (crop/rotation) ----------------
 * See imgprep_edit_t comments for semantics; edit=NULL is equivalent to the corresponding
 * legacy entry point above (full frame + EXIF auto). */
int imgprep_prepare_edit(const char *in_path, const char *out_path,
                         int target_w, int target_h, int quality,
                         unsigned flags, const imgprep_edit_t *edit,
                         imgprep_report_t *rep);
int imgprep_prepare_mem_edit(const unsigned char *jpeg, size_t jpeg_len,
                             const char *out_path, int target_w, int target_h,
                             int quality, unsigned flags,
                             const imgprep_edit_t *edit, imgprep_report_t *rep);
int imgprep_from_3fr_edit(const char *path, const char *out_path,
                          int target_w, int target_h, int quality,
                          unsigned flags, const imgprep_edit_t *edit,
                          imgprep_report_t *rep);

/* ---------------- Header probe (does not decode pixels; source for GUI doGet_photo_info) ----------------
 * .3fr/.3FR: extract embedded preview, then fill width/height/orientation from its header;
 * is_3fr=1 (has_jpeg=0, file_size=the RAW file's size).
 * JPEG: has_jpeg=1, is_3fr=0. rotated_* are logical dimensions after EXIF Orientation
 * is applied (the GUI swaps width/height itself for explicit rotation).
 * Failure returns IMGPREP_E_IO/E_DECODE/E_SRC. */
typedef struct {
    int  width, height;          /* Stored dimensions (3FR = embedded preview dimensions) */
    int  orientation;            /* EXIF Orientation 1..8; 0 = no EXIF */
    int  rotated_width, rotated_height; /* Logical dimensions after EXIF */
    int  has_jpeg;               /* 1 = file itself is JPEG (can decode directly) */
    int  is_3fr;                 /* 1 = Hasselblad RAW (uses embedded preview) */
    long file_size;              /* File size in bytes (3FR = RAW file itself) */
} imgprep_info_t;

int imgprep_probe(const char *path, imgprep_info_t *info);

#ifdef __cplusplus
}
#endif
#endif /* X2P_IMGPREP_H */
