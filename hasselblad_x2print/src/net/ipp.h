/* ipp.h — Minimal IPP client + HTTP/1.1 wrapper (RFC 8010/8011, hand-written codec,
 * no libcups dependency)
 *
 * General-purpose IPP printer client (all capabilities are discovered; no hard-coded
 * printer attributes):
 *   Default endpoint ipp://<ip>:631/ipp/print (path can be overridden by mDNS TXT rp= or
 *   ipp_set_path); plain HTTP, no authentication.
 *
 * Four core operations:
 *   ipp_get_printer_attributes → capabilities (media + physical dimensions/format/color mode/quality/copies/resolution/scaling)
 *   ipp_print_jpeg             → send JPEG with Print-Job; return job_id
 *   ipp_get_job_attributes     → poll job status
 *   ipp_cancel_job             → cancel remote job with Cancel-Job (0x0008)
 */
#ifndef X2P_IPP_H
#define X2P_IPP_H

#include <stddef.h>
#include <string.h> /* Used by inline conversion helpers in this header */

#ifdef __cplusplus
extern "C" {
#endif

#define IPP_DEF_PORT 631
#define IPP_DEF_PATH "/ipp/print"
#define IPP_MAX_FORMATS 24
#define IPP_MAX_MEDIA   48
#define IPP_MAX_MODES   16
#define IPP_MAX_QUALS   8
#define IPP_MAX_SCAL    8

/* ---- Error codes (all APIs return 0=success; see this enum for other values) ---- */
enum {
    IPP_OK          = 0,
    IPP_E_ARG       = -100, /* Invalid argument */
    IPP_E_SOCKET    = -101, /* Socket creation failed */
    IPP_E_CONNECT   = -102, /* Connection failed/timed out */
    IPP_E_SEND      = -103, /* Send failed */
    IPP_E_TIMEOUT   = -104, /* Response timed out */
    IPP_E_TRUNC     = -105, /* Incomplete response */
    IPP_E_HTTP      = -106, /* HTTP status not 200 / malformed response */
    IPP_E_PARSE     = -107, /* Failed to parse IPP message */
    IPP_E_STATUS    = -108, /* IPP status not ok (see ipp_last_status) */
    IPP_E_NOMEM     = -109, /* Out of memory */
};
const char *ipp_strerror(int err);
unsigned int ipp_last_status(void); /* IPP status-code when IPP_E_STATUS occurs (RFC 8011 §4.1.8/§4.2) */
const char *ipp_last_detail(void);  /* Human-readable error detail (HTTP status line, malformed position, etc.) */

/* ---- Printer capabilities (v2: generalized, all discovered) ---- */
typedef struct {
    char document_formats[IPP_MAX_FORMATS][48]; /* document-format-supported */
    int  n_formats;
    char media_supported[IPP_MAX_MEDIA][64];    /* Extracted from media-supported + media-col collection */
    int  n_media;
    int  media_native[IPP_MAX_MEDIA];           /* 1 = native printer keyword (can be sent as media) */
    int  media_x_um[IPP_MAX_MEDIA];             /* Physical width µm (media-col x-dimension×10; 0=unknown) */
    int  media_y_um[IPP_MAX_MEDIA];             /* Physical height µm (same) */
    char print_color_modes[IPP_MAX_MODES][48];  /* print-color-mode-supported */
    int  n_modes;
    int  qualities[IPP_MAX_QUALS];              /* print-quality-supported: 3=draft/4=normal/5=high */
    int  n_quals;
    int  quality_default;                       /* print-quality-default enum (0=not advertised) */
    char color_mode_default[48];                /* print-color-mode-default (empty=not advertised) */
    char print_scalings[IPP_MAX_SCAL][32];      /* print-scaling-supported */
    int  n_scalings;
    char print_scaling_default[32];             /* print-scaling-default */
    int  copies_min, copies_max;                /* copies-supported (rangeOfInteger; default 1,1=not advertised) */
    int  res_default_x, res_default_y;          /* printer-resolution-default dpi (0=not advertised) */
    int  jpeg_max_w, jpeg_max_h;                /* Derived maximum pixel bounds from media × default resolution (set by finish) */
    int  printer_state;                         /* 3=idle 4=processing 5=stopped */
    char printer_state_reasons[128];
    char printer_name[96];
    char printer_make_and_model[128];
    char media_default[64];
    /* Internal state (for extracting media-col collections); do not modify */
    int _lx, _ly, _szname;
} ipp_caps_t;

/* ---- Print-job status ---- */
typedef struct {
    int  job_id;
    int  job_state;               /* 3 pending / 5 processing / 7 canceled / 8 aborted / 9 completed */
    char state_reasons[128];      /* job-state-reasons (space-separated values) */
} ipp_job_t;

/* ---- Print-Job options (0/NULL means do not send the attribute; use printer default) ---- */
typedef struct {
    int         copies;           /* Default 1 */
    const char *color_mode;       /* Native print-color-mode keyword, e.g. "color"/"monochrome"/"auto" */
    const char *paper;            /* Native media keyword (from capability discovery) */
    int         quality;          /* print-quality enum: 3 draft / 4 normal / 5 high */
    const char *print_scaling;    /* Native print-scaling keyword (e.g. "fit"/"auto") */
    const char *job_name;         /* Default "x2print" */
    const char *document_format;  /* Default "image/jpeg" */
} ipp_print_opts_t;

/* ---- Public API (ip is dotted-decimal IPv4; port 0 = 631) ---- */
int ipp_get_printer_attributes(const char *ip, int port, ipp_caps_t *caps);
int ipp_print_jpeg(const char *ip, int port, const unsigned char *jpeg, size_t len,
                   const ipp_print_opts_t *opts, int *job_id);
int ipp_get_job_attributes(const char *ip, int port, int job_id, ipp_job_t *job);

/* ---- Cancel-Job(RFC 8011 operation 0x0008) ----
 * The target is specified by the integer job-id attribute (same as Get-Job-Attributes;
 * printer-uri is still required). Returns IPP_OK=0x0000 successful-ok; nonexistent or
 * terminal jobs and other 4xx/5xx responses return IPP_E_STATUS (see ipp_last_status for
 * the code and ipp_last_detail for details). */
int ipp_cancel_job(const char *ip, int port, int job_id);

/* ---- Endpoint path override (for mDNS TXT rp= / printers not using /ipp/print) ----
 * Pass NULL for the default IPP_DEF_PATH; affects HTTP POST path and printer-uri. */
void ipp_set_path(const char *path);
const char *ipp_get_path(void);            /* Current effective path (static string) */

/* ---- Timeout parameters (callers may tighten them per scenario; P0 fails fast on dead endpoints) ----
 * Affect all subsequent transactions: connect_ms=TCP connection timeout,
 * recv_ms=SO_RCVTIMEO (per read).
 * Defaults: connect=3000/recv=20000(ms); 0/negative restores defaults.
 * Service usage (NOTES §22): tighter settings for probing/polling (3000,5000), fast cancel
 * loop (2000,2000), default timeout for the Print-Job data path (20s receive timeout; large
 * jobs can take time to generate a response). */
void ipp_set_timeouts(int connect_ms, int recv_ms);

/* ---- Capability derivation/conversion helpers ---- */
/* Finalize after probing: derive jpeg_max_w/h from media dimensions × default resolution.
 * ipp_get_printer_attributes calls this automatically; callers feeding raw callbacks
 * themselves must call it manually. */
void ipp_caps_finish(ipp_caps_t *caps);
/* Calculate target pixels from a native media keyword: dimensions (µm) ×
 * printer-resolution-default (fallback to 300dpi if unavailable).
 * Returns 0=ok; -1=keyword not in capability list; -2=no size info for this media. */
int  ipp_media_target_px(const ipp_caps_t *caps, const char *media, int *w, int *h);

/* print-quality enum ↔ RFC 8011 keyword name (inline in header, no link dependency) */
static inline const char *ipp_quality_kw(int q)
{
    return q == 3 ? "draft" : q == 4 ? "normal" : q == 5 ? "high" : "";
}
static inline int ipp_quality_enum(const char *kw)
{
    if (!kw) return 0;
    if (!strcmp(kw, "draft"))  return 3;
    if (!strcmp(kw, "normal")) return 4;
    if (!strcmp(kw, "high"))   return 5;
    return 0;
}

/* ---- Generic attribute callback (used for full --probe output) ----
 * Called once per attribute value. IPP 1.1 collection (beginCollection 0x34) members are
 * flattened to separate attribute names (the memberAttrName 0x4A value) for readability.
 * Parameters: group_tag 0x01/0x02/0x04; value_tag per RFC 8010 table 3; name is a valid
 * attribute name; val/vlen are the value bytes. */
typedef void (*ipp_attr_cb)(void *ud, int group_tag, int value_tag,
                            const char *name, const unsigned char *val, int vlen);

/* Fetch all printer attributes and invoke the callback for each value (probe tool);
 * returns IPP_OK or an error code. */
int ipp_get_printer_attributes_raw(const char *ip, int port, ipp_attr_cb cb, void *ud);

/* Capability extractor: feed a single attribute value into ipp_caps_t (with a raw callback,
 * callers can print and extract at the same time, obtaining both a full listing and
 * structured capabilities in one network round trip). */
void ipp_caps_feed(ipp_caps_t *caps, int group, int tag, const char *name,
                   const unsigned char *val, int vlen);

/* ---- Display helpers (used by selftest) ---- */
const char *ipp_tag_name(int tag);       /* Value tag → name */
const char *ipp_group_name(int g);       /* Group tag → name */
const char *ipp_printer_state_name(int s);
const char *ipp_job_state_name(int s);

#ifdef __cplusplus
}
#endif
#endif /* X2P_IPP_H */
