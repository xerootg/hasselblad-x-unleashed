/* backend.h — Backend seam: the real print core (phase 2, src/net/) and mock backend
 * implement the same function table.
 * Conventions:
 *  - Return value: 0=accepted/success, negative=error code (see PRN_BE_*).
 *  - The backend updates properties through state.c's prn_set_ and prn_discovered_
 *    interfaces; the notify chain broadcasts propertiesChanged automatically. The backend
 *    does not access D-Bus.
 *  - tick() is called on every main-loop iteration (the mock uses it to advance simulated
 *    time; it may be NULL for the real backend).
 *  - The caller frees string arrays/strings returned by photo_list/start_print_job.
 */
#ifndef PRN_BACKEND_H
#define PRN_BACKEND_H

/* Error codes mapped by the adaptor to com.hasselblad.error.* */
#define PRN_BE_FAILED  (-1)   /* com.hasselblad.error.failed */
#define PRN_BE_PARAM   (-2)   /* com.hasselblad.error.parametererror */
#define PRN_BE_BUSY    (-3)   /* com.hasselblad.error.busy */

/* Start_print_job options (v2: keys are IPP keywords, values are native printer values;
 * 0/NULL uses the current service property). The adaptor parses a{sv} opts; the real
 * backend validates against capabilities before forwarding. Strings are borrowed pointers
 * (the adaptor guarantees their validity for the duration of the call).
 * §18 edit parameters (INTERFACE.md v2.1 §3b, crop/rotation; also used by Preview_print):
 *   crop_* are normalized 0..1 and defined on the **rotated** image (GUI preview coordinates);
 *   default 0,0,1,1 means full frame.
 *   rotation -1=EXIF auto (default), 0/90/180/270 explicitly override (do not stack with EXIF). */
typedef struct prn_job_opts {
    int         copies;          /* ≤0 = use copies property */
    const char *media;           /* NULL = use media property */
    const char *print_quality;   /* NULL = use print_quality property; draft|normal|high */
    const char *color_mode;      /* NULL = use color_mode property */
    const char *print_scaling;   /* NULL/"" = do not send (use printer default) */
    double      crop_x, crop_y, crop_w, crop_h;   /* Default 0,0,1,1 (full frame) */
    int         rotation;                           /* Default -1 (EXIF auto) */
} prn_job_opts_t;

/* Return value for doGet_photo_info (from imgprep_probe; does not decode pixels) */
typedef struct prn_photo_info {
    int  width, height;          /* Stored dimensions (3FR = embedded preview dimensions) */
    int  orientation;            /* EXIF Orientation 1..8; 0 = none */
    int  rotated_width, rotated_height; /* Logical dimensions after EXIF */
    int  has_jpeg;               /* 1 = file itself is JPEG */
    int  is_3fr;                 /* 1 = Hasselblad RAW (preview/print uses embedded preview) */
    long file_size;              /* File size in bytes (3FR = RAW file itself) */
} prn_photo_info_t;

typedef struct prn_backend {
    const char *name;                                        /* "mock" / "real" */
    void (*tick)(void);                                      /* May be NULL */
    void (*boot_init)(void);                                 /* Optional one-time startup hook once bus is ready
                                                                (§19 crash recovery + P1② residue cleanup) */
    int  (*connect_to_ap)(const char *ssid, const char *psk);/* Returns 0 on acceptance; state machine runs asynchronously */
    /* §20 Wi-Fi network scan (INTERFACE.md v2.2):
     * Only available in disconnected state (joining/joined/ready/discovering/scanning → PRN_BE_BUSY).
     * Elements are "SSID\tRSSI(dBm)\tSEC" (SEC=wpa2|open); skip hidden networks, deduplicate
     * SSIDs by strongest signal, sort by descending RSSI, max 32 items. Caller frees; results
     * are also published to scanned_networks (notify).
     * Returns: 0=completed synchronously (*out and *n are set); **1=accepted asynchronously**
     * (real backend: execution advances in tick; adaptor holds the method reply until complete
     * and prn_adaptor_scan_complete sends it; wire contract unchanged); negative=PRN_BE_*. */
    int  (*scan_wifi)(char ***out, int *n);
    int  (*disconnect_restore)(void);
    /* Discovery (v2.2 P0 semantics): **return on acceptance** — return 0 with *out=NULL,
     * *n=0 (empty as). L1→L4 cascade advances in the tick state machine (one endpoint/window
     * per subprocess + 60s total budget); publish each result via discovered_printers notify;
     * completion is when connection_state leaves discovering. */
    int  (*start_discovery)(char ***out, int *n);            /* Returns on acceptance; elements "name\tIP" */
    int  (*select_printer)(const char *name);                /* Match a discovered element or partial name;
                                                                real backend probes capabilities, negotiates format, publishes attributes */
    int  (*photo_list)(const char *path, char ***out, int *n);/* jpg/jpeg, max 64 items */
    int  (*start_print_job)(char **paths, int npaths,
                            const prn_job_opts_t *opts,
                            char **out_job_id);              /* 0=ok (*out_job_id is malloc'd), negative=PRN_BE_* */
    int  (*cancel_job)(void);                                /* 1=cancelled, 0=no active job */
    /* ---- §18 Preview/crop/rotation (GUI editor) ---- */
    int  (*photo_info)(const char *path, prn_photo_info_t *out);        /* Header probe */
    int  (*photo_preview)(const char *path, int max_w, int max_h,
                          char **out_path);                 /* Quick preview (cached); *out_path is malloc'd */
    int  (*preview_print)(const char *path, const prn_job_opts_t *opts,
                          char **out_path);                 /* WYSIWYG: same pipeline as print; *out_path is malloc'd */
} prn_backend;

/* Human-readable details for the most recent PRN_BE_* backend error (empty = none):
 * strongly defined by backend_real, weakly defined by mock_backend as a fallback; included
 * in the adaptor's error reply body. */
const char *prn_be_last_error(void)
#if defined(__GNUC__)
__attribute__((weak))
#endif
;

/* Mock backend (always linked into the service) */
const prn_backend *prn_backend_mock(void);

/* Real backend (provided by phase 2 src/net/; weak symbol, NULL if unimplemented → fall back to mock) */
const prn_backend *prn_backend_real(void)
#if defined(__GNUC__)
    __attribute__((weak))
#endif
;

#endif
