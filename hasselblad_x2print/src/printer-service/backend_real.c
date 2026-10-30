/* Implementation details. */
#include "netlog.h"      /* Implementation details. */
#undef LOGD
#undef LOGI
#undef LOGW
#undef LOGE

#include "backend.h"
#include "adaptor.h"     /* Implementation details. */
#include "state.h"
#include "config.h"
#include "log.h"
#include "wifi_cam.h"
#include "ipp.h"
#include "mdns.h"
#include "imgprep.h"
#include "3fr.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>       /* inet_pton (determine selection by direct IP) */
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

#if defined(__ANDROID__)
#define X2P_TMPDIR "/data/x2print/tmp"   /* Camera: /tmp is not guaranteed to exist */
#define X2P_ROOT   "/data/x2print"
#else
#define X2P_TMPDIR "/tmp/x2print"
#define X2P_ROOT   "/tmp/x2print"
#endif

/* ---------------- netlog → service log bridge (same level order: 0 debug .. 3 error) ---------------- */
static void netlog_bridge(x2p_log_level_t lvl, const char *fmt, va_list ap)
{
    char buf[512];
    vsnprintf(buf, sizeof buf, fmt, ap);
    if (lvl <= X2P_LOG_DEBUG)      LOGD("[net] %s", buf);
    else if (lvl == X2P_LOG_INFO)  LOGI("[net] %s", buf);
    else if (lvl == X2P_LOG_WARN)  LOGW("[net] %s", buf);
    else                           LOGE("[net] %s", buf);
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------- Backend failure detail (included in error reply body) ---------------- */
static char g_be_err[192];
static void be_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_be_err, sizeof g_be_err, fmt, ap);
    va_end(ap);
}
const char *prn_be_last_error(void) { return g_be_err; } /* Strong definition (weak definition in mock) */

/* ---------------- Selected-printer cache ---------------- */
static char g_ip[64];       /* Currently selected printer IP (set by Select_printer) */
static int  g_port;         /* IPP port (0=631) */
static ipp_caps_t g_caps;   /* Selected printer capabilities (probed by Select_printer; source for print options) */

/* Discovery endpoint side table: discovered elements contain only "name\tIP" (wire contract);
 * port/path are stored here. */
typedef struct {
    char name[PRN_SLEN];
    char ip[64];
    int  port;              /* 0=default 631 */
    char path[96];          /* mDNS TXT rp= (includes leading /); empty=default /ipp/print */
} prn_ep_t;
static prn_ep_t g_eps[PRN_MAX_PRINTERS];
static int g_eps_n;

static void eps_clear(void) { g_eps_n = 0; }

/* Register endpoint (distinguishes multiple mocks on one IP with different ports or dual-stack);
 * return index or -1. */
static int eps_add(const char *name, const char *ip, int port, const char *path)
{
    if (!name || !ip || g_eps_n >= PRN_MAX_PRINTERS) return -1;
    prn_ep_t *e = &g_eps[g_eps_n];
    snprintf(e->name, sizeof e->name, "%s", name);
    snprintf(e->ip, sizeof e->ip, "%s", ip);
    e->port = port > 0 ? port : 0;
    snprintf(e->path, sizeof e->path, "%s", path && path[0] ? path : "");
    return g_eps_n++;
}

/* Locate endpoint by selection: accept full "name\tIP" entry / name / IP
 * (first matching entry wins if IP is duplicated). */
static const prn_ep_t *eps_find(const char *name, char *name_out, size_t name_cap)
{
    for (int i = 0; i < g_eps_n; i++) {
        char full[PRN_SLEN + 80];
        snprintf(full, sizeof full, "%.95s\t%.63s", g_eps[i].name, g_eps[i].ip);
        if (!strcmp(full, name) || !strcmp(g_eps[i].name, name) ||
            !strcmp(g_eps[i].ip, name)) {
            snprintf(name_out, name_cap, "%.95s", g_eps[i].name);
            return &g_eps[i];
        }
    }
    return NULL;
}

static int cfg_port(void)
{
    const char *p = prn_config_get("ipp_port");
    int v = p[0] ? atoi(p) : 0;
    return (v > 0 && v < 65536) ? v : 0;
}

static int wifi_is_mock(void)
{
    return !strcmp(prn_config_get("wifi_backend"), "mock");
}

/* Implementation details. */
static const char *txt_rp(const char *txt);                    /* Implementation details. */
static int tcp_scan_subnet(struct in_addr base, int port, in_addr_t *hits, int max_hits);

static struct {
    pid_t     pid;
    int       fd;                 /* Implementation details. */
    long long t0;
    int       budget_ms;
    const char *what;             /* Implementation details. */
} g_ch;

static int child_run(const char *what, int budget_ms, void (*fn)(void *), void *arg)
{
    if (g_ch.pid > 0) {
        LOGW("Operation failed %s", g_ch.what);
        return -1;
    }
    int pfd[2];
    if (pipe(pfd) != 0) { LOGE("real: pipe: %s", strerror(errno)); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); LOGE("real: fork: %s", strerror(errno)); return -1; }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) dup2(dn, STDERR_FILENO);
        x2p_netlog_set_hook(NULL);              /* Implementation details. */
        fn(arg);
        fflush(stdout);
        _exit(0);
    }
    close(pfd[1]);
    g_ch.pid = pid;
    g_ch.fd = pfd[0];
    g_ch.t0 = now_ms();
    g_ch.budget_ms = budget_ms;
    g_ch.what = what;
    return 0;
}

/* Implementation details. */
static int child_poll(char *out, size_t cap)
{
    out[0] = 0;
    if (g_ch.pid <= 0) return 0;
    int st = 0;
    pid_t r = waitpid(g_ch.pid, &st, WNOHANG);
    if (r == 0) {
        if (now_ms() - g_ch.t0 <= g_ch.budget_ms) return 1;
        LOGW("Operation timed out %s %d", g_ch.what, g_ch.budget_ms);
        kill(g_ch.pid, SIGKILL);
        waitpid(g_ch.pid, &st, 0);
        close(g_ch.fd);
        g_ch.pid = 0;
        return 0;
    }
    g_ch.pid = 0;
    if (r > 0) {
        ssize_t n = read(g_ch.fd, out, cap - 1);
        if (n < 0) n = 0;
        out[n] = 0;
    }
    close(g_ch.fd);
    return 0;
}

/* Implementation details. */
static void child_abort(void)
{
    if (g_ch.pid <= 0) return;
    LOGW("Operation interrupted %s", g_ch.what);
    kill(g_ch.pid, SIGKILL);
    int st;
    waitpid(g_ch.pid, &st, 0);
    close(g_ch.fd);
    g_ch.pid = 0;
}

/* Implementation details. */
typedef struct { char ip[64]; int port; char path[96]; } probe_arg_t;
static void probe_child(void *p)
{
    probe_arg_t *a = (probe_arg_t *)p;
    ipp_set_path(a->path[0] ? a->path : NULL);
    ipp_set_timeouts(5000, 12000);              /* Implementation details. */
    ipp_caps_t caps;
    if (ipp_get_printer_attributes(a->ip, a->port, &caps) != IPP_OK) {
        printf("FAIL\n");
        return;
    }
    printf("OK\t%.95s\n", caps.printer_name[0] ? caps.printer_name : "IPP Printer");
}

/* Implementation details. */
static void mdns_child(void *p)
{
    (void)p;
    mdns_service_t svcs[8];
    int c = mdns_discover_ipp_if(svcs, 8, 6000, "wlan0");
    for (int i = 0; i < c && i < 8; i++) {
        if (!svcs[i].ip[0]) continue;
        printf("SVC\t%.63s\t%d\t%d\t%.159s\t%.700s\n", svcs[i].ip, svcs[i].port,
               svcs[i].universal, svcs[i].instance, svcs[i].txt);
    }
}

/* Implementation details. */
typedef struct { struct in_addr base; int port; } scan_arg_t;
static void netscan_child(void *p)
{
    scan_arg_t *a = (scan_arg_t *)p;
    in_addr_t hits[8];
    int n = tcp_scan_subnet(a->base, a->port, hits, 8);
    for (int i = 0; i < n; i++) {
        struct in_addr h = { .s_addr = hits[i] };
        printf("HIT\t%s\n", inet_ntoa(h));
    }
}

/* Implementation details. */
typedef struct { char ip[64]; int port; int id; } poll_arg_t;
static void poll_child(void *p)
{
    poll_arg_t *a = (poll_arg_t *)p;
    ipp_set_timeouts(4000, 10000);              /* Implementation details. */
    ipp_job_t job;
    if (ipp_get_job_attributes(a->ip, a->port, a->id, &job) != IPP_OK) return;
    printf("ST\t%d\n", job.job_state);
}

/* Implementation details. */

/* Implementation details. */
static int jpeg_scan_size(const char *path, int *w_out, int *h_out, int *orient_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char soi[2];
    if (fread(soi, 1, 2, f) != 2 || soi[0] != 0xFF || soi[1] != 0xD8) { fclose(f); return -1; }

    int w = 0, h = 0, orient = 1;
    for (;;) {
        int m = fgetc(f);
        if (m == EOF) break;
        if (m != 0xFF) continue;                       /* Implementation details. */
        while (m == 0xFF) m = fgetc(f);                /* Implementation details. */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD9)) continue;
        unsigned char lb[2];
        if (fread(lb, 1, 2, f) != 2) break;
        int len = (lb[0] << 8) | lb[1];                /* Implementation details. */
        if (len < 2) break;
        if (m == 0xDA) break;                          /* Implementation details. */

        if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            unsigned char sof[6];                      /* precision,h(2),w(2) */
            if (len < 8 || fread(sof, 1, 6, f) != 6) break;
            h = (sof[1] << 8) | sof[2];
            w = (sof[3] << 8) | sof[4];
        } else if (m == 0xE1 && len >= 10) {           /* APP1: Exif Orientation */
            size_t blen = (size_t)len - 2;
            unsigned char *buf = malloc(blen);
            if (buf && fread(buf, 1, blen, f) == blen && blen >= 12 &&
                !memcmp(buf, "Exif\0\0", 6)) {
                const unsigned char *t = buf + 6;      /* Implementation details. */
                unsigned tlen = (unsigned)blen - 6;
                int be = (t[0] == 'M' && t[1] == 'M');
                #define RD16(p) (be ? ((p)[0] << 8) | (p)[1] : ((p)[1] << 8) | (p)[0])
                #define RD32(p) (be ? (RD16(p) << 16) | RD16((p) + 2) \
                                  : (RD16((p) + 2) << 16) | RD16(p))
                if (tlen >= 8 && RD16(t + 2) == 42) {
                    unsigned off = (unsigned)RD32(t + 4);
                    if (off + 2 <= tlen) {
                        int n = RD16(t + off);         /* Implementation details. */
                        for (int i = 0; i < n; i++) {
                            unsigned ep = off + 2 + (unsigned)i * 12;
                            if (ep + 12 > tlen) break;
                            const unsigned char *e = t + ep;
                            if (RD16(e) == 0x0112 && RD16(e + 2) == 3) {
                                int v = RD16(e + 8);   /* Implementation details. */
                                if (v >= 1 && v <= 8) orient = v;
                                break;
                            }
                        }
                    }
                }
                #undef RD16
                #undef RD32
            }
            free(buf);
        } else {
            if (fseek(f, len - 2, SEEK_CUR) != 0) break;
        }
    }
    fclose(f);
    if (!w || !h) return -1;
    *w_out = w;
    *h_out = h;
    *orient_out = orient;
    return 0;
}

/* Implementation details. */
static int oriented_size_ex(const char *path, const imgprep_edit_t *e, int *ow, int *oh)
{
    int w, h, o;
    if (jpeg_scan_size(path, &w, &h, &o) != 0) return -1;
    int swap;
    if (e && e->rotation >= 0) swap = ((e->rotation / 90) % 2) == 1;
    else                       swap = (o >= 5);
    *ow = swap ? h : w;
    *oh = swap ? w : h;
    return 0;
}

/* Implementation details. */
static void target_for_file(const char *path, int pw, int ph,
                            const imgprep_edit_t *edit, int *tw, int *th)
{
    *tw = pw;
    *th = ph;
    if (pw == ph) return;                         /* Implementation details. */
    int w, h;
    if (oriented_size_ex(path, edit, &w, &h) == 0 &&
        ((w > h) != (pw > ph))) {                 /* Implementation details. */
        *tw = ph;
        *th = pw;
    }
}

/* Implementation details. */
static int suffix_is(const char *s, const char *suf);   /* Implementation details. */
enum { RJ_IDLE = 0, RJ_PREP, RJ_SEND, RJ_PRINT, RJ_DONE, RJ_FAILED, RJ_CANCELED };
#define RJ_MAX_FILES 64
#define RJ_POLL_MS        1000
#define RJ_TOTAL_TIMEOUT_MS_BASE   120000              /* Implementation details. */
#define RJ_TOTAL_TIMEOUT_MS_PAGE   75000               /* Implementation details. */
#define RJ_HOLD_MS        3000     /* Implementation details. */

static struct {
    int  phase;
    char *files[RJ_MAX_FILES];                        /* Implementation details. */
    int  nfiles, cur;
    int  copies;
    int  jobno;                                       /* Implementation details. */
    int  ipp_ids[RJ_MAX_FILES];                       /* Implementation details. */
    int  done_jobs;
    long long t0, last_poll;
    int  poll_armed;                                 /* Implementation details. */
    char tmps[RJ_MAX_FILES][128];                     /* Implementation details. */
    /* Implementation details. */
    int  paper_w, paper_h;                            /* Implementation details. */
    int  quality_enum;                                /* Implementation details. */
    char media[PRN_SLEN];                             /* Implementation details. */
    char color[PRN_SLEN];                             /* Implementation details. */
    char scaling[32];                                 /* Implementation details. */
    imgprep_edit_t edit;                              /* Implementation details. */
} g_rj;

/* Implementation details. */
enum { RX_EV_JOB_STATUS = 1, RX_EV_CANCEL = 2 };

/* Implementation details. */
static void rj_emit_rx(int ev_type, unsigned job_id, const char *fmt, ...);  /* Implementation details. */
typedef struct { char ip[64]; int port; int ids[RJ_MAX_FILES]; int n; } cancel_arg_t;
static cancel_arg_t g_cancel_arg;
static int g_cancel_pending;              /* Implementation details. */

static void cancel_child(void *p)
{
    cancel_arg_t *a = (cancel_arg_t *)p;
    ipp_set_timeouts(4000, 10000);              /* Implementation details. */
    int sent = 0;
    for (int i = 0; i < a->n; i++)
        if (ipp_cancel_job(a->ip, a->port, a->ids[i]) == IPP_OK) sent++;
    int confirmed = 0;
    for (int try = 0; try < 12 && sent; try++) {
        int all = 1;
        for (int i = 0; i < a->n; i++) {
            ipp_job_t job;
            if (ipp_get_job_attributes(a->ip, a->port, a->ids[i], &job) == IPP_OK &&
                job.job_state != 7)
                all = 0;
        }
        if (all) { confirmed = sent; break; }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 250L * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    printf("RES\t%d\t%d\n", sent, confirmed);
}

/* Implementation details. */
static void cancel_job_async(void)
{
    /* Implementation details. */
    if (g_ch.pid > 0) {
        kill(g_ch.pid, SIGKILL);
        int st; waitpid(g_ch.pid, &st, 0);
        close(g_ch.fd);
        g_ch.pid = 0;
        g_rj.poll_armed = 0;
    }
    int n = 0;
    g_cancel_arg.ip[0] = 0;
    snprintf(g_cancel_arg.ip, sizeof g_cancel_arg.ip, "%.63s", g_ip);
    g_cancel_arg.port = g_port;
    for (int i = 0; i < g_rj.nfiles && n < RJ_MAX_FILES; i++)
        if (g_rj.ipp_ids[i] > 0) g_cancel_arg.ids[n++] = g_rj.ipp_ids[i];
    g_cancel_arg.n = n;
    if (n > 0 && g_ip[0]) {
        if (child_run("ipp cancel", 30000, cancel_child, &g_cancel_arg) == 0)
            g_cancel_pending = 1;
        else
            LOGW("Operation failed");
    }
}

static void tick_cancel(void)
{
    if (!g_cancel_pending) return;
    char out[64];
    if (child_poll(out, sizeof out) == 1) return;      /* Implementation details. */
    g_cancel_pending = 0;
    int sent = 0, confirmed = 0;
    sscanf(out, "RES\t%d\t%d", &sent, &confirmed);
    if (sent && !confirmed)
        LOGW("Operation failed %d", sent);
    rj_emit_rx(RX_EV_CANCEL, (unsigned)g_rj.jobno, "ipp_cancel sent=%d confirmed=%d",
               sent, confirmed);
    LOGI("real: job canceled (IPP Cancel-Job: sent=%d confirmed=%d)", sent, confirmed);
}

static void rj_free_files(void)
{
    for (int i = 0; i < g_rj.nfiles; i++) {
        free(g_rj.files[i]);
        g_rj.files[i] = NULL;
        if (g_rj.tmps[i][0]) {
            remove(g_rj.tmps[i]);                     /* Implementation details. */
            g_rj.tmps[i][0] = 0;
        }
    }
    g_rj.nfiles = 0;
    g_rj.cur = 0;
}

/* Implementation details. */
static void opts_to_edit(const prn_job_opts_t *o, imgprep_edit_t *e);

/* Implementation details. */
static void rj_emit_rx(int ev_type, unsigned job_id, const char *fmt, ...)
{
    unsigned char f[255];
    va_list ap;
    f[0] = 'X'; f[1] = '2'; f[2] = 'P';
    f[3] = (unsigned char)((unsigned)ev_type >> 8);
    f[4] = (unsigned char)((unsigned)ev_type & 0xff);
    f[5] = (unsigned char)(job_id >> 24);
    f[6] = (unsigned char)(job_id >> 16);
    f[7] = (unsigned char)(job_id >> 8);
    f[8] = (unsigned char)job_id;
    va_start(ap, fmt);
    int want = vsnprintf((char *)f + 9, sizeof f - 9, fmt, ap);
    va_end(ap);
    int sl = want < 0 ? 0 : (want > (int)(sizeof f - 9 - 1) ? (int)(sizeof f - 9 - 1) : want);
    int n = 9 + sl;
    prn_adaptor_emit_rx((unsigned char)n, f, n);
    LOGD("real: rx event ev=%d job=%u n=%d [%.*s]", ev_type, job_id, n, sl, (const char *)f + 9);
}

/* Implementation details. */
static void rj_set_status(const char *status, const char *why)
{
    prn_set_job_status(status);
    if (why) rj_emit_rx(RX_EV_JOB_STATUS, (unsigned)g_rj.jobno, "job_status=%s; %s", status, why);
    else     rj_emit_rx(RX_EV_JOB_STATUS, (unsigned)g_rj.jobno, "job_status=%s", status);
}

/* Implementation details. */
static int rj_prep_file(const char *cur, const char *dst, int paper_w, int paper_h,
                        const imgprep_edit_t *edit, imgprep_report_t *rep)
{
    char src_path[192];
    char prev_tmp[160];
    int tw, th, e, is_3fr = suffix_is(cur, ".3fr");

    snprintf(src_path, sizeof src_path, "%s", cur);
    prev_tmp[0] = 0;
    if (is_3fr) {
        unsigned char *prev = NULL;
        size_t plen = 0;
        int he = h3fr_extract_preview(cur, &prev, &plen);
        if (he != H3FR_OK || !prev || plen == 0) {
            LOGE("Operation failed %s %s", cur, h3fr_strerror(he));
            free(prev);
            be_err("Operation failed %s", h3fr_strerror(he));
            return -1;
        }
        snprintf(prev_tmp, sizeof prev_tmp, "%s/prev_%d_%d.jpg",
                 X2P_TMPDIR, (int)getpid(), (int)(now_ms() & 0xffff));
        FILE *pf = fopen(prev_tmp, "wb");
        if (!pf || fwrite(prev, 1, plen, pf) != plen) {
            LOGE("Operation failed %s", prev_tmp);
            free(prev);
            if (pf) fclose(pf);
            remove(prev_tmp);
            be_err("Operation failed");
            return -1;
        }
        fclose(pf);
        free(prev);
        snprintf(src_path, sizeof src_path, "%s", prev_tmp);
        LOGI("Operation failed %s %zu", cur, plen);
    } else if (suffix_is(cur, ".heic") || suffix_is(cur, ".heif")) {
        /* Implementation details. */
        snprintf(prev_tmp, sizeof prev_tmp, "%s/heic_%d_%d.jpg",
                 X2P_TMPDIR, (int)getpid(), (int)(now_ms() & 0xffff));
        char hcmd[320];
        snprintf(hcmd, sizeof hcmd, "%s/bin/heif_thumb %s > %s",
                 X2P_ROOT, cur, prev_tmp);
        int hrc = system(hcmd);
        /* Implementation details. */
        int ok = 0;
        if (hrc == 0) {
            FILE *hf = fopen(prev_tmp, "rb");
            if (hf) { unsigned char soi[2];
                ok = (fread(soi, 1, 2, hf) == 2 && soi[0] == 0xFF && soi[1] == 0xD8);
                fclose(hf); }
        }
        if (!ok) {
            LOGE("Operation failed %s %d", cur, hrc);
            remove(prev_tmp);
            be_err("Unsupported operation");
            return -1;
        }
        snprintf(src_path, sizeof src_path, "%s", prev_tmp);
        LOGI("Operation failed %s", cur);
    }

    target_for_file(src_path, paper_w, paper_h, edit, &tw, &th);
    e = imgprep_prepare_edit(src_path, dst, tw, th, is_3fr ? 0 : 92, 0, edit, rep);
    if (prev_tmp[0]) remove(prev_tmp);             /* Implementation details. */
    if (e != 0) {
        LOGE("Operation failed %s %s", cur, imgprep_strerror(e));
        be_err("Operation failed %s %s", imgprep_strerror(e),
               e == IMGPREP_E_ARG ? "Invalid value" : "");
        return -1;
    }
    return 0;
}

static void rj_finish(const char *status, const char *why)
{
    if (why) LOGW("real: job -> %s (%s)", status, why);
    if (!strcmp(status, "done")) {
        prn_set_job_progress(100);
        g_rj.phase = RJ_DONE;
    } else if (!strcmp(status, "canceled")) {
        g_rj.phase = RJ_CANCELED;
    } else {
        g_rj.phase = RJ_FAILED;
    }
    rj_set_status(status, why);
    prn_set_printer_state(!strcmp(status, "failed") ? "error" : "idle");
    g_rj.t0 = now_ms();
}

static void tick_job(void)
{
    long long now = now_ms();

    switch (g_rj.phase) {
    case RJ_PREP: {
        /* Implementation details. */
        const char *cur = g_rj.files[g_rj.cur];
        imgprep_report_t rep;
        if (rj_prep_file(cur, g_rj.tmps[g_rj.cur], g_rj.paper_w, g_rj.paper_h,
                         &g_rj.edit, &rep) != 0) {
            rj_finish("failed", suffix_is(cur, ".3fr") ? "3fr prep" : "imgprep");
            return;
        }
        LOGI("real: prep %d/%d %s → %dx%d (%ld B, rot=%d°, crop=%d,%d %dx%d)",
             g_rj.cur + 1, g_rj.nfiles, cur, rep.out_w, rep.out_h, rep.out_bytes,
             rep.rotation, rep.ucrop_x, rep.ucrop_y, rep.ucrop_w, rep.ucrop_h);
        if (++g_rj.cur >= g_rj.nfiles) {
            g_rj.cur = 0;
            g_rj.phase = RJ_SEND;
            g_rj.last_poll = 0;
            prn_set_job_progress(10);
            rj_set_status("sending", NULL);
        }
        break;
    }
    case RJ_SEND: {
        /* Implementation details. */
        FILE *f = fopen(g_rj.tmps[g_rj.cur], "rb");
        if (!f) { rj_finish("failed", "open prep output"); return; }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *jpeg = malloc(len > 0 ? (size_t)len : 1);
        if (!jpeg || (len > 0 && fread(jpeg, 1, (size_t)len, f) != (size_t)len)) {
            free(jpeg);
            fclose(f);
            rj_finish("failed", "read prep output");
            return;
        }
        fclose(f);

        ipp_print_opts_t o;
        memset(&o, 0, sizeof o);
        o.copies = g_rj.copies;
        o.color_mode = g_rj.color;                  /* Implementation details. */
        o.paper = g_rj.media;
        o.quality = g_rj.quality_enum;
        o.print_scaling = g_rj.scaling[0] ? g_rj.scaling : NULL;
        o.job_name = "x2print";
        o.document_format = "image/jpeg";           /* Implementation details. */

        int job_id = 0;
        int e = ipp_print_jpeg(g_ip, g_port, jpeg, (size_t)len, &o, &job_id);
        free(jpeg);
        if (e != IPP_OK) {
            LOGE("Operation failed %d %d %s %s", g_rj.cur + 1, g_rj.nfiles,
                 ipp_strerror(e), ipp_last_detail());
            rj_finish("failed", "ipp print");
            return;
        }
        g_rj.ipp_ids[g_rj.cur] = job_id;
        LOGI("real: sent %d/%d → ipp job %d (copies=%d media=%s)", g_rj.cur + 1,
             g_rj.nfiles, job_id, o.copies, o.paper);
        g_rj.cur++;
        prn_set_job_progress(10 + (30 * g_rj.cur) / g_rj.nfiles);
        if (g_rj.cur >= g_rj.nfiles) {
            g_rj.cur = 0;
            g_rj.phase = RJ_PRINT;
            g_rj.last_poll = 0;
            rj_set_status("printing", NULL);
            prn_set_job_progress(40);
        }
        break;
    }
    case RJ_PRINT: {
        /* Implementation details. */
        long long budget = RJ_TOTAL_TIMEOUT_MS_BASE
                         + (long long)RJ_TOTAL_TIMEOUT_MS_PAGE * g_rj.nfiles * (g_rj.copies > 0 ? g_rj.copies : 1);
        if (now - g_rj.t0 > budget) { rj_finish("failed", "total timeout"); return; }
        if (g_rj.last_poll && now - g_rj.last_poll < RJ_POLL_MS) return;
        g_rj.last_poll = now;

        int id = g_rj.ipp_ids[g_rj.cur];
        /* Implementation details. */
        static poll_arg_t pa;
        if (!g_rj.poll_armed) {
            snprintf(pa.ip, sizeof pa.ip, "%.63s", g_ip);
            pa.port = g_port; pa.id = id;
            if (child_run("job poll", 12000, poll_child, &pa) == 0)
                g_rj.poll_armed = 1;
            return;                                    /* Implementation details. */
        }
        char po[64];
        child_poll(po, sizeof po);
        g_rj.poll_armed = 0;
        int jstate = -1;
        if (sscanf(po, "ST\t%d", &jstate) != 1) {
            LOGW("Operation timed out %d", id);
            return;                                    /* Implementation details. */
        }
        if (jstate == 9) {                             /* completed */
            g_rj.cur++;
            g_rj.done_jobs++;
            if (g_rj.cur >= g_rj.nfiles) { rj_finish("done", NULL); return; }
            prn_set_job_progress(40 + (55 * g_rj.done_jobs) / g_rj.nfiles);
        } else if (jstate == 7) {
            rj_finish("canceled", "ipp job canceled");
        } else if (jstate == 8) {
            rj_finish("failed", "ipp job aborted");
        } else {                                       /* Implementation details. */
            int p = 40 + (55 * g_rj.done_jobs) / g_rj.nfiles
                    + 2 * (int)((now - g_rj.t0) / 4000);
            if (p > 95) p = 95;
            prn_set_job_progress(p);
        }
        break;
    }
    case RJ_DONE:
    case RJ_FAILED:
    case RJ_CANCELED:
        if (now - g_rj.t0 >= RJ_HOLD_MS) {             /* Implementation details. */
            g_rj.phase = RJ_IDLE;
            rj_free_files();
            rj_set_status("idle", NULL);
            prn_set_job_progress(0);
            prn_set_printer_state("idle");
        }
        break;
    case RJ_IDLE:
    default:
        break;
    }
}

/* Implementation details. */
enum {
    WJ_IDLE = 0,
    WJ_MOCK_WAIT,         /* Implementation details. */
    WJ_YIELD_AP,          /* Implementation details. */
    WJ_YIELD_WAIT,        /* Implementation details. */
    WJ_GUARD_A,           /* Implementation details. */
    WJ_GUARD_B,           /* Implementation details. */
    WJ_JOIN,              /* Implementation details. */
    WJ_RESTORE_WIFI,      /* Implementation details. */
    WJ_RESTORE_POWER,     /* Implementation details. */
    WJ_RESTORE_GUARD,     /* Implementation details. */
    WJ_RESTORE_FINAL,     /* Implementation details. */
};
static struct {
    int  phase;
    int  full_clear;      /* Implementation details. */
    long long t0;
    char ssid[PRN_SLEN];
    char psk[PRN_SLEN];
} g_wj;
static int g_ap_was_on = -1;    /* Implementation details. */
static int g_scan_kept = 0;     /* Implementation details. */
static int if_addr_netmask(const char *ifname, struct in_addr *addr, struct in_addr *mask); /* Implementation details. */
static void session_guard_a(void);     /* Implementation details. */
static void session_guard_b(void);
static void session_guard_off(void);
static void legal_power_sync(void);

/* Implementation details. */
#define AP_WAS_ON_FILE "/data/x2print/ap_was_on.saved"

static void ap_was_on_save(void)
{
    FILE *f = fopen(AP_WAS_ON_FILE, "w");
    if (f) { fputs("1\n", f); fclose(f); }
}

/* Implementation details. */
static int native_ap_running(void)
{
    DIR *d = opendir("/proc");
    if (d) {
        struct dirent *de;
        char fn[64], comm[64];
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
            snprintf(fn, sizeof fn, "/proc/%.20s/comm", de->d_name);
            FILE *f = fopen(fn, "r");
            if (!f) continue;
            if (fgets(comm, sizeof comm, f) && !strncmp(comm, "hostapd", 7)) {
                fclose(f);
                closedir(d);
                return 1;
            }
            fclose(f);
        }
        closedir(d);
    }
    struct in_addr a, m;
    if (if_addr_netmask("wlan0", &a, &m) == 0) {
        char ip[32];
        snprintf(ip, sizeof ip, "%s", inet_ntoa(a));
        if (!strcmp(ip, "192.168.2.1")) return 1;      /* Implementation details. */
    }
    return 0;
}

/* Implementation details. */
static void legal_power_sync(void)
{
    /* Implementation details. */
    g_ap_was_on = -1;
    remove(AP_WAS_ON_FILE);                        /* Implementation details. */
    if (prn_adaptor_system_wifi_power(0) != 0)
        LOGW("Operation failed");
    else
        LOGI("Operation completed");
}

/* Implementation details. */
static void restore_finalize(int full_clear)
{
    if (full_clear) {
        g_ip[0] = 0;
        eps_clear();
        prn_caps_invalidate();                             /* Implementation details. */
        prn_discovered_clear();
        prn_set_printer_name("");
        /* Implementation details. */
        prn_config_set("printer_ip", "");
        prn_set_printer_ip("");
        /* Implementation details. */
        prn_set_string(PRN_SSID, "");
        wifi_cam_psk_purge();
    }
    g_scan_kept = 0;                                       /* Implementation details. */
    prn_set_connection_state("disconnected");
    LOGI("real: disconnected, wifi restored (idle=%.0fs)",
         prn_adaptor_idle_ms() / 1000.0);
}

static void wj_join_done(int ok, const char *how)
{
    long long dt = now_ms() - g_wj.t0;
    memset(g_wj.psk, 0, sizeof g_wj.psk);              /* Implementation details. */
    if (ok) {
        g_wj.phase = WJ_IDLE;
        prn_set_connection_state("joined");
        LOGI("Operation failed %s %.1f", how, dt / 1000.0);
        return;
    }
    LOGW("Operation failed %s %.1f", how, dt / 1000.0);
    if (!wifi_is_mock()) {
        /* Implementation details. */
        wifi_cam_restore_begin();
        g_wj.full_clear = 1;   /* Implementation details. */
        g_wj.phase = WJ_RESTORE_WIFI;
        return;
    }
    g_wj.phase = WJ_IDLE;
    prn_set_connection_state("disconnected");
}

/* Implementation details. */
static void tick_join(void)
{
    switch (g_wj.phase) {
    case WJ_MOCK_WAIT:                                 /* Implementation details. */
        if (now_ms() - g_wj.t0 >= 1200) wj_join_done(1, "wifi_backend=mock");
        break;
    case WJ_YIELD_AP: {                                /* Implementation details. */
        if (g_ap_was_on < 0)
            g_ap_was_on = native_ap_running();
        if (g_ap_was_on == 1) {
            LOGI("Operation failed");
            ap_was_on_save();                          /* Implementation details. */
            prn_adaptor_system_wifi_power(0);
            g_wj.phase = WJ_YIELD_WAIT;                /* Implementation details. */
            g_wj.t0 = now_ms();
        } else {
            g_wj.phase = WJ_GUARD_A;
        }
        break;
    }
    case WJ_YIELD_WAIT:
        if (now_ms() - g_wj.t0 >= 1000) g_wj.phase = WJ_GUARD_A;
        break;
    case WJ_GUARD_A:                                   /* Implementation details. */
        session_guard_a();
        g_wj.phase = WJ_GUARD_B;
        break;
    case WJ_GUARD_B:                                   /* Implementation details. */
        session_guard_b();
        if (wifi_cam_join_start(g_wj.ssid, g_wj.psk) != 0) {  /* Implementation details. */
            be_err("Invalid value");
            wj_join_done(0, "wifi_cam_join_start");
            break;
        }
        g_wj.phase = WJ_JOIN;
        break;
    case WJ_JOIN: {                                    /* Implementation details. */
        int r = wifi_cam_join_step();                  /* Implementation details. */
        if (r == 0) wj_join_done(1, wifi_cam_join_phase());
        else if (r < 0) wj_join_done(0, wifi_cam_join_phase());
        break;
    }
    case WJ_RESTORE_WIFI:                              /* Implementation details. */
        if (wifi_cam_restore_step() == 0) g_wj.phase = WJ_RESTORE_POWER;
        break;
    case WJ_RESTORE_POWER:                             /* Implementation details. */
        legal_power_sync();
        g_wj.phase = WJ_RESTORE_GUARD;
        break;
    case WJ_RESTORE_GUARD:                             /* Implementation details. */
        session_guard_off();
        g_wj.phase = WJ_RESTORE_FINAL;
        break;
    case WJ_RESTORE_FINAL:
        restore_finalize(g_wj.full_clear);
        g_wj.phase = WJ_IDLE;
        break;
    default:
        break;
    }
}

/* Implementation details. */
static int rb_connect_to_ap(const char *ssid, const char *psk)
{
    if (!ssid || !*ssid) return PRN_BE_PARAM;
    if (g_wj.phase != WJ_IDLE) return PRN_BE_BUSY;
    if (!prn_state_is("disconnected")) return PRN_BE_BUSY;
    /* Implementation details. */
    if (!psk || (psk[0] && strlen(psk) < 8)) {
        LOGW("real: psk invalid len %zu (empty=open, 8..63=wpa2)", psk ? strlen(psk) : 0);
        return PRN_BE_FAILED;
    }
    prn_set_string(PRN_SSID, ssid);                    /* Implementation details. */
    prn_set_connection_state("joining");
    snprintf(g_wj.ssid, sizeof g_wj.ssid, "%s", ssid);
    snprintf(g_wj.psk, sizeof g_wj.psk, "%s", psk);
    g_wj.t0 = now_ms();
    g_wj.full_clear = 0;   /* Implementation details. */
    if (wifi_is_mock()) {
        g_wj.phase = WJ_MOCK_WAIT;
    } else {
        /* Implementation details. */
        g_wj.phase = WJ_YIELD_AP;
    }
    LOGI("real: joining '%s' via wifi_backend=%s", ssid,
         wifi_is_mock() ? "mock" : "Operation failed");
    return 0;                                          /* Implementation details. */
}

/* Implementation details. */
/* Implementation details. */
static void free_str_array_local(char **items, int n)
{
    if (!items) return;
    for (int i = 0; i < n; i++) free(items[i]);
    free(items);
}

enum { WS_IDLE = 0, WS_YIELD, WS_YIELD_WAIT, WS_GUARD_A, WS_GUARD_B, WS_SCAN };
static struct { int phase; long long t0; } g_ws;

/* Implementation details. */
static void ws_abort(void)
{
    if (g_ws.phase == WS_IDLE) return;
    LOGW("Operation interrupted");
    be_err("Operation interrupted");
    g_ws.phase = WS_IDLE;
    prn_adaptor_scan_complete(0);
}

static void ws_finish_ok(char **items, int cnt)
{
    prn_scanned_clear();
    for (int i = 0; i < cnt && i < PRN_MAX_SCAN; i++)
        prn_scanned_add(items[i]);
    free_str_array_local(items, cnt);
    g_scan_kept = 1;                    /* Implementation details. */
    g_ws.phase = WS_IDLE;
    prn_adaptor_scan_complete(1);       /* Implementation details. */
    LOGI("real: scan_wifi done, %d network(s) published (scanned_networks notify)",
         prn_state_get()->scanned.n);
}

static void tick_scan(void)
{
    switch (g_ws.phase) {
    case WS_YIELD:                                      /* Implementation details. */
        if (g_ap_was_on < 0)
            g_ap_was_on = native_ap_running();
        if (g_ap_was_on == 1) {
            LOGI("Operation failed");
            ap_was_on_save();
            prn_adaptor_system_wifi_power(0);
            g_ws.phase = WS_YIELD_WAIT;
            g_ws.t0 = now_ms();
        } else {
            g_ws.phase = WS_GUARD_A;
        }
        break;
    case WS_YIELD_WAIT:
        if (now_ms() - g_ws.t0 >= 1000) g_ws.phase = WS_GUARD_A;
        break;
    case WS_GUARD_A:
        session_guard_a();                              /* Implementation details. */
        g_ws.phase = WS_GUARD_B;
        break;
    case WS_GUARD_B:
        session_guard_b();
        if (wifi_cam_scan_begin() != 0) {
            be_err("Operation failed");
            g_scan_kept = 1;                            /* Implementation details. */
            g_ws.phase = WS_IDLE;
            prn_adaptor_scan_complete(0);
            break;
        }
        g_ws.phase = WS_SCAN;
        break;
    case WS_SCAN: {
        char **items = NULL;
        int cnt = 0;
        int r = wifi_cam_scan_step(&items, &cnt);
        if (r == 0) { ws_finish_ok(items, cnt); break; }
        if (r < 0) {
            be_err("Operation failed");
            g_scan_kept = 1;
            g_ws.phase = WS_IDLE;
            prn_adaptor_scan_complete(0);
        }
        break;
    }
    default:
        break;
    }
}

static int rb_scan_wifi(char ***out, int *n)
{
    *out = NULL;
    *n = 0;
    if (g_wj.phase != WJ_IDLE)
        return PRN_BE_BUSY;                            /* Implementation details. */
    if (!prn_state_is("disconnected"))
        return PRN_BE_BUSY;                            /* joining/joined/ready/discovering */
    if (g_scan_kept == 2 || g_ws.phase != WS_IDLE)
        return PRN_BE_BUSY;                            /* Implementation details. */

    if (wifi_is_mock()) {                              /* Implementation details. */
        struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        static const char *const nets[] = {
            "hannto-printer-honey1s_mibt4D0F\t-50\twpa2",
            "guguji-IOT\t-58\twpa2",
            "Coffee-Guest\t-72\topen",
            "HomeNet_5G\t-80\twpa2",
        };
        char **items = calloc(sizeof nets / sizeof nets[0] + 1, sizeof(char *));
        if (!items) return PRN_BE_FAILED;
        int cnt = 0;
        for (size_t i = 0; i < sizeof nets / sizeof nets[0]; i++)
            items[cnt++] = strdup(nets[i]);
        prn_scanned_clear();
        for (int i = 0; i < cnt && i < PRN_MAX_SCAN; i++)
            prn_scanned_add(items[i]);
        free_str_array_local(items, cnt);
        prn_state *st = prn_state_get();
        char **reply = calloc((size_t)st->scanned.n + 1, sizeof(char *));
        if (!reply) return PRN_BE_FAILED;
        for (int i = 0; i < st->scanned.n; i++)
            reply[i] = strdup(st->scanned.items[i]);
        *out = reply;
        *n = st->scanned.n;
        LOGI("real: scan_wifi done, %d network(s) published (scanned_networks notify)",
             st->scanned.n);
        return 0;
    }

    /* Implementation details. */
    g_ws.phase = WS_YIELD;
    g_ws.t0 = now_ms();
    g_scan_kept = 2;                    /* Implementation details. */
    LOGI("Operation completed");
    return 1;                           /* Implementation details. */
}

/* Implementation details. */
static void disc_abort(void);                   /* Implementation details. */
static int rb_disconnect_restore(void)
{
    if (g_wj.phase >= WJ_RESTORE_WIFI) {
        LOGI("Operation failed");
        return 0;
    }
    if (prn_job_active()) {                            /* Implementation details. */
        g_rj.phase = RJ_CANCELED;
        g_rj.t0 = now_ms();
        rj_set_status("canceled", "disconnect");
        prn_set_printer_state("idle");
        cancel_job_async();                             /* Implementation details. */
    }
    /* Implementation details. */
    wifi_cam_abort();
    child_abort();                                      /* Implementation details. */
    disc_abort();
    ws_abort();
    memset(g_wj.psk, 0, sizeof g_wj.psk);

    if (!wifi_is_mock()) {
        wifi_cam_restore_begin();                       /* Implementation details. */
        g_wj.full_clear = 1;
        g_wj.phase = WJ_RESTORE_WIFI;                   /* Implementation details. */
        LOGI("Operation failed");
        return 0;
    }
    restore_finalize(1);                                /* Implementation details. */
    return 0;
}

/* Implementation details. */
static const char *txt_rp(const char *txt)
{
    static char rp[96];
    if (!txt) return NULL;
    const char *p = txt;
    while (p) {
        const char *semi = strchr(p, ';');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        if (len > 3 && !strncmp(p, "rp=", 3)) {
            size_t c = len - 3 < sizeof rp - 1 ? len - 3 : sizeof rp - 1;
            memcpy(rp, p + 3, c);
            rp[c] = 0;
            return rp;
        }
        p = semi ? semi + 1 : NULL;
    }
    return NULL;
}

/* Implementation details. */
static int parse_hostport(const char *s, char *host, size_t cap)
{
    snprintf(host, cap, "%s", s);
    char *colon = strrchr(host, ':');
    if (colon) {
        int port = atoi(colon + 1);
        if (port > 0 && port < 65536) {
            *colon = 0;
            return port;
        }
        *colon = 0;                                    /* Implementation details. */
    }
    return 0;
}

/* Implementation details. */
#define DISC_BUDGET_MS  60000   /* Implementation details. */
#define DISC_PROBE_MS    15000  /* Implementation details. */
#define DISC_MDNS_MS     8000   /* Implementation details. */
#define DISC_SCAN_MS     6000   /* Implementation details. */

enum {
    D_IDLE = 0,
    D_L1,          /* Implementation details. */
    D_MDNS,        /* Implementation details. */
    D_MDNS_PROBE,  /* Implementation details. */
    D_GW,          /* Implementation details. */
    D_GW_PROBE,
    D_SCAN,        /* Implementation details. */
    D_SCAN_PROBE,  /* Implementation details. */
    D_FINISH,
};
static struct {
    int  phase;
    long long t0;
    int  defport, port;
    /* Implementation details. */
    char ep_ip[8][80]; int ep_port[8]; int n_eps, eps_i;
    /* Implementation details. */
    char m_ip[8][64]; int m_port[8]; char m_rp[8][96]; char m_inst[8][160];
    int  m_univ[8]; int n_mdns, mdns_i;
    /* Implementation details. */
    char hit[8][32]; int n_hits, hit_i;
    char gw[32]; int have_gw;
    int  found;
    int  m_started;                 /* Implementation details. */
    probe_arg_t parg;       /* Implementation details. */
    scan_arg_t sarg;
    char out[8192];
} g_disc;

static void disc_abort(void)
{
    if (g_disc.phase == D_IDLE) return;
    LOGW("Operation interrupted %d", g_disc.found);
    g_disc.phase = D_IDLE;
}

/* Implementation details. */
static void disc_probe_collect(const char *ip, int port, const char *path)
{
    if (strncmp(g_disc.out, "OK\t", 3) == 0) {
        char name[PRN_SLEN];
        snprintf(name, sizeof name, "%.95s", g_disc.out + 3);
        /* Implementation details. */
        char *nl = strchr(name, '\n');
        if (nl) *nl = 0;
        if (!name[0]) snprintf(name, sizeof name, "%s", "IPP Printer");
        if (eps_add(name, ip, port, path) >= 0) {
            char el[PRN_SLEN + 80];
            snprintf(el, sizeof el, "%s\t%s", name, ip);
            prn_discovered_add(el);
            g_disc.found++;
            LOGI("Operation failed %s %d %s", ip, port ? port : IPP_DEF_PORT, name);
            return;
        }
    } else {
        LOGW("Operation failed %s %d %.1f", ip,
             port ? port : IPP_DEF_PORT, (now_ms() - g_ch.t0) / 1000.0);
    }
}

/* Implementation details. */
static void disc_writeback(void)
{
    if (g_disc.found <= 0 || g_eps_n <= 0) return;
    char cache[300];
    size_t off = 0;
    cache[0] = 0;
    for (int i = 0; i < g_eps_n; i++) {
        char one[96];
        if (g_eps[i].port > 0)
            snprintf(one, sizeof one, "%s%s:%d", off ? "," : "",
                     g_eps[i].ip, g_eps[i].port);
        else
            snprintf(one, sizeof one, "%s%s", off ? "," : "", g_eps[i].ip);
        size_t ol = strlen(one);
        if (off + ol + 1 >= sizeof cache) break;
        memcpy(cache + off, one, ol + 1);
        off += ol;
    }
    const char *cur = prn_config_get("printer_ip");
    if (strcmp(cache, cur) != 0) {
        prn_config_set("printer_ip", cache);
        LOGI("Operation failed %s", cache);
    }
}

static void disc_finish(void)
{
    disc_writeback();
    ipp_set_path(NULL);                                /* Implementation details. */
    prn_set_connection_state("joined");                /* Implementation details. */
    LOGI("real: discovery done, %d found (%.1fs)", g_disc.found,
         (now_ms() - g_disc.t0) / 1000.0);
    g_disc.phase = D_IDLE;
}

static void disc_spawn_probe(const char *ip, int port, const char *path)
{
    snprintf(g_disc.parg.ip, sizeof g_disc.parg.ip, "%.63s", ip);
    g_disc.parg.port = port;
    snprintf(g_disc.parg.path, sizeof g_disc.parg.path, "%.95s", path ? path : "");
    child_run("ipp probe", DISC_PROBE_MS, probe_child, &g_disc.parg);
}

static int default_gw(char *gw, size_t cap);    /* Implementation details. */

static void tick_discovery(void)
{
    if (g_disc.phase == D_IDLE) return;
    if (now_ms() - g_disc.t0 > DISC_BUDGET_MS && g_disc.phase != D_FINISH) {
        LOGW("Operation failed %d %d", DISC_BUDGET_MS / 1000,
             g_disc.found);
        g_disc.phase = D_FINISH;
    }

    switch (g_disc.phase) {
    case D_L1:
        if (child_poll(g_disc.out, sizeof g_disc.out) == 1) return;   /* Implementation details. */
        if (g_disc.eps_i > 0)                          /* Implementation details. */
            disc_probe_collect(g_disc.ep_ip[g_disc.eps_i - 1],
                               g_disc.ep_port[g_disc.eps_i - 1], NULL);
        if (g_disc.eps_i < g_disc.n_eps) {
            int i = g_disc.eps_i++;
            LOGI("Operation failed %s %d %d %d",
                 g_disc.ep_ip[i], g_disc.ep_port[i] ? g_disc.ep_port[i] : IPP_DEF_PORT,
                 i + 1, g_disc.n_eps);
            disc_spawn_probe(g_disc.ep_ip[i], g_disc.ep_port[i], NULL);
            g_disc.phase = D_L1;
            return;
        }
        g_disc.phase = (g_disc.found == 0) ? D_MDNS : D_FINISH;
        return;
    case D_MDNS:
        if (!g_disc.m_started) {                        /* Implementation details. */
            if (child_run("mdns window", DISC_MDNS_MS, mdns_child, NULL) != 0) {
                LOGW("Operation failed");
                g_disc.phase = D_GW;
                return;
            }
            g_disc.m_started = 1;
            return;
        }
        if (child_poll(g_disc.out, sizeof g_disc.out) == 1) return;
        g_disc.n_mdns = 0;
        for (char *line = strtok(g_disc.out, "\n"); line; line = strtok(NULL, "\n")) {
            char ip[64], inst[160], txt[768];
            int port = 0, univ = 0;
            if (sscanf(line, "SVC\t%63s\t%d\t%d\t%159[^\t]\t%767[^\n]",
                       ip, &port, &univ, inst, txt) == 5 && g_disc.n_mdns < 8) {
                int i = g_disc.n_mdns++;
                snprintf(g_disc.m_ip[i], sizeof g_disc.m_ip[i], "%s", ip);
                g_disc.m_port[i] = port;
                g_disc.m_univ[i] = univ;
                const char *rp = txt_rp(txt);
                snprintf(g_disc.m_rp[i], sizeof g_disc.m_rp[i], "%s", rp ? rp : "");
                snprintf(g_disc.m_inst[i], sizeof g_disc.m_inst[i], "%s", inst);
            }
        }
        if (g_disc.n_mdns > 0) {
            g_disc.mdns_i = 0;
            g_disc.phase = D_MDNS_PROBE;
        } else {
            LOGI("Operation failed");
            g_disc.phase = D_GW;
        }
        return;
    case D_MDNS_PROBE:
        if (child_poll(g_disc.out, sizeof g_disc.out) == 1) return;
        if (g_disc.mdns_i > 0) {
            int i = g_disc.mdns_i - 1;
            disc_probe_collect(g_disc.m_ip[i], g_disc.m_port[i], g_disc.m_rp[i]);
        }
        if (g_disc.mdns_i < g_disc.n_mdns) {
            int i = g_disc.mdns_i++;
            char *dot = strstr(g_disc.m_inst[i], "._ipp");   /* Implementation details. */
            if (dot) *dot = 0;
            LOGI("Operation failed %s %s %d %s %s",
                 g_disc.m_inst[i], g_disc.m_ip[i],
                 g_disc.m_port[i] ? g_disc.m_port[i] : IPP_DEF_PORT,
                 g_disc.m_rp[i][0] ? g_disc.m_rp[i] : "(default)",
                 g_disc.m_univ[i] ? " [AirPrint]" : "");
            disc_spawn_probe(g_disc.m_ip[i], g_disc.m_port[i], g_disc.m_rp[i]);
            return;
        }
        g_disc.phase = (g_disc.found == 0) ? D_GW : D_FINISH;
        return;
    case D_GW: {
        if (child_poll(g_disc.out, sizeof g_disc.out) == 1) return;   /* Implementation details. */
        g_disc.have_gw = (default_gw(g_disc.gw, sizeof g_disc.gw) == 0);
        if (g_disc.have_gw) {
            LOGI("Operation failed %s %d", g_disc.gw, g_disc.port);
            disc_spawn_probe(g_disc.gw, g_disc.defport, NULL);
            g_disc.phase = D_GW_PROBE;
        } else {
            LOGW("Operation failed");
            g_disc.phase = D_SCAN;
        }
        return;
    }
    case D_GW_PROBE:
        if (child_poll(g_disc.out, sizeof g_disc.out) == 1) return;
        disc_probe_collect(g_disc.gw, g_disc.defport, NULL);
        g_disc.phase = (g_disc.found == 0) ? D_SCAN : D_FINISH;
        return;
    case D_SCAN: {
        if (child_poll(g_disc.out, sizeof g_disc.out) == 1) return;
        struct in_addr a, m;
        if (if_addr_netmask("wlan0", &a, &m) == 0) {
            g_disc.sarg.base.s_addr = a.s_addr & m.s_addr;
            g_disc.sarg.port = g_disc.port;
            LOGI("Operation failed %s %d", inet_ntoa(g_disc.sarg.base), g_disc.port);
            if (child_run("subnet scan", DISC_SCAN_MS, netscan_child, &g_disc.sarg) == 0) {
                g_disc.phase = D_SCAN_PROBE;
                g_disc.hit_i = 0;
                g_disc.n_hits = 0;
                return;
            }
        } else {
            LOGW("Operation failed");
        }
        g_disc.phase = D_FINISH;
        return;
    }
    case D_SCAN_PROBE:
        if (child_poll(g_disc.out, sizeof g_disc.out) == 1) return;
        if (g_disc.hit_i == 0 && g_disc.n_hits == 0) {
            /* Implementation details. */
            for (char *line = strtok(g_disc.out, "\n"); line; line = strtok(NULL, "\n")) {
                char ip[32];
                if (sscanf(line, "HIT\t%31s", ip) == 1 && g_disc.n_hits < 8)
                    snprintf(g_disc.hit[g_disc.n_hits++], sizeof g_disc.hit[0], "%s", ip);
            }
        } else if (g_disc.hit_i > 0) {
            disc_probe_collect(g_disc.hit[g_disc.hit_i - 1], g_disc.defport, NULL);
        }
        if (g_disc.hit_i < g_disc.n_hits && g_disc.found < 4) {
            int i = g_disc.hit_i++;
            disc_spawn_probe(g_disc.hit[i], g_disc.defport, NULL);
            return;
        }
        g_disc.phase = D_FINISH;
        return;
    case D_FINISH:
        disc_finish();
        return;
    default:
        g_disc.phase = D_IDLE;
        return;
    }
}

static int rb_start_discovery(char ***out, int *n)
{
    *out = NULL;
    *n = 0;                            /* Implementation details. */
    if (g_wj.phase != WJ_IDLE || prn_state_is("joining") || prn_state_is("discovering"))
        return PRN_BE_BUSY;

    const char *direct = prn_config_get("printer_ip");
    if (direct[0] == 0 && !prn_state_is("joined") && !prn_state_is("ready")) {
        LOGW("real: discovery needs joined state or printer_ip direct mode");
        return PRN_BE_FAILED;
    }

    prn_set_connection_state("discovering");
    prn_discovered_clear();
    eps_clear();
    memset(&g_disc, 0, sizeof g_disc);
    g_disc.t0 = now_ms();
    g_disc.defport = cfg_port();
    g_disc.port = g_disc.defport ? g_disc.defport : IPP_DEF_PORT;

    /* Implementation details. */
    if (direct[0]) {
        char list[512];
        snprintf(list, sizeof list, "%s", direct);
        char *save = NULL;
        for (char *tok = strtok_r(list, ",", &save); tok;
             tok = strtok_r(NULL, ",", &save)) {
            char host[80];
            int p = parse_hostport(tok, host, sizeof host);
            if (!host[0] || g_disc.n_eps >= 8) continue;
            snprintf(g_disc.ep_ip[g_disc.n_eps], sizeof g_disc.ep_ip[0], "%s", host);
            g_disc.ep_port[g_disc.n_eps] = p ? p : g_disc.defport;
            g_disc.n_eps++;
        }
    }
    g_disc.phase = (g_disc.n_eps > 0) ? D_L1 : D_MDNS;
    LOGI("Operation failed %d %d"
         "Operation completed",
         g_disc.n_eps, DISC_BUDGET_MS / 1000);
    return 0;
}

/* Implementation details. */

/* Implementation details. */
static int default_gw(char *gw, size_t cap)
{
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return -1;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char iface[32];
        unsigned dest, gwhex;
        if (sscanf(line, "%31s %x %x", iface, &dest, &gwhex) != 3) continue;
        if (dest != 0 || gwhex == 0) continue;          /* Implementation details. */
        struct in_addr a = { .s_addr = gwhex };        /* Implementation details. */
        snprintf(gw, cap, "%s", inet_ntoa(a));
        found = 1;
        LOGI("Operation failed %s %s", gw, iface);
        break;
    }
    fclose(f);
    return found ? 0 : -1;
}

/* Implementation details. */
static int if_addr_netmask(const char *ifname, struct in_addr *addr, struct in_addr *mask)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct ifreq fr;
    memset(&fr, 0, sizeof fr);
    snprintf(fr.ifr_name, sizeof fr.ifr_name, "%s", ifname);
    if (ioctl(s, SIOCGIFADDR, &fr) != 0) { close(s); return -1; }
    *addr = ((struct sockaddr_in *)&fr.ifr_addr)->sin_addr;
    if (ioctl(s, SIOCGIFNETMASK, &fr) != 0) { close(s); return -1; }
    *mask = ((struct sockaddr_in *)&fr.ifr_netmask)->sin_addr;
    close(s);
    return 0;
}

/* Implementation details. */
static int tcp_scan_subnet(struct in_addr base, int port, in_addr_t *hits, int max_hits)
{
    int total = 0;
    for (int start = 1; start <= 254 && total < max_hits; start += 48) {
        int fds[48];
        in_addr_t addrs[48];
        int nf = 0, maxfd = 0;
        fd_set wf;
        FD_ZERO(&wf);
        for (int h = start; h < start + 48 && h <= 254 && nf < 48; h++) {
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof sa);
            sa.sin_family = AF_INET;
            sa.sin_port = htons((unsigned short)port);
            sa.sin_addr.s_addr = base.s_addr + htonl((unsigned)h);
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;
            int fl = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, fl | O_NONBLOCK);
            int r = connect(fd, (struct sockaddr *)&sa, sizeof sa);
            if (r != 0 && errno != EINPROGRESS) { close(fd); continue; }
            fds[nf] = fd;
            addrs[nf] = sa.sin_addr.s_addr;
            FD_SET(fd, &wf);
            nf++;
            if (fd > maxfd) maxfd = fd;
        }
        if (nf > 0) {
            struct timeval tv = { .tv_sec = 0, .tv_usec = 800000 };   /* Implementation details. */
            select(maxfd + 1, NULL, &wf, NULL, &tv);
            for (int i = 0; i < nf && total < max_hits; i++) {
                if (!FD_ISSET(fds[i], &wf)) continue;   /* Implementation details. */
                int soerr = 0;
                socklen_t sl = sizeof soerr;
                getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &soerr, &sl);
                if (soerr == 0) hits[total++] = addrs[i];
            }
            for (int i = 0; i < nf; i++) close(fds[i]);
        }
    }
    return total;
}

/* Implementation details. */
static const char *negotiate_format(const ipp_caps_t *caps)
{
    int jpeg = 0, raster = 0;
    for (int i = 0; i < caps->n_formats; i++) {
        if (!strcmp(caps->document_formats[i], "image/jpeg")) jpeg = 1;
        else if (!strcmp(caps->document_formats[i], "image/pwg-raster") ||
                 !strcmp(caps->document_formats[i], "image/urf") ||
                 !strcmp(caps->document_formats[i], "cups-raster")) raster = 1;
    }
    if (jpeg) return "image/jpeg";
    if (raster) {
        be_err("Unsupported operation %s",
               caps->n_formats ? caps->document_formats[0] : "raster");
        LOGE("Operation failed %s", g_be_err);
        return NULL;
    }
    be_err("Operation failed %d", caps->n_formats);
    LOGE("Operation failed %s", g_be_err);
    return NULL;
}

static int rb_select_printer(const char *name)
{
    if (!name || !*name) return PRN_BE_PARAM;
    be_err("");
    prn_state *st = prn_state_get();
    if (st->discovered_n == 0) { LOGW("real: select before discovery"); return PRN_BE_FAILED; }

    char el_name[PRN_SLEN];
    /* Implementation details. */
    const prn_ep_t *ep = eps_find(name, el_name, sizeof el_name);
    const char *el_ip = ep ? ep->ip : NULL;
    int el_port = ep ? ep->port : 0;
    const char *el_path = ep ? ep->path : NULL;
    if (!el_ip) {
        /* Implementation details. */
        char host[80];
        int port = parse_hostport(name, host, sizeof host);
        struct in_addr dummy;
        if (host[0] && inet_pton(AF_INET, host, &dummy) == 1) {
            el_ip = host;
            el_port = port ? port : cfg_port();
            el_path = NULL;
            snprintf(el_name, sizeof el_name, "%s", host);
        }
    }
    if (!el_ip) { LOGW("real: '%s' not in discovered list", name); return PRN_BE_PARAM; }
    if (!prn_state_is("joined") && !prn_state_is("ready")) {
        LOGW("real: select in state %s", st->connection_state);
        return PRN_BE_FAILED;
    }

    /* Implementation details. */
    prn_caps_invalidate();                             /* Implementation details. */
    ipp_set_path(el_path && el_path[0] ? el_path : NULL);
    ipp_caps_t caps;
    ipp_set_timeouts(5000, 12000);
    int e = ipp_get_printer_attributes(el_ip, el_port, &caps);
    ipp_set_timeouts(0, 0);
    if (e != IPP_OK) {
        LOGE("real: probe %s failed: %s (%s)", el_ip, ipp_strerror(e), ipp_last_detail());
        be_err("Operation failed %s", ipp_last_detail());
        return PRN_BE_FAILED;
    }

    /* Implementation details. */
    const char *docfmt = negotiate_format(&caps);
    if (!docfmt) return PRN_BE_FAILED;

    snprintf(g_ip, sizeof g_ip, "%.63s", el_ip);
    g_port = el_port > 0 ? el_port : IPP_DEF_PORT;
    g_caps = caps;
    prn_set_printer_name(el_name);
    prn_set_printer_ip(el_ip);
    prn_set_printer_state(caps.printer_state == 4 ? "busy"
                         : caps.printer_state == 5 ? "error" : "idle");
    prn_caps_publish(&g_caps, docfmt);                  /* Implementation details. */
    prn_set_connection_state("ready");
    LOGI("real: selected '%s' @ %s:%d path=%s (docfmt=%s media=%d copies_max=%d res=%d)",
         el_name, el_ip, g_port, ipp_get_path(), docfmt,
         caps.n_media, caps.copies_max, caps.res_default_x);
    return 0;
}

static int suffix_is(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && !strcasecmp(s + ls - lf, suf);
}

static int rb_photo_list(const char *path, char ***out, int *n)
{
    const char *dir = (path && *path) ? path : prn_config_get("photo_root");
    if (!dir || !*dir) dir = "/mnt/media_runtime";     /* Implementation details. */
    DIR *d = opendir(dir);
    if (!d) { LOGW("real: opendir '%s' failed", dir); return PRN_BE_PARAM; }
    char **items = calloc(RJ_MAX_FILES + 1, sizeof(char *));
    if (!items) { closedir(d); return PRN_BE_FAILED; }
    int cnt = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && cnt < RJ_MAX_FILES) {
        if (de->d_name[0] == '.') continue;
        /* Implementation details. */
        if (suffix_is(de->d_name, ".jpg") || suffix_is(de->d_name, ".jpeg")
            || suffix_is(de->d_name, ".3fr")
            || suffix_is(de->d_name, ".heic") || suffix_is(de->d_name, ".heif")) {
            char buf[512];
            snprintf(buf, sizeof buf, "%s/%s", dir, de->d_name);
            items[cnt++] = strdup(buf);
        }
    }
    closedir(d);
    *out = items;
    *n = cnt;
    LOGI("real: photo_list '%s' -> %d", dir, cnt);
    return 0;
}

/* Implementation details. */
static int resolve_job_opts(const prn_job_opts_t *opts, int *copies, int *quality_enum,
                            const char **media, const char **color, const char **scaling,
                            int *paper_w, int *paper_h)
{
    prn_state *st = prn_state_get();
    be_err("");
    if (!st->caps_ready) {
        LOGW("real: caps not ready (select_printer first)");
        be_err("Not ready");
        return PRN_BE_FAILED;
    }

    *copies = (opts && opts->copies > 0) ? opts->copies : st->copies;
    if (*copies < 1 || *copies > st->copies_max) {
        LOGW("Operation failed %d %d", *copies, st->copies_max);
        be_err("Operation failed %d %d", *copies, st->copies_max);
        return PRN_BE_PARAM;
    }

    *media = (opts && opts->media && opts->media[0]) ? opts->media : st->media;
    if (!prn_list_contains(&st->sup_media, *media)) {
        LOGW("Operation failed %s", *media);
        be_err("Operation failed %s", *media);
        return PRN_BE_PARAM;
    }
    int w, h;
    int pe = ipp_media_target_px(&g_caps, *media, &w, &h);   /* Implementation details. */
    if (pe == -2) {
        LOGW("Operation failed %s", *media);
        be_err("Operation failed %s", *media);
        return PRN_BE_PARAM;
    }
    *paper_w = w;
    *paper_h = h;

    const char *q = (opts && opts->print_quality && opts->print_quality[0])
                    ? opts->print_quality : st->print_quality;
    *quality_enum = ipp_quality_enum(q);
    if (!*quality_enum || !prn_list_contains(&st->sup_qualities, q)) {
        LOGW("Operation failed %s", q);
        be_err("Operation failed %s", q);
        return PRN_BE_PARAM;
    }

    *color = (opts && opts->color_mode && opts->color_mode[0])
             ? opts->color_mode : st->color_mode;
    if (!prn_list_contains(&st->sup_color_modes, *color)) {
        LOGW("Operation failed %s", *color);
        be_err("Operation failed %s", *color);
        return PRN_BE_PARAM;
    }

    *scaling = (opts && opts->print_scaling && opts->print_scaling[0])
               ? opts->print_scaling : NULL;
    if (*scaling && g_caps.n_scalings > 0) {           /* Implementation details. */
        int hit = 0;
        for (int i = 0; i < g_caps.n_scalings; i++)
            if (!strcmp(g_caps.print_scalings[i], *scaling)) { hit = 1; break; }
        if (!hit) {
            LOGW("Operation failed %s", *scaling);
            be_err("Operation failed %s", *scaling);
            return PRN_BE_PARAM;
        }
    }
    return 0;
}

static int rb_start_print_job(char **paths, int npaths, const prn_job_opts_t *opts,
                              char **out_job_id)
{
    *out_job_id = NULL;
    if (!paths || npaths <= 0) return PRN_BE_PARAM;
    if (!prn_state_is("ready")) {
        LOGW("real: print in state %s", prn_state_get()->connection_state);
        return PRN_BE_FAILED;
    }
    if (prn_job_active()) return PRN_BE_BUSY;
    if (!g_ip[0]) {
        LOGW("real: no printer selected (select_printer first)");
        return PRN_BE_FAILED;
    }

    /* Implementation details. */
    int copies, quality_enum, paper_w, paper_h;
    const char *media, *color, *scaling;
    int rc = resolve_job_opts(opts, &copies, &quality_enum, &media, &color, &scaling,
                              &paper_w, &paper_h);
    if (rc != 0) return rc;

    if (prn_mkdir_p(X2P_TMPDIR) != 0) {
        LOGE("real: mkdir %s failed", X2P_TMPDIR);
        return PRN_BE_FAILED;
    }

    rj_free_files();
    g_rj.jobno++;                                      /* Implementation details. */
    for (int i = 0; i < npaths && i < RJ_MAX_FILES; i++)
        g_rj.files[g_rj.nfiles++] = strdup(paths[i]);
    g_rj.copies = copies;
    g_rj.done_jobs = 0;
    g_rj.quality_enum = quality_enum;
    snprintf(g_rj.media, sizeof g_rj.media, "%s", media);
    snprintf(g_rj.color, sizeof g_rj.color, "%s", color);
    snprintf(g_rj.scaling, sizeof g_rj.scaling, "%s", scaling ? scaling : "");
    g_rj.paper_w = paper_w;
    g_rj.paper_h = paper_h;
    opts_to_edit(opts, &g_rj.edit);                       /* Implementation details. */
    for (int i = 0; i < RJ_MAX_FILES; i++) {
        g_rj.ipp_ids[i] = 0;
        snprintf(g_rj.tmps[i], sizeof g_rj.tmps[0], "%s/prep_%d_%d_%d.jpg",
                 X2P_TMPDIR, (int)getpid(), g_rj.jobno, i);
    }

    prn_set_job_progress(0);
    rj_set_status("preparing", NULL);                  /* propertiesChanged + rx(ev=1) */
    prn_set_printer_state("busy");
    g_rj.phase = RJ_PREP;
    g_rj.t0 = now_ms();
    g_rj.last_poll = 0;

    char id[32];
    snprintf(id, sizeof id, "job-%d", g_rj.jobno);
    *out_job_id = strdup(id);
    LOGI("real: print job %s accepted (%d file(s), copies=%d/%d, media=%s, target=%dx%d, "
         "quality=%d, color=%s%s%s, rot=%d°, crop=%.3f,%.3f,%.3f,%.3f)", id,
         g_rj.nfiles, copies, prn_state_get()->copies_max, media, paper_w, paper_h,
         quality_enum, color, scaling ? ", scaling=" : "", scaling ? scaling : "",
         g_rj.edit.rotation, g_rj.edit.crop_x, g_rj.edit.crop_y,
         g_rj.edit.crop_w, g_rj.edit.crop_h);
    return 0;
}

static int rb_cancel_job(void)
{
    if (!prn_job_active()) {
        be_err("Operation failed %s", prn_state_get()->job_status);
        LOGW("real: cancel_job: %s", prn_be_last_error());
        return 0;                                      /* Implementation details. */
    }
    /* Implementation details. */
    g_rj.phase = RJ_CANCELED;
    g_rj.t0 = now_ms();
    rj_set_status("canceled", NULL);
    prn_set_printer_state("idle");
    cancel_job_async();
    return 1;
}

/* Implementation details. */

#if defined(__ANDROID__)
#define X2P_PREVIEW_DIR "/data/x2print/preview"      /* Implementation details. */
#else
#define X2P_PREVIEW_DIR "/tmp/x2print/preview"
#endif

/* Implementation details. */
static unsigned long long path_hash64(const char *s)
{
    unsigned long long h = 1469598103934665603ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

/* Implementation details. */
static void opts_to_edit(const prn_job_opts_t *o, imgprep_edit_t *e)
{
    if (o) {
        e->crop_x = o->crop_x; e->crop_y = o->crop_y;
        e->crop_w = o->crop_w; e->crop_h = o->crop_h;
        e->rotation = o->rotation;
    } else {
        e->crop_x = 0; e->crop_y = 0; e->crop_w = 1; e->crop_h = 1;
        e->rotation = -1;
    }
}

/* Implementation details. */
static int mtime_ge(const struct stat *a, const struct stat *b)
{
    if (a->st_mtim.tv_sec != b->st_mtim.tv_sec)
        return a->st_mtim.tv_sec > b->st_mtim.tv_sec;
    return a->st_mtim.tv_nsec >= b->st_mtim.tv_nsec;
}

/* Implementation details. */
static int rb_photo_info(const char *path, prn_photo_info_t *out)
{
    if (!path || !*path) {
        be_err("Value is empty");
        return PRN_BE_PARAM;
    }
    /* Implementation details. */
    char heic_tmp[192] = "";
    const char *probe_path = path;
    if (suffix_is(path, ".heic") || suffix_is(path, ".heif")) {
        snprintf(heic_tmp, sizeof heic_tmp, "%s/info_%d.jpg",
                 X2P_TMPDIR, (int)getpid());
        char hcmd[320];
        snprintf(hcmd, sizeof hcmd, "%s/bin/heif_thumb %s > %s 2>/dev/null",
                 X2P_ROOT, path, heic_tmp);
        if (system(hcmd) != 0) {
            remove(heic_tmp);
            be_err("Operation failed %s", path);
            return PRN_BE_FAILED;
        }
        probe_path = heic_tmp;
    }
    imgprep_info_t ii;
    int e = imgprep_probe(probe_path, &ii);
    if (heic_tmp[0]) { remove(heic_tmp); probe_path = path; }
    if (e != IMGPREP_OK) {
        be_err("%s: %s", path, imgprep_strerror(e));
        LOGW("Operation failed %s %s", path, imgprep_strerror(e));
        return e == IMGPREP_E_IO ? PRN_BE_PARAM : PRN_BE_FAILED;
    }
    out->width = ii.width;
    out->height = ii.height;
    out->orientation = ii.orientation;
    out->rotated_width = ii.rotated_width;
    out->rotated_height = ii.rotated_height;
    out->has_jpeg = ii.has_jpeg;
    out->is_3fr = ii.is_3fr;
    out->file_size = ii.file_size;
    LOGI("real: photo_info %s -> %dx%d orient=%d rotated=%dx%d %s(%ld B)",
         path, ii.width, ii.height, ii.orientation, ii.rotated_width,
         ii.rotated_height, ii.is_3fr ? "[3FR] " : "", ii.file_size);
    return 0;
}

/* Implementation details. */
static int rb_photo_preview(const char *path, int max_w, int max_h, char **out_path)
{
    *out_path = NULL;
    if (!path || !*path) {
        be_err("Value is empty");
        return PRN_BE_PARAM;
    }
    if (max_w <= 0) max_w = 1024;                    /* Implementation details. */
    if (max_h <= 0) max_h = 1024;
    if (max_w > 4096) max_w = 4096;                  /* Implementation details. */
    if (max_h > 4096) max_h = 4096;

    struct stat sst;
    if (stat(path, &sst) != 0) {
        be_err("Does not exist %s", path);
        return PRN_BE_PARAM;
    }
    /* Implementation details. */
    prn_photo_info_t pi;
    int rc = rb_photo_info(path, &pi);
    if (rc != 0) return rc;

    double sc = 1.0;
    if (pi.rotated_width > max_w || pi.rotated_height > max_h)
        sc = (double)max_w / pi.rotated_width < (double)max_h / pi.rotated_height
             ? (double)max_w / pi.rotated_width : (double)max_h / pi.rotated_height;
    int tw = (int)(pi.rotated_width * sc + 0.5);
    int th = (int)(pi.rotated_height * sc + 0.5);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    if (prn_mkdir_p(X2P_PREVIEW_DIR) != 0) {
        LOGE("real: mkdir %s failed", X2P_PREVIEW_DIR);
        return PRN_BE_FAILED;
    }
    char cache[256];
    snprintf(cache, sizeof cache, "%s/%016llx_%dx%d.jpg", X2P_PREVIEW_DIR,
             path_hash64(path), tw, th);

    /* Implementation details. */
    struct stat cst;
    if (stat(cache, &cst) == 0 && cst.st_size > 0 && mtime_ge(&cst, &sst)) {
        LOGI("Operation failed %s %d %d", cache, tw, th);
        *out_path = strdup(cache);
        return 0;
    }

    long long t0 = now_ms();
    int e;
    if (pi.is_3fr) {
        e = imgprep_from_3fr_edit(path, cache, tw, th, 0, IMGPREP_F_FAST, NULL, NULL);
    } else if (suffix_is(path, ".heic") || suffix_is(path, ".heif")) {
        /* Implementation details. */
        char htmp[192];
        snprintf(htmp, sizeof htmp, "%s/pv_%d_%d.jpg",
                 X2P_TMPDIR, (int)getpid(), (int)(now_ms() & 0xffff));
        char hcmd[320];
        snprintf(hcmd, sizeof hcmd, "%s/bin/heif_thumb %s > %s 2>/dev/null",
                 X2P_ROOT, path, htmp);
        if (system(hcmd) != 0) { remove(htmp); be_err("Operation failed"); return PRN_BE_FAILED; }
        e = imgprep_prepare_edit(htmp, cache, tw, th, 0, IMGPREP_F_FAST, NULL, NULL);
        remove(htmp);
    } else {
        e = imgprep_prepare_edit(path, cache, tw, th, 0, IMGPREP_F_FAST, NULL, NULL);
    }
    if (e != IMGPREP_OK) {
        be_err("%s: %s", path, imgprep_strerror(e));
        LOGW("Operation failed %s %s", path, imgprep_strerror(e));
        remove(cache);                                 /* Implementation details. */
        return e == IMGPREP_E_IO ? PRN_BE_PARAM : PRN_BE_FAILED;
    }
    LOGI("real: photo_preview %s -> %s (%dx%d, %.0f ms)", path, cache, tw, th,
         (now_ms() - t0) / 1.0);
    *out_path = strdup(cache);
    return 0;
}

/* Implementation details. */
#define PV_RING_N 8
static char g_pv_ring[PV_RING_N][192];
static int  g_pv_ring_head;                         /* Implementation details. */
static int  g_pv_ring_len;
static int  g_pvno;

static int rb_preview_print(const char *path, const prn_job_opts_t *opts,
                            char **out_path)
{
    *out_path = NULL;
    if (!path || !*path) {
        be_err("Value is empty");
        return PRN_BE_PARAM;
    }
    if (prn_job_active()) {
        be_err("Operation failed %s", prn_state_get()->job_status);
        return PRN_BE_BUSY;
    }
    if (access(path, R_OK) != 0) {
        be_err("Does not exist %s", path);
        return PRN_BE_PARAM;
    }

    /* Implementation details. */
    int copies, quality_enum, paper_w, paper_h;
    const char *media, *color, *scaling;
    int rc = resolve_job_opts(opts, &copies, &quality_enum, &media, &color,
                              &scaling, &paper_w, &paper_h);
    if (rc != 0) return rc;                          /* Implementation details. */

    /* Implementation details. */
    imgprep_edit_t edit;
    opts_to_edit(opts, &edit);
    int ow, oh;
    if (oriented_size_ex(path, &edit, &ow, &oh) != 0) {
        if (suffix_is(path, ".3fr")) {
            /* Implementation details. */
            prn_photo_info_t pi;
            if (rb_photo_info(path, &pi) != 0) return PRN_BE_FAILED;
            int swap = (edit.rotation >= 0 && (edit.rotation / 90) % 2 == 1);
            ow = swap ? pi.height : pi.width;
            oh = swap ? pi.width : pi.height;
        } else {
            be_err("Operation failed %s", path);
            return PRN_BE_FAILED;
        }
    }
    if (imgprep_edit_validate(&edit, ow, oh) != IMGPREP_OK) {
        be_err("Invalid value %d", IMGPREP_MIN_CROP_PX);
        LOGW("Invalid value %s %.3f %.3f %.3f %.3f %d %d %d",
             path, edit.crop_x, edit.crop_y, edit.crop_w, edit.crop_h, edit.rotation, ow, oh);
        return PRN_BE_PARAM;
    }

    if (prn_mkdir_p(X2P_PREVIEW_DIR) != 0) {
        LOGE("real: mkdir %s failed", X2P_PREVIEW_DIR);
        return PRN_BE_FAILED;
    }
    char out[192];
    snprintf(out, sizeof out, "%s/job_%d_preview.jpg", X2P_PREVIEW_DIR, ++g_pvno);
    /* Implementation details. */
    if (g_pv_ring_len == PV_RING_N) {
        remove(g_pv_ring[g_pv_ring_head]);
        g_pv_ring_len--;
    }
    snprintf(g_pv_ring[g_pv_ring_head], sizeof g_pv_ring[0], "%s", out);
    g_pv_ring_head = (g_pv_ring_head + 1) % PV_RING_N;
    if (g_pv_ring_len < PV_RING_N) g_pv_ring_len++;

    imgprep_report_t rep;
    long long t0 = now_ms();
    if (rj_prep_file(path, out, paper_w, paper_h, &edit, &rep) != 0) {
        remove(out);
        g_pv_ring_len--;                              /* Implementation details. */
        return PRN_BE_FAILED;                         /* Implementation details. */
    }
    LOGI("real: preview_print job_%d %s -> %dx%d (%ld B, rot=%d°, crop=%d,%d %dx%d, "
         "media=%s, %.0f ms)", g_pvno, path, rep.out_w, rep.out_h, rep.out_bytes,
         rep.rotation, rep.ucrop_x, rep.ucrop_y, rep.ucrop_w, rep.ucrop_h, media,
         (now_ms() - t0) / 1.0);
    *out_path = strdup(out);
    return 0;
}

/* Implementation details. */
static void tick_idle_watchdog(void)
{
    static long long armed_at = 0;                     /* Implementation details. */
    static int prev_active = 0;                        /* Implementation details. */
    int active = prn_job_active();
    if (prev_active && !active) {
        prn_adaptor_touch();                           /* Implementation details. */
        LOGD("Operation failed");
    }
    prev_active = active;
    const char *k = prn_config_get("idle_restore_min");
    int min = k[0] ? atoi(k) : 5;                      /* Implementation details. */
    if (min <= 0) return;
    if (g_wj.phase != WJ_IDLE) {                         /* Implementation details. */
        armed_at = now_ms();
        return;
    }
    if (g_disc.phase != D_IDLE) {                        /* Implementation details. */
        armed_at = now_ms();
        return;
    }
    if (!prn_state_is("joined") && !prn_state_is("ready") && !g_scan_kept) {
        armed_at = 0;
        return;
    }
    if (active) {                                      /* Implementation details. */
        armed_at = now_ms();
        return;
    }
    long long idle = prn_adaptor_idle_ms();
    long long limit = (long long)min * 60 * 1000;
    if (idle < limit) return;
    if (armed_at && now_ms() - armed_at < 1000) return;
    armed_at = now_ms();
    LOGW("Operation failed %.1f %d",
         idle / 60000.0, min);
    rb_disconnect_restore();                           /* Implementation details. */
}

static void rb_tick(void)
{
    tick_join();
    tick_scan();
    tick_discovery();
    tick_cancel();                                     /* Implementation details. */
    tick_job();
    tick_idle_watchdog();
}

/* Implementation details. */
#define SCR_SAVE_FILE "/data/x2print/screen_timeout.saved"
#define SCR_SESSION   86400                       /* Implementation details. */
static int g_scr_saved = -1;                      /* Implementation details. */

/* Implementation details. */
static void session_guard_a(void)
{
    wifi_cam_wake_hold();                         /* Implementation details. */
    int cur = -1;
    if (prn_adaptor_screen_idle_get(&cur) == 0 && cur > 0 && cur < SCR_SESSION) {
        g_scr_saved = cur;
        FILE *f = fopen(SCR_SAVE_FILE, "w");      /* Implementation details. */
        if (f) { fprintf(f, "%d\n", cur); fclose(f); }
    } else if (cur >= SCR_SESSION) {
        LOGI("Operation failed %d", SCR_SESSION);
    } else {
        LOGW("Operation failed %d", cur);
    }
}

/* Implementation details. */
static void session_guard_b(void)
{
    if (g_scr_saved > 0) {
        if (prn_adaptor_screen_idle_set(SCR_SESSION) == 0)
            LOGI("Operation failed %d %d",
                 g_scr_saved, SCR_SESSION);
        else
            LOGW("Operation failed");
    }
}

static void session_guard_off(void)
{
    if (g_scr_saved > 0) {
        if (prn_adaptor_screen_idle_set(g_scr_saved) == 0)
            LOGI("Operation failed %d", g_scr_saved);
        else
            LOGW("Operation failed %d",
                 g_scr_saved);
        remove(SCR_SAVE_FILE);
        g_scr_saved = -1;
    }
    wifi_cam_wake_release();                      /* Implementation details. */
}

/* Implementation details. */
static void rb_boot_init(void)
{
    wifi_cam_wake_release();                      /* Implementation details. */
    FILE *f = fopen(SCR_SAVE_FILE, "r");
    if (f) {
        int v = -1;
        int got = fscanf(f, "%d", &v);
        fclose(f);
        remove(SCR_SAVE_FILE);
        if (got == 1 && v > 0) {
            if (prn_adaptor_screen_idle_set(v) == 0)
                LOGI("Operation failed %d", v);
            else
                LOGW("Operation failed %d", v);
        }
    }
    /* Implementation details. */
    if (!wifi_is_mock())
        wifi_cam_boot_cleanup();
    /* Implementation details. */
    f = fopen(AP_WAS_ON_FILE, "r");
    if (f) { fclose(f); remove(AP_WAS_ON_FILE); }
}

static const prn_backend g_rb = {
    .name = "real",
    .tick = rb_tick,
    .boot_init = rb_boot_init,                        /* Implementation details. */
    .connect_to_ap = rb_connect_to_ap,
    .scan_wifi = rb_scan_wifi,                         /* Implementation details. */
    .disconnect_restore = rb_disconnect_restore,
    .start_discovery = rb_start_discovery,
    .select_printer = rb_select_printer,
    .photo_list = rb_photo_list,
    .start_print_job = rb_start_print_job,
    .cancel_job = rb_cancel_job,
    .photo_info = rb_photo_info,                     /* §18 */
    .photo_preview = rb_photo_preview,               /* §18 */
    .preview_print = rb_preview_print,               /* §18 */
};

const prn_backend *prn_backend_real(void)
{
    x2p_netlog_set_hook(netlog_bridge);                /* Implementation details. */
    x2p_netlog_set_level(X2P_LOG_INFO);
    return &g_rb;
}
