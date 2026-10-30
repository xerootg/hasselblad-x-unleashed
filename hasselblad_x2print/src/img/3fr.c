/* 3fr.c — Extract the embedded JPEG preview from Hasselblad .3FR RAW files (TIFF structure parsing)
 *
 * Implementation notes (no real samples are available; follows standard TIFF semantics,
 * see the header comments in 3fr.h):
 *   1) Header: byte-order marker II/MM + magic 42 + IFD0 offset; otherwise H3FR_E_NOTIFF.
 *   2) Traverse IFDs: recurse through the NextIFD chain and SubIFDs (0x014A); limit depth
 *      to 4, total IFDs to 64, entries per IFD to 4096, and deduplicate offsets to prevent
 *      loops or slow paths on corrupt files.
 *   3) Collect candidate JPEG segments from each IFD:
 *      a) JPEGInterchangeFormat (0x0201) + Length (0x0202), the standard way to embed
 *         JPEG thumbnails/previews in TIFF;
 *      b) StripOffsets (0x0111) + StripByteCounts (0x0117): accept the strip group as JPEG
 *         only if its first strip starts with SOI (FFD8). Raw PhotoData strips do not,
 *         and are skipped. Concatenate multiple strips in order as one JPEG stream.
 *   4) Select the largest candidate whose SOI check passes (the largest is
 *      PreviewFullEmbedded; smaller ones are thumbnails); if none qualify, return
 *      H3FR_E_NOPREVIEW.
 *   5) Use fseek/fread only for the matching ranges instead of loading the whole file
 *      (3FR files can be hundreds of MB). Check all offsets and lengths against file size;
 *      out-of-range data returns H3FR_E_TRUNC.
 */
#include "3fr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Traversal safety limits for corrupt files */
#define H3FR_MAX_IFDS    64
#define H3FR_MAX_DEPTH   4
#define H3FR_MAX_ENTRIES 4096

const char *h3fr_strerror(int err)
{
    switch (err) {
    case H3FR_OK:        return "success";
    case H3FR_E_IO:      return "failed to open/read 3FR file";
    case H3FR_E_MEM:     return "out of memory";
    case H3FR_E_NOTIFF:  return "invalid TIFF structure (not 3FR or corrupt)";
    case H3FR_E_NOPREV:  return "no recognized JPEG preview found in 3FR file";
    case H3FR_E_TRUNC:   return "3FR strip out of range or file truncated";
    default:             return "unknown error";
    }
}

/* ---------------- Low-level reading ---------------- */

typedef struct {
    FILE *f;
    long  fsize;
    int   be;                          /* 1=big-endian(MM) */
    long  visited[H3FR_MAX_IFDS];      /* Visited IFD offsets (loop prevention) */
    int   nvisited;
} tr_t;

static int tr_read(tr_t *t, long off, void *buf, size_t n)
{
    if (off < 0 || n == 0 || off + (long)n > t->fsize) return -1;
    if (fseek(t->f, off, SEEK_SET) != 0) return -1;
    if (fread(buf, 1, n, t->f) != n) return -1;
    return 0;
}

static unsigned rd16(const unsigned char *p, int be)
{
    return be ? ((unsigned)p[0] << 8) | p[1]
              : ((unsigned)p[1] << 8) | p[0];
}

static unsigned long rd32(const unsigned char *p, int be)
{
    return be ? ((unsigned long)rd16(p, be) << 16) | rd16(p + 2, be)
              : ((unsigned long)rd16(p + 2, be) << 16) | rd16(p, be);
}

/* JPEG candidate: concatenate segments (off,len) in order */
typedef struct {
    long   *off;
    size_t *len;
    size_t  n;
    size_t  total;
} cand_t;

static void cand_free(cand_t *c)
{
    free(c->off);
    free(c->len);
    c->off = NULL;
    c->len = NULL;
    c->n = c->total = 0;
}

/* Replace best if c is larger (transfer ownership; clear or release c) */
static void cand_keep_better(cand_t *c, cand_t *best)
{
    if (c->n == 0 || c->total == 0) { cand_free(c); return; }
    if (c->total > best->total) {
        cand_free(best);
        *best = *c;                    /* Ownership transferred */
        memset(c, 0, sizeof(*c));
    } else {
        cand_free(c);
    }
}

/* Read an IFD entry's value array (expand BYTE/SHORT/LONG into unsigned longs).
 * Returns a malloc-allocated array of entry count elements, or NULL on failure. */
static unsigned long *entry_values(tr_t *t, const unsigned char *e)
{
    int be = t->be;
    int type = (int)rd16(e + 2, be);
    unsigned long cnt = rd32(e + 4, be);
    size_t tsz;
    switch (type) {
    case 1: case 2: case 6: case 7: tsz = 1; break;   /* BYTE/ASCII/unknown, one byte each */
    case 3: case 8:                 tsz = 2; break;   /* SHORT */
    case 4: case 9: case 11:        tsz = 4; break;   /* LONG */
    default: return NULL;                              /* RATIONAL(5) and others are unused */
    }
    if (cnt == 0 || cnt > H3FR_MAX_ENTRIES * 4) return NULL;
    unsigned char raw[4];
    size_t bytes = tsz * (size_t)cnt;
    unsigned char *heap = NULL;
    const unsigned char *vals;
    if (bytes <= 4) {                   /* Value is inline in entry bytes 8..11 */
        memcpy(raw, e + 8, 4);
        vals = raw;
    } else {                            /* Value is at the offset recorded in the entry */
        long voff = (long)rd32(e + 8, be);
        heap = (unsigned char *)malloc(bytes);
        if (!heap) return NULL;
        if (tr_read(t, voff, heap, bytes) != 0) { free(heap); return NULL; }
        vals = heap;
    }
    unsigned long *out = (unsigned long *)malloc(sizeof(unsigned long) * (size_t)cnt);
    if (!out) { free(heap); return NULL; }
    for (unsigned long i = 0; i < cnt; i++) {
        if (tsz == 1)      out[i] = vals[i];
        else if (tsz == 2) out[i] = rd16(vals + (size_t)i * 2, be);
        else               out[i] = rd32(vals + (size_t)i * 4, be);
    }
    free(heap);
    return out;
}

/* Check whether the first two segment bytes are JPEG SOI (FFD8) */
static int seg_is_soi(tr_t *t, long off)
{
    unsigned char b[2];
    if (tr_read(t, off, b, 2) != 0) return 0;
    return b[0] == 0xFF && b[1] == 0xD8;
}

/* Collect candidates (strips / JPEGInterchangeFormat) from one IFD and select the best.
 * Returns 0 on success or -1 for corrupt structure (out of range/truncated). */
static int scan_ifd(tr_t *t, long ifd_off, int depth, cand_t *best)
{
    if (depth > H3FR_MAX_DEPTH || t->nvisited >= H3FR_MAX_IFDS)
        return 0;
    for (int i = 0; i < t->nvisited; i++)
        if (t->visited[i] == ifd_off) return 0;   /* Prevent loops */
    t->visited[t->nvisited++] = ifd_off;

    unsigned char hdr[2];
    if (tr_read(t, ifd_off, hdr, 2) != 0) return -1;
    int n = (int)rd16(hdr, t->be);
    if (n < 0 || (unsigned)n > H3FR_MAX_ENTRIES) return -1;

    size_t bytes = 2u + (size_t)n * 12u + 4u;
    unsigned char *ent = (unsigned char *)malloc(bytes);
    if (!ent) return -1;
    if (tr_read(t, ifd_off, ent, bytes) != 0) { free(ent); return -1; }
    long next_ifd = (long)rd32(ent + 2 + (size_t)n * 12, t->be);

    /* Gather this IFD's key fields */
    unsigned long *soff = NULL, *scnt = NULL;
    unsigned long nsoff = 0, nscnt = 0;
    unsigned long jif_off = 0, jif_len = 0;
    unsigned long *subifds = NULL;
    unsigned long nsub = 0;

    for (int i = 0; i < n; i++) {
        const unsigned char *e = ent + 2 + (size_t)i * 12;
        unsigned tag = rd16(e, t->be);
        switch (tag) {
        case 0x0111:                                /* StripOffsets */
            soff = entry_values(t, e);
            nsoff = rd32(e + 4, t->be);
            break;
        case 0x0117:                                /* StripByteCounts */
            scnt = entry_values(t, e);
            nscnt = rd32(e + 4, t->be);
            break;
        case 0x0201: {                              /* JPEGInterchangeFormat */
            unsigned long *v = entry_values(t, e);
            if (v) { jif_off = v[0]; free(v); }
            break;
        }
        case 0x0202: {                              /* JPEGInterchangeFormatLength */
            unsigned long *v = entry_values(t, e);
            if (v) { jif_len = v[0]; free(v); }
            break;
        }
        case 0x014A:                                /* SubIFDs */
            subifds = entry_values(t, e);
            nsub = rd32(e + 4, t->be);
            break;
        default:
            break;
        }
    }
    free(ent);

    /* Candidate 1: single JPEGInterchangeFormat segment (standard TIFF embedding) */
    if (jif_off != 0 && jif_len >= 2
        && (long)jif_off + (long)jif_len <= t->fsize
        && seg_is_soi(t, (long)jif_off)) {
        cand_t c;
        memset(&c, 0, sizeof(c));
        c.off = (long *)malloc(sizeof(long));
        c.len = (size_t *)malloc(sizeof(size_t));
        if (c.off && c.len) {
            c.off[0] = (long)jif_off;
            c.len[0] = (size_t)jif_len;
            c.n = 1;
            c.total = (size_t)jif_len;
            cand_keep_better(&c, best);
        } else {
            cand_free(&c);
        }
    }

    /* Candidate 2: Strip group — accept as JPEG only if the first strip starts with SOI
       (PhotoData is skipped) */
    if (soff && scnt && nsoff == nscnt && nsoff > 0) {
        size_t total = 0;
        int in_bounds = 1;
        for (unsigned long i = 0; i < nsoff; i++) {
            if ((long)soff[i] < 0 || scnt[i] == 0
                || (long)soff[i] + (long)scnt[i] > t->fsize) {
                in_bounds = 0;
                break;
            }
            total += (size_t)scnt[i];
        }
        if (in_bounds && seg_is_soi(t, (long)soff[0])) {
            cand_t c;
            memset(&c, 0, sizeof(c));
            c.off = (long *)malloc(sizeof(long) * (size_t)nsoff);
            c.len = (size_t *)malloc(sizeof(size_t) * (size_t)nsoff);
            if (c.off && c.len) {
                c.n = (size_t)nsoff;
                for (unsigned long i = 0; i < nsoff; i++) {
                    c.off[i] = (long)soff[i];
                    c.len[i] = (size_t)scnt[i];
                }
                c.total = total;
                cand_keep_better(&c, best);
            } else {
                cand_free(&c);
            }
        }
    }
    free(soff);
    free(scnt);

    /* Recurse through SubIFD (Hasselblad MetadataIFDPreview indicates the preview is
       attached to a metadata IFD) */
    if (subifds) {
        for (unsigned long i = 0; i < nsub && i < H3FR_MAX_IFDS; i++)
            scan_ifd(t, (long)subifds[i], depth + 1, best);
        free(subifds);
    }

    /* NextIFD chain */
    if (next_ifd > 0 && next_ifd < t->fsize)
        scan_ifd(t, next_ifd, depth, best);
    return 0;
}

int h3fr_extract_preview(const char *path, unsigned char **out_jpeg,
                         size_t *out_len)
{
    if (!path || !out_jpeg || !out_len) return H3FR_E_IO;
    *out_jpeg = NULL;
    *out_len = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return H3FR_E_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return H3FR_E_IO; }
    long fsize = ftell(f);
    if (fsize < 8) { fclose(f); return H3FR_E_NOTIFF; }

    tr_t t;
    memset(&t, 0, sizeof(t));
    t.f = f;
    t.fsize = fsize;

    unsigned char head[8];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(head, 1, 8, f) != 8) {
        fclose(f);
        return H3FR_E_IO;
    }
    if (head[0] == 'I' && head[1] == 'I')      t.be = 0;
    else if (head[0] == 'M' && head[1] == 'M') t.be = 1;
    else { fclose(f); return H3FR_E_NOTIFF; }
    if (rd16(head + 2, t.be) != 42) { fclose(f); return H3FR_E_NOTIFF; }
    long ifd0 = (long)rd32(head + 4, t.be);

    cand_t best;
    memset(&best, 0, sizeof(best));
    if (scan_ifd(&t, ifd0, 0, &best) != 0) {
        cand_free(&best);
        fclose(f);
        return H3FR_E_TRUNC;
    }
    if (best.n == 0 || best.total == 0) {
        fclose(f);
        return H3FR_E_NOPREV;
    }

    unsigned char *buf = (unsigned char *)malloc(best.total);
    if (!buf) { cand_free(&best); fclose(f); return H3FR_E_MEM; }
    size_t pos = 0;
    for (size_t i = 0; i < best.n; i++) {
        if (fseek(f, best.off[i], SEEK_SET) != 0
            || fread(buf + pos, 1, best.len[i], f) != best.len[i]) {
            free(buf);
            cand_free(&best);
            fclose(f);
            return H3FR_E_TRUNC;
        }
        pos += best.len[i];
    }
    cand_free(&best);
    fclose(f);

    if (pos < 4 || buf[0] != 0xFF || buf[1] != 0xD8) {  /* Double-check */
        free(buf);
        return H3FR_E_NOPREV;
    }
    *out_jpeg = buf;
    *out_len = pos;
    return H3FR_OK;
}
