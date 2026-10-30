/* state.c — Property storage, validation, and change notifications (see state.h /
 * INTERFACE.md §2 v2).
 * v2: media/print_quality/color_mode use strings; prn_caps_publish fills capability
 * properties; after caps_ready, Set validation uses capability lists. */
#include "state.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static prn_state g_st;             /* Zero-initialized; init() fills defaults */
static prn_notify_fn g_notify;
static void *g_notify_user;

static const prn_strlist_t g_empty_list = { { { 0 } }, 0 };

/* Property metadata: order = enum = GetAll output order; matches INTERFACE.md §2 row by row */
static const prn_prop_info g_props[PRN_PROP_COUNT] = {
    [PRN_CONNECTION_STATE]       = { "connection_state",       's', 0, "disconnected|joining|joined|discovering|ready" },
    [PRN_SSID]                   = { "ssid",                   's', 1, "target AP ssid" },
    [PRN_PRINTER_STATE]          = { "printer_state",          's', 0, "idle|busy|error" },
    [PRN_PRINTER_NAME]           = { "printer_name",           's', 0, "selected printer" },
    [PRN_PRINTER_IP]             = { "printer_ip",             's', 0, "selected printer ip" },
    [PRN_JOB_PROGRESS]           = { "job_progress",           'i', 0, "0..100" },
    [PRN_JOB_STATUS]             = { "job_status",             's', 0, "idle|preparing|sending|printing|done|failed|canceled" },
    [PRN_COPIES]                 = { "copies",                 'i', 1, "1..copies_max(caps not ready=99)" },
    [PRN_MEDIA]                  = { "media",                  's', 1, "Native printer media keyword (from supported_media)" },
    [PRN_PRINT_QUALITY]          = { "print_quality",          's', 1, "draft|normal|high (from supported_qualities)" },
    [PRN_COLOR_MODE]             = { "color_mode",             's', 1, "Native print-color-mode keyword (from supported_color_modes)" },
    [PRN_PRESET]                 = { "preset",                 'i', 1, "0 default 1 ID 2 instant photo (application layer)" },
    [PRN_DOCUMENT_FORMAT]        = { "document_format",        's', 0, "Actual document-format sent (read-only)" },
    [PRN_CAPS_READY]             = { "caps_ready",             'b', 0, "Capability probe complete" },
    [PRN_COPIES_MAX]             = { "copies_max",             'i', 0, "copies-supported limit" },
    [PRN_SUPPORTED_MEDIA]        = { "supported_media",        'A', 0, "as: native media keywords" },
    [PRN_SUPPORTED_QUALITIES]    = { "supported_qualities",    'A', 0, "as: supported subset of draft/normal/high" },
    [PRN_SUPPORTED_COLOR_MODES]  = { "supported_color_modes",  'A', 0, "as: native color-mode keywords" },
    [PRN_SUPPORTED_FORMATS]      = { "supported_formats",      'A', 0, "as: document-format-supported" },
    [PRN_MEDIA_SIZE_INFO]        = { "media_size_info",        'A', 0, "as: each entry keyword=WxHmm" },
    [PRN_DISCOVERED_PRINTERS]    = { "discovered_printers",    'A', 0, "array of \"name\\tIP\"" },
    [PRN_SCANNED_NETWORKS]       = { "scanned_networks",       'A', 0, "as: \"SSID\\tRSSI(dBm)\\tSEC\"(wpa2|open), §20 Scan_wifi" },
    /* odindb-send-printer compatibility surface (INTERFACE.md §7b): fixed properties from
     * patched slot, read-only */
    [PRN_SYNCED]                 = { "synced",                 'b', 0, "odindb usbhost-slot compat" },
    [PRN_TX_ACTIVE]              = { "tx_active",              'b', 0, "true while tx() runs" },
};

const prn_prop_info *prn_prop_info_get(int idx)
{
    return (idx >= 0 && idx < PRN_PROP_COUNT) ? &g_props[idx] : NULL;
}

int prn_prop_index(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < PRN_PROP_COUNT; i++)
        if (!strcmp(g_props[i].name, name)) return i;
    return -1;
}

void prn_state_set_notify(prn_notify_fn fn, void *user)
{
    g_notify = fn;
    g_notify_user = user;
}

void prn_prop_changed(int idx)
{
    if (g_notify) g_notify(idx, g_notify_user);
}

prn_state *prn_state_get(void)
{
    static int inited = 0;
    if (!inited) {
        inited = 1;
        /* Defaults = initial values in INTERFACE.md §2 v2 */
        strcpy(g_st.connection_state, "disconnected");
        g_st.ssid[0] = 0;
        strcpy(g_st.printer_state, "idle");
        g_st.printer_name[0] = 0;
        g_st.printer_ip[0] = 0;
        g_st.job_progress = 0;
        strcpy(g_st.job_status, "idle");
        g_st.copies = 1;
        g_st.media[0] = 0;                    /* After probe = printer media-default */
        strcpy(g_st.print_quality, "normal"); /* After probe = printer quality-default */
        strcpy(g_st.color_mode, "auto");      /* After probe = printer color-default */
        g_st.preset = 0;
        g_st.document_format[0] = 0;
        g_st.caps_ready = 0;
        g_st.copies_max = 99;                 /* Permissive limit before probe */
        g_st.discovered_n = 0;
        g_st.scanned.n = 0;            /* §20: scan list initially empty */
        g_st.synced = 1;               /* §7b initial value: synced=true */
        g_st.tx_active = 0;            /* §7b initial value: tx_active=false */
    }
    return &g_st;
}

/* ---- Writable-property validation ---- */
static int int_range_ok(int idx, int v)
{
    prn_state *st = prn_state_get();
    switch (idx) {
    case PRN_COPIES:          return v >= 1 && v <= st->copies_max;
    case PRN_PRESET:          return v >= 0 && v <= 2;
    default:                  return 0;
    }
}

/* Semantic validation for writable string properties: after caps_ready, value must be in
 * capability list */
static int str_value_ok(int idx, const char *v)
{
    prn_state *st = prn_state_get();
    if (!st->caps_ready) return 1;             /* Not probed yet: no capabilities to validate against */
    switch (idx) {
    case PRN_MEDIA:         return prn_list_contains(&st->sup_media, v);
    case PRN_PRINT_QUALITY: return prn_list_contains(&st->sup_qualities, v);
    case PRN_COLOR_MODE:    return prn_list_contains(&st->sup_color_modes, v);
    default:                return 1;
    }
}

int prn_set_int(int idx, int val)
{
    const prn_prop_info *pi = prn_prop_info_get(idx);
    if (!pi) { LOGW("set: unknown property #%d", idx); return -1; }
    if (!pi->writable || pi->type != 'i') {
        LOGW("set: property '%s' not writable int", pi->name);
        return -1;
    }
    if (!int_range_ok(idx, val)) {
        LOGW("set: property '%s' value %d out of range", pi->name, val);
        return -2;
    }
    int *dst = NULL;
    prn_state *st = prn_state_get();
    switch (idx) {
    case PRN_COPIES: dst = &st->copies; break;
    case PRN_PRESET: dst = &st->preset; break;
    default: return -1;
    }
    if (*dst != val) { *dst = val; prn_prop_changed(idx); }
    return 0;
}

int prn_set_string(int idx, const char *val)
{
    const prn_prop_info *pi = prn_prop_info_get(idx);
    if (!pi || !val) { LOGW("set: bad string set #%d", idx); return -1; }
    if (!pi->writable || pi->type != 's') {
        LOGW("set: property '%s' not writable string", pi->name);
        return -1;
    }
    if (!*val && idx != PRN_SSID && idx != PRN_MEDIA) {
        LOGW("set: property '%s' empty value", pi->name); /* Empty media is allowed (no printer selected) */
        return -2;
    }
    if (strlen(val) >= PRN_SLEN) {
        LOGW("set: property '%s' value too long", pi->name);
        return -2;
    }
    if (!str_value_ok(idx, val)) {
        LOGW("set: property '%s' value '%s' not in supported list", pi->name, val);
        return -2;
    }
    prn_state *st = prn_state_get();
    char *dst = NULL;
    switch (idx) {
    case PRN_SSID:          dst = st->ssid;          break;
    case PRN_MEDIA:         dst = st->media;         break;
    case PRN_PRINT_QUALITY: dst = st->print_quality; break;
    case PRN_COLOR_MODE:    dst = st->color_mode;    break;
    default: return -1;
    }
    if (strcmp(dst, val)) {
        strcpy(dst, val);
        prn_prop_changed(idx);
    }
    return 0;
}

/* Get string value: returns static array (never NULL; safe for marshalling); unknown idx returns "" */
const char *prn_get_string(int idx)
{
    prn_state *st = prn_state_get();
    switch (idx) {
    case PRN_CONNECTION_STATE: return st->connection_state;
    case PRN_SSID:             return st->ssid;
    case PRN_PRINTER_STATE:    return st->printer_state;
    case PRN_PRINTER_NAME:     return st->printer_name;
    case PRN_PRINTER_IP:       return st->printer_ip;
    case PRN_JOB_STATUS:       return st->job_status;
    case PRN_MEDIA:            return st->media;
    case PRN_PRINT_QUALITY:    return st->print_quality;
    case PRN_COLOR_MODE:       return st->color_mode;
    case PRN_DOCUMENT_FORMAT:  return st->document_format;
    default:                   return "";
    }
}int prn_get_int(int idx)
{
    prn_state *st = prn_state_get();
    switch (idx) {
    case PRN_JOB_PROGRESS:    return st->job_progress;
    case PRN_COPIES:          return st->copies;
    case PRN_PRESET:          return st->preset;
    case PRN_CAPS_READY:      return st->caps_ready;
    case PRN_COPIES_MAX:      return st->copies_max;
    case PRN_SYNCED:          return st->synced;
    case PRN_TX_ACTIVE:       return st->tx_active;
    default:                  return 0;
    }
}

const prn_strlist_t *prn_get_list(int idx)
{
    prn_state *st = prn_state_get();
    switch (idx) {
    case PRN_SUPPORTED_MEDIA:       return &st->sup_media;
    case PRN_SUPPORTED_QUALITIES:   return &st->sup_qualities;
    case PRN_SUPPORTED_COLOR_MODES: return &st->sup_color_modes;
    case PRN_SUPPORTED_FORMATS:     return &st->sup_formats;
    case PRN_MEDIA_SIZE_INFO:       return &st->media_size_info;
    case PRN_SCANNED_NETWORKS:      return &st->scanned;
    default:                        return &g_empty_list;
    }
}

int prn_list_contains(const prn_strlist_t *l, const char *v)
{
    if (!l || !v) return 0;
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->items[i], v)) return 1;
    return 0;
}

/* ---- Capability publication/invalidation ---- */

/* Fill list (silently overwrite; caller handles notify) */
static void list_fill(prn_strlist_t *l, const char *const *vals, int n)
{
    l->n = 0;
    for (int i = 0; i < n && i < PRN_MAX_LIST; i++) {
        if (!vals[i] || !vals[i][0]) continue;
        snprintf(l->items[l->n], PRN_LIST_LEN, "%.*s", PRN_LIST_LEN - 1, vals[i]);
        l->n++;
    }
}

static void caps_notify_lists(void)
{
    prn_prop_changed(PRN_SUPPORTED_MEDIA);
    prn_prop_changed(PRN_SUPPORTED_QUALITIES);
    prn_prop_changed(PRN_SUPPORTED_COLOR_MODES);
    prn_prop_changed(PRN_SUPPORTED_FORMATS);
    prn_prop_changed(PRN_MEDIA_SIZE_INFO);
    prn_prop_changed(PRN_COPIES_MAX);
}

void prn_caps_publish(const ipp_caps_t *caps, const char *document_format)
{
    prn_state *st = prn_state_get();
    if (!caps) return;

    /* 1) Capability lists */
    const char *medias[IPP_MAX_MEDIA];
    int nm = 0;
    char sizes[IPP_MAX_MEDIA][PRN_LIST_LEN];
    const char *size_ptrs[IPP_MAX_MEDIA];
    int ns = 0;
    for (int i = 0; i < caps->n_media && nm < IPP_MAX_MEDIA; i++) {
        if (!caps->media_native[i]) continue;      /* Synthesized display names cannot be sent; omit from supported */
        medias[nm++] = caps->media_supported[i];
        if (caps->media_x_um[i] > 0 && caps->media_y_um[i] > 0 && ns < IPP_MAX_MEDIA) {
            /* media_size_info: "keyword=WxHmm" (W/H in mm, readable by people and software) */
            snprintf(sizes[ns], PRN_LIST_LEN, "%s=%.1fx%.1fmm", caps->media_supported[i],
                     caps->media_x_um[i] / 1000.0, caps->media_y_um[i] / 1000.0);
            size_ptrs[ns] = sizes[ns];
            ns++;
        }
    }
    list_fill(&st->sup_media, medias, nm);
    list_fill(&st->media_size_info, size_ptrs, ns);

    const char *quals[IPP_MAX_QUALS];
    int nq = 0;
    for (int i = 0; i < caps->n_quals && nq < IPP_MAX_QUALS; i++) {
        const char *kw = ipp_quality_kw(caps->qualities[i]);
        if (kw[0]) quals[nq++] = kw;               /* Recognize only 3/4/5; skip other enum values */
    }
    list_fill(&st->sup_qualities, quals, nq);

    const char *modes[IPP_MAX_MODES];
    for (int i = 0; i < caps->n_modes && i < IPP_MAX_MODES; i++)
        modes[i] = caps->print_color_modes[i];
    list_fill(&st->sup_color_modes, modes, caps->n_modes);

    const char *fmts[IPP_MAX_FORMATS];
    for (int i = 0; i < caps->n_formats && i < IPP_MAX_FORMATS; i++)
        fmts[i] = caps->document_formats[i];
    list_fill(&st->sup_formats, fmts, caps->n_formats);

    st->copies_max = caps->copies_max > 0 ? caps->copies_max : 99;
    caps_notify_lists();

    /* 2) Normalize printer defaults (set ready first, then validate defaults) */
    st->caps_ready = 1;
    prn_prop_changed(PRN_CAPS_READY);

    if (caps->media_default[0] && prn_list_contains(&st->sup_media, caps->media_default))
        prn_set_string(PRN_MEDIA, caps->media_default);
    else if (st->sup_media.n > 0)
        prn_set_string(PRN_MEDIA, st->sup_media.items[0]);
    else
        prn_set_string(PRN_MEDIA, "");

    const char *qdef = ipp_quality_kw(caps->quality_default);
    if (!qdef[0]) qdef = "normal";
    if (st->sup_qualities.n == 0 || prn_list_contains(&st->sup_qualities, qdef))
        prn_set_string(PRN_PRINT_QUALITY, qdef);
    else
        prn_set_string(PRN_PRINT_QUALITY, st->sup_qualities.items[0]);

    if (caps->color_mode_default[0] &&
        prn_list_contains(&st->sup_color_modes, caps->color_mode_default))
        prn_set_string(PRN_COLOR_MODE, caps->color_mode_default);
    else if (st->sup_color_modes.n > 0)
        prn_set_string(PRN_COLOR_MODE, st->sup_color_modes.items[0]);

    if (document_format && document_format[0]) {
        if (strcmp(st->document_format, document_format)) {
            snprintf(st->document_format, sizeof st->document_format, "%s", document_format);
            prn_prop_changed(PRN_DOCUMENT_FORMAT);
        }
    }

    /* 3) Clamp copies to limit (out-of-range value is no longer valid) */
    if (st->copies > st->copies_max) {
        st->copies = st->copies_max;
        prn_prop_changed(PRN_COPIES);
    }
    LOGI("caps: published media=%d size_info=%d qualities=%d modes=%d formats=%d "
         "copies_max=%d res=%ddpi docfmt=%s",
         st->sup_media.n, st->media_size_info.n, st->sup_qualities.n,
         st->sup_color_modes.n, st->sup_formats.n, st->copies_max,
         caps->res_default_x, st->document_format);
}

void prn_caps_invalidate(void)
{
    prn_state *st = prn_state_get();
    if (!st->caps_ready && st->sup_media.n == 0 && st->copies_max == 99) return;
    st->caps_ready = 0;
    st->copies_max = 99;
    st->sup_media.n = 0;
    st->sup_qualities.n = 0;
    st->sup_color_modes.n = 0;
    st->sup_formats.n = 0;
    st->media_size_info.n = 0;
    if (st->document_format[0]) { st->document_format[0] = 0; prn_prop_changed(PRN_DOCUMENT_FORMAT); }
    if (st->copies > st->copies_max) { st->copies = st->copies_max; prn_prop_changed(PRN_COPIES); }
    prn_prop_changed(PRN_CAPS_READY);
    prn_prop_changed(PRN_COPIES_MAX);
    caps_notify_lists();
    LOGI("caps: invalidated");
}

/* ---- discovered_printers ---- */
void prn_discovered_clear(void)
{
    prn_state *st = prn_state_get();
    int had = st->discovered_n > 0;
    for (int i = 0; i < st->discovered_n; i++) free(st->discovered[i]);
    st->discovered_n = 0;
    if (had) prn_prop_changed(PRN_DISCOVERED_PRINTERS);
}

int prn_discovered_add(const char *name_ip)
{
    prn_state *st = prn_state_get();
    if (st->discovered_n >= PRN_MAX_PRINTERS) { LOGW("discovered list full"); return -1; }
    char *dup = strdup(name_ip);
    if (!dup) { LOGE("oom discovered_add"); return -1; }
    st->discovered[st->discovered_n++] = dup;
    prn_prop_changed(PRN_DISCOVERED_PRINTERS);
    return 0;
}

/* ---- scanned_networks (§20 scan selection) ---- */
void prn_scanned_clear(void)
{
    prn_state *st = prn_state_get();
    int had = st->scanned.n > 0;
    st->scanned.n = 0;
    if (had) prn_prop_changed(PRN_SCANNED_NETWORKS);
}

int prn_scanned_add(const char *ssid_rssi_sec)
{
    prn_state *st = prn_state_get();
    if (!ssid_rssi_sec || !ssid_rssi_sec[0]) return -1;
    if (st->scanned.n >= PRN_MAX_SCAN || st->scanned.n >= PRN_MAX_LIST) {
        LOGW("scanned list full(%d)", st->scanned.n);
        return -1;
    }
    if (strlen(ssid_rssi_sec) >= PRN_LIST_LEN) {
        LOGW("scanned item too long(%zu): %.32s...", strlen(ssid_rssi_sec), ssid_rssi_sec);
        return -1;
    }
    snprintf(st->scanned.items[st->scanned.n], PRN_LIST_LEN, "%s", ssid_rssi_sec);
    st->scanned.n++;
    prn_prop_changed(PRN_SCANNED_NETWORKS);
    return 0;
}

/* ---- State-machine convenience transitions (internal use; values are not validated) ---- */
static void set_s(char *dst, const char *v, int idx)
{
    if (strcmp(dst, v)) {
        strncpy(dst, v, PRN_SLEN - 1);
        dst[PRN_SLEN - 1] = 0;
        prn_prop_changed(idx);
    }
}
void prn_set_connection_state(const char *v) { set_s(prn_state_get()->connection_state, v, PRN_CONNECTION_STATE); }
void prn_set_printer_state(const char *v)    { set_s(prn_state_get()->printer_state,    v, PRN_PRINTER_STATE); }
void prn_set_job_status(const char *v)       { set_s(prn_state_get()->job_status,       v, PRN_JOB_STATUS); }
void prn_set_printer_name(const char *v)     { set_s(prn_state_get()->printer_name,     v, PRN_PRINTER_NAME); }
void prn_set_printer_ip(const char *v)       { set_s(prn_state_get()->printer_ip,       v, PRN_PRINTER_IP); }

void prn_set_job_progress(int v)
{
    prn_state *st = prn_state_get();
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (st->job_progress != v) { st->job_progress = v; prn_prop_changed(PRN_JOB_PROGRESS); }
}

/* Toggle odindb compatibility properties (§7b): normalize to 0/1; notify only on change */
void prn_set_synced(int v)
{
    prn_state *st = prn_state_get();
    v = v ? 1 : 0;
    if (st->synced != v) { st->synced = v; prn_prop_changed(PRN_SYNCED); }
}

void prn_set_tx_active(int v)
{
    prn_state *st = prn_state_get();
    v = v ? 1 : 0;
    if (st->tx_active != v) { st->tx_active = v; prn_prop_changed(PRN_TX_ACTIVE); }
}

int prn_job_active(void)
{
    return prn_job_status_is("preparing") || prn_job_status_is("sending") || prn_job_status_is("printing");
}
int prn_state_is(const char *v)      { return !strcmp(prn_state_get()->connection_state, v); }
int prn_job_status_is(const char *v) { return !strcmp(prn_state_get()->job_status, v); }
