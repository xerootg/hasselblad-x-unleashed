/* mock_backend.c — Mock backend: simulates hotspot connection delay/discovery list/print
 * progress (2%/sec).
 * Used for WSL and GUI integration; timeline (INTERFACE.md §4):
 *   joining 3s → joined | discovery takes 2s synchronously inside Start_discovery
 *   preparing 2s → sending(0→40, 2%/s) → printing(40→100, 2%/s) → done 2s → idle
 * Easter eggs for testing error paths: PSK 1–7 chars → synchronous failed (empty string =
 * open network, §20); SSID containing "fail" → async failure back to disconnected.
 * §18 preview/crop methods: mock has no image pipeline (libjpeg is not linked), so returns
 * failed explicitly; use --real backend for real image behavior (wifi_backend=mock is fine;
 * print path uses mock_ipp).
 */
#include "backend.h"
#include "state.h"
#include "log.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- Connection simulation ---- */
static int  g_joining;            /* 1=join in progress */
static int  g_join_fail;          /* Demonstrate async failure */
static long long g_join_t0;

/* ---- Print-job simulation ---- */
enum { MJ_NONE = 0, MJ_PREPARING, MJ_SENDING, MJ_PRINTING, MJ_DONE, MJ_CANCELED };
static int      g_mj = MJ_NONE;
static long long g_mj_t0;
static int      g_jobno;

/* ---- Discovery simulation (v2.2 semantics: return on acceptance, publish after 2s in
 * tick — same timing as real backend) ---- */
static int      g_disc_run;
static long long g_disc_t0;

/* String-array helpers (malloc each element; caller frees each one) */
static char **arr_new(int n) { return calloc((size_t)n + 1, sizeof(char *)); }
static void arr_put(char ***out, int *n, char **items, int cnt)
{
    *out = items; *n = cnt;
}

static int mb_connect_to_ap(const char *ssid, const char *psk)
{
    if (!ssid || !*ssid) return PRN_BE_PARAM;
    /* §20: empty PSK = open network (no password); 1–7 chars = incomplete password, fail synchronously */
    if (!psk || (psk[0] && strlen(psk) < 8)) {
        LOGW("mock: psk invalid (%zu, empty=open)", psk ? strlen(psk) : 0);
        return PRN_BE_FAILED;
    }
    if (!prn_state_is("disconnected")) return PRN_BE_BUSY;

    g_joining = 1;
    g_join_fail = (strstr(ssid, "fail") != NULL);
    g_join_t0 = now_ms();
    prn_set_string(PRN_SSID, ssid);          /* Also remember target ssid */
    prn_set_connection_state("joining");     /* Async transition after 3s → joined/disconnected (tick) */
    LOGI("mock: joining '%s' (3s)", ssid);
    return 0;
}

/* §20 Scan selection: simulate a 2.5s scan, return 4 fake networks (including a printer-style
 * hannto-* hotspot and one open network), strongest (-50) first; busy in every state other
 * than disconnected (joining/joined/ready/discovering). */
static int mb_scan_wifi(char ***out, int *n)
{
    if (!prn_state_is("disconnected")) {
        LOGW("mock: scan_wifi in state %s → busy", prn_state_get()->connection_state);
        return PRN_BE_BUSY;
    }
    /* Simulate a blocking scan (real backend uses iw/wpa_cli scan window) */
    for (int i = 0; i < 25; i++) usleep(100 * 1000);

    static const char *const nets[] = {
        "hannto-printer-honey1s_mibt4D0F\t-50\twpa2",  /* Printer hotspot (strongest, listed first) */
        "guguji-IOT\t-58\twpa2",
        "Coffee-Guest\t-72\topen",                       /* Open network: connect without password */
        "HomeNet_5G\t-80\twpa2",
    };
    prn_scanned_clear();
    for (size_t i = 0; i < sizeof nets / sizeof nets[0]; i++)
        prn_scanned_add(nets[i]);
    prn_state *st = prn_state_get();
    char **items = arr_new(st->scanned.n);
    for (int i = 0; i < st->scanned.n; i++)
        items[i] = strdup(st->scanned.items[i]);
    arr_put(out, n, items, st->scanned.n);
    LOGI("mock: scan_wifi done, %d network(s) (2.5s)", st->scanned.n);
    return 0;
}

static int mb_disconnect_restore(void)
{
    g_joining = 0;
    if (prn_job_active()) {                  /* Disconnect = force cancel */
        g_mj = MJ_NONE;
        prn_set_job_status("canceled");
        prn_set_job_progress(0);
        prn_set_printer_state("idle");
    }
    prn_discovered_clear();
    prn_set_printer_name("");
    prn_set_printer_ip("");
    prn_set_connection_state("disconnected");
    LOGI("mock: disconnected, wifi restored");
    return 0;
}

static int mb_start_discovery(char ***out, int *n)
{
    *out = NULL;
    *n = 0;
    if (prn_state_is("joining"))   return PRN_BE_BUSY;
    if (!prn_state_is("joined"))   return PRN_BE_FAILED;

    prn_set_connection_state("discovering");
    /* v2.2 semantics (match real backend): return empty as on acceptance, simulate 2s scan
     * in tick, publish results through discovered_printers notify */
    g_disc_run = 1;
    g_disc_t0 = now_ms();
    LOGI("mock: discovery accepted (publishing through discovered_printers notify after 2s)");
    return 0;
}

static int mb_select_printer(const char *name)
{
    prn_state *st = prn_state_get();
    if (st->discovered_n == 0) { LOGW("mock: select before discovery"); return PRN_BE_FAILED; }
    /* Match list first (no match=parametererror), then check state gate (reselection allowed) */
    for (int i = 0; i < st->discovered_n; i++) {
        const char *el = st->discovered[i];
        const char *tab = strchr(el, '\t');
        size_t nlen = tab ? (size_t)(tab - el) : strlen(el);
        int full = !strcmp(el, name);                       /* Full-string match */
        int part = (tab && strlen(name) == nlen && !strncmp(el, name, nlen)); /* Name only */
        if (full || part) {
            if (!prn_state_is("joined") && !prn_state_is("ready")) {
                LOGW("mock: select in state %s", st->connection_state);
                return PRN_BE_FAILED;
            }
            char nm[PRN_SLEN], ip[PRN_SLEN];
            snprintf(nm, sizeof nm, "%.*s", (int)nlen, el);
            snprintf(ip, sizeof ip, "%s", tab ? tab + 1 : "");
            prn_set_printer_name(nm);
            prn_set_printer_ip(ip);
            prn_set_connection_state("ready");
            LOGI("mock: selected '%s' @ %s", nm, ip);
            return 0;
        }
    }
    LOGW("mock: '%s' not in discovered list", name);
    return PRN_BE_PARAM;
}

static int suffix_is(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && !strcasecmp(s + ls - lf, suf);
}

static int mb_photo_list(const char *path, char ***out, int *n)
{
    if (!path || !*path) return PRN_BE_PARAM;
    DIR *d = opendir(path);
    if (!d) { LOGW("mock: opendir '%s' failed", path); return PRN_BE_PARAM; }
    char **items = arr_new(64);
    int cnt = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && cnt < 64) {
        if (e->d_name[0] == '.') continue;
        if (suffix_is(e->d_name, ".jpg") || suffix_is(e->d_name, ".jpeg") ||
            suffix_is(e->d_name, ".png")) {
            char buf[512];
            snprintf(buf, sizeof buf, "%s/%s", path, e->d_name);
            items[cnt++] = strdup(buf);
        }
    }
    closedir(d);
    arr_put(out, n, items, cnt);
    LOGI("mock: photo_list '%s' -> %d", path, cnt);
    return 0;
}

static int mb_start_print_job(char **paths, int npaths, const prn_job_opts_t *opts,
                              char **out_job_id)
{
    (void)paths; (void)opts;                          /* Mock skips capability validation/image processing */
    *out_job_id = NULL;
    if (!prn_state_is("ready")) { LOGW("mock: print in state %s", prn_state_get()->connection_state); return PRN_BE_FAILED; }
    if (prn_job_active()) return PRN_BE_BUSY;

    g_mj = MJ_PREPARING;
    g_mj_t0 = now_ms();
    prn_set_job_status("preparing");
    prn_set_printer_state("busy");
    char id[32];
    snprintf(id, sizeof id, "job-%d", ++g_jobno);
    *out_job_id = strdup(id);
    LOGI("mock: print job %s (%d file(s))", id, npaths);
    return 0;
}

/* Backend error detail: mock never fills it (backend.h declaration is weak, so this is a
 * weak definition; strong definition in backend_real overrides it when linked; effective
 * only when linking mock alone). */
const char *prn_be_last_error(void) { return ""; }

/* §18 three methods: mock has no image pipeline and returns failed (use --real backend) */
static int mb_photo_info(const char *path, prn_photo_info_t *out)
{
    (void)path; (void)out;
    LOGW("mock: photo_info has no image pipeline (use --real backend)");
    return PRN_BE_FAILED;
}
static int mb_photo_preview(const char *path, int max_w, int max_h, char **out_path)
{
    (void)path; (void)max_w; (void)max_h; (void)out_path;
    LOGW("mock: photo_preview has no image pipeline (use --real backend)");
    return PRN_BE_FAILED;
}
static int mb_preview_print(const char *path, const prn_job_opts_t *opts,
                            char **out_path)
{
    (void)path; (void)opts; (void)out_path;
    LOGW("mock: preview_print has no image pipeline (use --real backend)");
    return PRN_BE_FAILED;
}

static int mb_cancel_job(void)
{
    if (!prn_job_active()) return 0;
    g_mj = MJ_CANCELED;
    g_mj_t0 = now_ms();
    prn_set_job_status("canceled");
    LOGI("mock: job canceled");
    return 1;
}

static void mb_tick(void)
{
    /* Advance discovery state machine (v2.2 returns on acceptance: publish discovered_printers
     * and return to joined after 2s) */
    if (g_disc_run) {
        if (now_ms() - g_disc_t0 >= 2000) {
            g_disc_run = 0;
            prn_discovered_clear();               /* Simulate two printers (entries "name\tIP") */
            prn_discovered_add("Mi Photo Printer 1S [A1B2]\t192.168.43.1");
            prn_discovered_add("DIRECT-8B-Mi-Photo-Printer1S\t192.168.49.1");
            prn_set_connection_state("joined");
            LOGI("mock: discovery done, %d found", prn_state_get()->discovered_n);
        }
        return;                                    /* Do not advance jobs during discovery */
    }

    /* Advance connection state machine */
    if (g_joining) {
        if (now_ms() - g_join_t0 >= 3000) {
            g_joining = 0;
            if (g_join_fail) {
                prn_set_connection_state("disconnected");
                LOGW("mock: join failed (ssid contains 'fail')");
            } else {
                prn_set_connection_state("joined");
                LOGI("mock: joined");
            }
        }
        return;                                /* Do not advance jobs while connecting */
    }

    /* Advance print-job state machine (2%/sec) */
    prn_state *st = prn_state_get();
    long long dt = now_ms() - g_mj_t0;
    switch (g_mj) {
    case MJ_PREPARING:
        if (dt >= 2000) { g_mj = MJ_SENDING; g_mj_t0 = now_ms(); prn_set_job_status("sending"); }
        break;
    case MJ_SENDING:
        prn_set_job_progress((int)(dt / 500));            /* 2%/s */
        if (st->job_progress >= 40) { g_mj = MJ_PRINTING; g_mj_t0 = now_ms(); prn_set_job_status("printing"); }
        break;
    case MJ_PRINTING:
        prn_set_job_progress(40 + (int)(dt / 500));
        if (st->job_progress >= 100) { g_mj = MJ_DONE; g_mj_t0 = now_ms(); prn_set_job_progress(100); prn_set_job_status("done"); }
        break;
    case MJ_DONE:
        if (dt >= 2000) { g_mj = MJ_NONE; prn_set_job_status("idle"); prn_set_job_progress(0); prn_set_printer_state("idle"); }
        break;
    case MJ_CANCELED:
        if (dt >= 1000) { g_mj = MJ_NONE; prn_set_job_status("idle"); prn_set_job_progress(0); prn_set_printer_state("idle"); }
        break;
    case MJ_NONE:
    default:
        break;
    }
}

static const prn_backend g_mb = {
    .name = "mock",
    .tick = mb_tick,
    .connect_to_ap = mb_connect_to_ap,
    .scan_wifi = mb_scan_wifi,                        /* §20 network scan/selection */
    .disconnect_restore = mb_disconnect_restore,
    .start_discovery = mb_start_discovery,
    .select_printer = mb_select_printer,
    .photo_list = mb_photo_list,
    .start_print_job = mb_start_print_job,
    .cancel_job = mb_cancel_job,
    .photo_info = mb_photo_info,                     /* §18: stub */
    .photo_preview = mb_photo_preview,               /* §18: stub */
    .preview_print = mb_preview_print,               /* §18: stub */
};

const prn_backend *prn_backend_mock(void) { return &g_mb; }
