/* state.h — Service state machine and property storage (pure data layer, no D-Bus)
 * The property table/ranges match dbus/INTERFACE.md §2 (v2).
 * v2 generalization: removed fixed enums such as paper_size/media_type; media/print_quality/
 * color_mode are now native printer IPP keyword strings. Capability properties (§2b) are
 * published by prn_caps_publish after discovery in select_printer; Set validation uses
 * those capabilities.
 * All property modifications must go through prn_set_*: after allowlist and capability
 * validation, the notify callback is invoked (installed in main.c → adaptor emits propertiesChanged).
 */
#ifndef PRN_STATE_H
#define PRN_STATE_H

#include "ipp.h"                    /* ipp_caps_t (prn_caps_publish argument; type declaration only, no link dependency) */

#define PRN_MAX_PRINTERS 16   /* discovered_printers capacity */
#define PRN_MAX_SCAN     32   /* scanned_networks capacity (INTERFACE.md v2.2 §3 Scan_wifi: max 32 items) */
#define PRN_SLEN         96   /* String property limit (including \0) */
#define PRN_MAX_LIST     48   /* Capacity of capability-list properties (supported_* and media_size_info/scan list) */
#define PRN_LIST_LEN     96   /* Maximum capability-list element length (supports long keyword=WxHmm values) */

/* Property index = GetAll output order.
 * The final two properties, synced/tx_active, are for odindb-send-printer compatibility
 * (INTERFACE.md §7b): fixed properties from the patched slot (usbhost renamed to printer
 * with the same length), read-only + notify. */
enum prn_prop {
    PRN_CONNECTION_STATE = 0,
    PRN_SSID,
    PRN_PRINTER_STATE,
    PRN_PRINTER_NAME,
    PRN_PRINTER_IP,
    PRN_JOB_PROGRESS,
    PRN_JOB_STATUS,
    PRN_COPIES,            /* i writable: 1..copies_max (99 before capabilities are ready) */
    PRN_MEDIA,             /* s writable: native printer media keyword (e.g. na_index-4x6_4x6in) */
    PRN_PRINT_QUALITY,     /* s writable: draft|normal|high (RFC 8011 print-quality keywords) */
    PRN_COLOR_MODE,        /* s writable: native printer print-color-mode keyword */
    PRN_PRESET,            /* i writable: application preset (independent of printer capabilities) */
    PRN_DOCUMENT_FORMAT,   /* s read-only: actual document-format (image/jpeg after selection) */
    PRN_CAPS_READY,        /* b read-only: capability probe completed (true after Select_printer succeeds) */
    PRN_COPIES_MAX,        /* i read-only: copies-supported limit (99 before probe) */
    PRN_SUPPORTED_MEDIA,   /* as read-only: native media keyword list */
    PRN_SUPPORTED_QUALITIES,   /* as read-only: supported subset of draft/normal/high */
    PRN_SUPPORTED_COLOR_MODES, /* as read-only */
    PRN_SUPPORTED_FORMATS,     /* as read-only: document-format-supported */
    PRN_MEDIA_SIZE_INFO,   /* as read-only: each item "keyword=WxHmm" (human and machine readable) */
    PRN_DISCOVERED_PRINTERS,
    PRN_SCANNED_NETWORKS,  /* §20 network scan: read-only as with notify, elements "SSID\tRSSI(dBm)\tSEC" */
    PRN_SYNCED,            /* odindb compatibility: read-only b, initially true */
    PRN_TX_ACTIVE,         /* odindb compatibility: read-only b, initially false; true while tx() runs */
    PRN_PROP_COUNT
};

/* Property metadata: type 's'=string, 'i'=i32, 'b'=bool, 'A'=as array */
typedef struct prn_prop_info {
    const char *name;
    char        type;
    int         writable;
    const char *desc;
} prn_prop_info;

const prn_prop_info *prn_prop_info_get(int idx); /* Returns NULL if idx is out of range */
int prn_prop_index(const char *name);             /* Returns -1 if unknown */

/* Change-notification callback (idx=changed property) */
typedef void (*prn_notify_fn)(int idx, void *user);
void prn_state_set_notify(prn_notify_fn fn, void *user);
void prn_prop_changed(int idx); /* Manually notify after directly changing a field (used by mock backend) */

/* String list (stores capability properties; fixed-size 2D array avoids linked allocations) */
typedef struct {
    char items[PRN_MAX_LIST][PRN_LIST_LEN];
    int  n;
} prn_strlist_t;

typedef struct prn_state {
    char connection_state[PRN_SLEN]; /* disconnected|joining|joined|discovering|ready */
    char ssid[PRN_SLEN];
    char printer_state[PRN_SLEN];    /* idle|busy|error */
    char printer_name[PRN_SLEN];
    char printer_ip[PRN_SLEN];
    int  job_progress;               /* 0..100 */
    char job_status[PRN_SLEN];       /* idle|preparing|sending|printing|done|failed|canceled */
    int  copies;                     /* 1..copies_max */
    char media[PRN_SLEN];            /* Native media keyword (printer default after discovery) */
    char print_quality[PRN_SLEN];    /* draft|normal|high */
    char color_mode[PRN_SLEN];       /* Native print-color-mode keyword */
    int  preset;                     /* 0 default, 1 ID photo, 2 instant photo (application layer) */
    char document_format[PRN_SLEN];  /* Actual document-format sent */
    int  caps_ready;                 /* Capability probe complete */
    int  copies_max;                 /* 99 before discovery */
    prn_strlist_t sup_media;         /* supported_media */
    prn_strlist_t sup_qualities;
    prn_strlist_t sup_color_modes;
    prn_strlist_t sup_formats;
    prn_strlist_t media_size_info;   /* "keyword=WxHmm" */
    char *discovered[PRN_MAX_PRINTERS]; /* Elements "name\tIP" */
    int  discovered_n;
    prn_strlist_t scanned;          /* §20: most recent Scan_wifi results (≤PRN_MAX_SCAN) */
    int  synced;                    /* odindb compatibility (§7b): 1=true */
    int  tx_active;                 /* odindb compatibility (§7b): 1 while tx() runs */
} prn_state;

prn_state *prn_state_get(void);     /* Singleton, initialized to defaults */

/* Set entry point (validation + notify): 0=success, -1=unknown/read-only, -2=invalid value */
int prn_set_string(int idx, const char *val);
int prn_set_int(int idx, int val);
/* Get convenience functions */
const char *prn_get_string(int idx);
int         prn_get_int(int idx);
const prn_strlist_t *prn_get_list(int idx); /* Non-'A' property returns an empty list (never NULL) */

/* ---- Publish capabilities (called after successful backend_real Select_printer probe;
 * notify each property) ----
 * Populates capability lists/limits + normalizes printer defaults
 * (media/print_quality/color_mode) + caps_ready=1 + document_format;
 * clamp copies to the maximum if it exceeds the limit. */
void prn_caps_publish(const ipp_caps_t *caps, const char *document_format);
/* Invalidate capabilities (disconnect/reselection failure): caps_ready=0, clear capability lists,
 * reset copies_max to 99 */
void prn_caps_invalidate(void);

/* List membership check (for caps_ready validation/Set forwarding): 1=found */
int prn_list_contains(const prn_strlist_t *l, const char *v);

/* discovered_printers array operations (automatic notify) */
void prn_discovered_clear(void);
int  prn_discovered_add(const char *name_ip); /* -1=full */

/* scanned_networks list operations (§20, automatic notify; elements "SSID\tRSSI(dBm)\tSEC") */
void prn_scanned_clear(void);
int  prn_scanned_add(const char *ssid_rssi_sec); /* -1=full/too long */

/* State-machine convenience transitions (with notify); values are not validated,
 * internal service use only */
void prn_set_connection_state(const char *v);
void prn_set_printer_state(const char *v);
void prn_set_job_status(const char *v);
void prn_set_printer_name(const char *v);
void prn_set_printer_ip(const char *v);
void prn_set_job_progress(int v);

/* Convenience setters for odindb compatibility properties (§7b) (with notify; normalize
 * values to 0/1) */
void prn_set_synced(int v);
void prn_set_tx_active(int v);

/* Whether a job is in progress (preparing/sending/printing) */
int prn_job_active(void);
/* connection_state convenience checks */
int prn_state_is(const char *connection_state);
int prn_job_status_is(const char *job_status);

#endif
