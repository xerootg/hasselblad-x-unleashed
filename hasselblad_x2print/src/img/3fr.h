/* 3fr.h — Extract the embedded JPEG preview from Hasselblad .3FR RAW files
 *
 * Background: no real 3FR samples are available; implementation follows standard TIFF
 * semantics and clues from Hasselblad firmware:
 *   - 3FR has a TIFF structure (II/MM byte order + magic 42 + IFD chain);
 *   - The main IFD's StripOffsets/StripByteCounts point to PhotoData (raw sensor data,
 *     not JPEG);
 *   - Firmware strings from Hasselblad cameras include:
 *       E_Resolutions_PreviewFullEmbedded   — full-size embedded preview
 *       MetadataIFDPreview                  — metadata IFD containing the preview
 *     Thus, the preview is attached somewhere in the IFD chain/SubIFD as JPEG strips
 *     (Strip/JPEGInterchangeFormat). This module traverses all IFDs, collects strip
 *     groups that start with SOI (FFD8), selects the one with the most bytes
 *     (= PreviewFullEmbedded; smaller ones are thumbnails), and returns the complete
 *     JPEG stream for the imgprep pipeline.
 *   - Calibration against camera samples is pending (see NOTES.md §11).
 */
#ifndef X2P_3FR_H
#define X2P_3FR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    H3FR_OK        = 0,
    H3FR_E_IO      = -1,  /* Failed to open/read file */
    H3FR_E_MEM     = -2,  /* Out of memory */
    H3FR_E_NOTIFF  = -3,  /* Invalid TIFF structure (not 3FR/corrupt) */
    H3FR_E_NOPREV  = -4,  /* No recognized JPEG preview found after traversing IFDs */
    H3FR_E_TRUNC   = -5,  /* Strip out of range/truncated (corrupt file) */
};

const char *h3fr_strerror(int err);

/* Extract the largest embedded JPEG preview (PreviewFullEmbedded) from path.
 * On success, returns H3FR_OK and sets *out_jpeg to a malloc-allocated JPEG stream
 * starting with FFD8, with length *out_len; the caller must free() it. On failure,
 * returns one of the negative values above and leaves the pointers unchanged. */
int h3fr_extract_preview(const char *path, unsigned char **out_jpeg,
                         size_t *out_len);

#ifdef __cplusplus
}
#endif
#endif /* X2P_3FR_H */
