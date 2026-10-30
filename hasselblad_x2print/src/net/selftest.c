/* selftest.c — Networking-layer integration self-test utility (primary entry point for camera testing)
 *
 * Usage:
 *   selftest --mdns [scan-ms]            Scan _ipp._tcp.local (default 10s), list printers
 *   selftest --probe <ip> [port]         Full human-readable Get-Printer-Attributes listing
 *   selftest --dns <server> <name> [t]   Unicast DNS query to test encoding/decoding
 *                                        t = a|aaaa|ptr|srv|txt (default a)
 *   selftest --print <ip> <jpeg> [port]  Send Print-Job and poll until terminal job state
 *
 * Build (see build_termux.sh / TERMUX_TEST.md):
 *   gcc -std=c11 -O2 -Wall -Wextra -o selftest selftest.c ipp.c mdns.c netlog.c
 */
#define _DEFAULT_SOURCE /* nanosleep/strcasecmp */
#include "ipp.h"
#include "mdns.h"
#include "netlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(const char *p)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s --mdns [ms] [interface]  Scan mDNS printing services (default 10000; e.g. wlan0,\n"
            "                              omitted=auto-select multicast interface wlan0>non-loopback>loopback)\n"
            "  %s --dnsmock                Test offline decoding of a synthetic mDNS response\n"
            "  %s --probe <ip> [port]     Full printer capability listing\n"
            "  %s --dns <server> <name> [a|aaaa|ptr|srv|txt]\n"
            "  %s --print <ip> <jpeg> [port] [copies]\n",
            p, p, p, p, p);
}

static void sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

/* ---------- --mdns ---------- */

static int do_mdns(int total_ms, const char *ifname)
{
    printf("== mDNS scan _ipp._tcp.local (%d ms, interface=%s) ==\n", total_ms,
           (ifname && ifname[0]) ? ifname : "auto (wlan0 preferred)");
    mdns_service_t svc[16];
    int n = mdns_discover_ipp_if(svc, 16, total_ms, ifname);
    if (n < 0) {
        fprintf(stderr, "mDNS discovery failed: socket/bind error (see log above)\n");
        return 1;
    }
    if (n == 0) {
        printf("No IPP services found.\n"
               "Troubleshooting: 1) Confirm connection to printer hotspot (Mi Photo Printer 1S [last 4 digits of SN])\n"
               "      2) Printer direct-connect light should be solid blue  3) Try ./selftest --probe 192.168.x.1\n");
        return 2;
    }
    for (int i = 0; i < n; i++) {
        printf("[%d] Instance: %s\n", i, svc[i].instance);
        printf("    Host: %s  IP: %s  Port: %d%s\n", svc[i].host,
               svc[i].ip[0] ? svc[i].ip : "(unresolved)",
               svc[i].port, svc[i].universal ? "  [AirPrint/universal subtype]" : "");
        printf("    TXT : %s\n", svc[i].txt[0] ? svc[i].txt : "(none)");
        printf("    Next: ./selftest --probe %s %d\n",
               svc[i].ip[0] ? svc[i].ip : "<printer-IP>", svc[i].port ? svc[i].port : 631);
    }
    return 0;
}

/* ---------- --dnsmock (offline encoder/decoder unit test using synthetic mDNS response) ----------
 * Manually construct a message matching a camera mDNS response:
 *   answer: PTR(_ipp._tcp.local → instance) + SRV(instance → host:631)
 *   additional: TXT + A + AAAA; all names use compression pointers.
 * Decode with mdns_parse_reply and assert each item, covering branches unavailable offline
 * (AAAA/multiple sections/pointers). */

static void put16m(unsigned char *p, int *o, int v)
{
    p[(*o)++] = (unsigned char)(v >> 8);
    p[(*o)++] = (unsigned char)v;
}
static void put32m(unsigned char *p, int *o, unsigned v)
{
    put16m(p, o, (int)(v >> 16));
    put16m(p, o, (int)(v & 0xFFFF));
}
static int put_labels(unsigned char *p, int *o, const char *name)
{
    int start = *o;
    const char *s = name;
    while (*s) {
        const char *e = strchr(s, '.');
        int l = e ? (int)(e - s) : (int)strlen(s);
        p[(*o)++] = (unsigned char)l;
        memcpy(p + *o, s, (size_t)l);
        *o += l;
        if (!e) break;
        s = e + 1;
    }
    p[(*o)++] = 0;
    return start;
}
static void put_ptrm(unsigned char *p, int *o, int off)
{
    p[(*o)++] = (unsigned char)(0xC0 | (off >> 8));
    p[(*o)++] = (unsigned char)(off & 0xFF);
}
/* Write RR header with rdlen placeholder; return offset of rdlen field
 * (name points to earlier data) */
static int rr_hdr(unsigned char *p, int *o, int name_off, int type)
{
    put_ptrm(p, o, name_off);
    put16m(p, o, type);
    put16m(p, o, 1);
    put32m(p, o, 120);
    int rl = *o;
    put16m(p, o, 0);
    return rl;
}
/* As above, but name is already written inline (first RR must be inline before later pointers can reference it) */
static int rr_hdr_noname(unsigned char *p, int *o, int type)
{
    put16m(p, o, type);
    put16m(p, o, 1);
    put32m(p, o, 120);
    int rl = *o;
    put16m(p, o, 0);
    return rl;
}

typedef struct { int hits; const char *want_name; int want_type; const char *want_text; int want_port; int fail; } mock_chk;

static void mock_cb(void *ud, const mdns_rr_t *rr)
{
    mock_chk *m = (mock_chk *)ud;
    if (rr->type != m->want_type || strcmp(rr->name, m->want_name) != 0) return;
    int ok = strcmp(rr->text, m->want_text) == 0 &&
             (m->want_type != MDNS_T_SRV || rr->srv_port == m->want_port);
    printf("  %-4s %s -> %s", rr->type == MDNS_T_AAAA ? "AAAA" : "RR",
           rr->name, rr->text);
    if (rr->srv_port) printf(":%d", rr->srv_port);
    printf(" : %s\n", ok ? "PASS" : "FAIL");
    if (!ok) m->fail = 1;
    m->hits++;
}

static int do_dnsmock(void)
{
    unsigned char p[512];
    memset(p, 0, sizeof(p));
    int o = 12;
    /* First RR (PTR) name "_ipp._tcp.local" is inline at 12; later RRs reference it by pointer */
    int base = put_labels(p, &o, "_ipp._tcp.local");       /* base = 12 */
    int local_off = base + (1 + 4) + (1 + 4);              /* "local" label offset=22 */
    int rl = rr_hdr_noname(p, &o, MDNS_T_PTR);
    int inst = o;
    const char *in = "Mi Photo Printer 1S [1234]";
    p[o++] = (unsigned char)strlen(in);
    memcpy(p + o, in, strlen(in));
    o += (int)strlen(in);
    put_ptrm(p, &o, base);
    put16m(p, &rl, o - inst); /* Fill in rdlen */
    /* SRV: prio0 weight0 port631; target = label + pointer to local */
    rl = rr_hdr(p, &o, inst, MDNS_T_SRV);
    int rd = o;
    put16m(p, &o, 0);
    put16m(p, &o, 0);
    put16m(p, &o, 631);
    int host = o;
    const char *hn = "printer-abc";
    p[o++] = (unsigned char)strlen(hn);
    memcpy(p + o, hn, strlen(hn));
    o += (int)strlen(hn);
    put_ptrm(p, &o, local_off);
    put16m(p, &rl, o - rd);
    /* additional: TXT / A / AAAA */
    rl = rr_hdr(p, &o, inst, MDNS_T_TXT);
    rd = o;
    const char *kv = "rp=ipp/print";
    p[o++] = (unsigned char)strlen(kv);
    memcpy(p + o, kv, strlen(kv));
    o += (int)strlen(kv);
    put16m(p, &rl, o - rd);

    rl = rr_hdr(p, &o, host, MDNS_T_A);
    p[o++] = 192; p[o++] = 168; p[o++] = 4; p[o++] = 1;
    put16m(p, &rl, 4);

    rl = rr_hdr(p, &o, host, MDNS_T_AAAA);
    memset(p + o, 0, 16);
    p[o] = 0xFE;
    p[o + 1] = 0x80;
    p[o + 15] = 1; /* fe80::1 */
    o += 16;
    put16m(p, &rl, 16);

    p[2] = 0x84; p[3] = 0x00;      /* QR|AA */
    p[7] = 2;                      /* ANCOUNT = PTR+SRV */
    p[11] = 3;                     /* ARCOUNT = TXT+A+AAAA */

    printf("== Synthetic mDNS response decode test (%d bytes) ==\n", o);
    mock_chk c;
    int fail = 0;
    struct { const char *n; int t; const char *txt; int port; } exp[] = {
        { "_ipp._tcp.local", MDNS_T_PTR,
          "Mi Photo Printer 1S [1234]._ipp._tcp.local", 0 },
        { "Mi Photo Printer 1S [1234]._ipp._tcp.local", MDNS_T_SRV,
          "printer-abc.local", 631 },
        { "Mi Photo Printer 1S [1234]._ipp._tcp.local", MDNS_T_TXT,
          "rp=ipp/print", 0 },
        { "printer-abc.local", MDNS_T_A, "192.168.4.1", 0 },
        { "printer-abc.local", MDNS_T_AAAA, "fe80::1", 0 },
    };
    for (size_t i = 0; i < sizeof(exp) / sizeof(exp[0]); i++) {
        memset(&c, 0, sizeof(c));
        c.want_name = exp[i].n;
        c.want_type = exp[i].t;
        c.want_text = exp[i].txt;
        c.want_port = exp[i].port;
        if (mdns_parse_reply(p, o, mock_cb, &c) != 0) {
            printf("  Parse failed\n");
            return 1;
        }
        if (!c.hits) { printf("  %s: no match\n", exp[i].txt); fail = 1; }
        if (c.fail) fail = 1;
    }
    printf(fail ? "-- Some checks failed\n" : "-- All checks passed\n");
    return fail;
}

/* ---------- --dns (encoding/decoding unit test) ---------- */

static void dns_rr_print(void *ud, const mdns_rr_t *rr)
{
    (void)ud;
    static const char *tn[] = { [1] = "A", [5] = "CNAME", [12] = "PTR",
                                [16] = "TXT", [28] = "AAAA", [33] = "SRV",
                                [41] = "OPT(EDNS)" };
    const char *t = (rr->type >= 0 && rr->type <= 33 && tn[rr->type]) ? tn[rr->type] : "?";
    printf("  %-6s ttl=%-6u %s", t, (unsigned)rr->ttl, rr->name);
    if (rr->srv_port) printf(" -> %s:%d", rr->text, rr->srv_port);
    else if (rr->text[0]) printf(" -> %s", rr->text);
    else if (rr->rlen) printf(" (rlen=%d raw)", rr->rlen);
    printf("\n");
}

static int qtype_from_name(const char *s)
{
    if (!strcasecmp(s, "a")) return MDNS_T_A;
    if (!strcasecmp(s, "aaaa")) return MDNS_T_AAAA;
    if (!strcasecmp(s, "ptr")) return MDNS_T_PTR;
    if (!strcasecmp(s, "srv")) return MDNS_T_SRV;
    if (!strcasecmp(s, "txt")) return MDNS_T_TXT;
    return 0;
}

static int do_dns(const char *server, const char *name, const char *ts)
{
    int t = ts ? qtype_from_name(ts) : MDNS_T_A;
    if (!t) { fprintf(stderr, "Unknown query type: %s\n", ts); return 1; }
    printf("== Unicast DNS query %s: %s (type %d) ==\n", server, name, t);
    unsigned char ans[4096];
    int n = mdns_query_unicast(server, 53, name, t, ans, sizeof(ans), 4000);
    if (n <= 0) {
        fprintf(stderr, "Query failed (%d): network unavailable or DNS server unreachable\n", n);
        return 1;
    }
    if (mdns_parse_reply(ans, n, dns_rr_print, NULL) != 0) {
        fprintf(stderr, "Response parse failed (malformed message)\n");
        return 1;
    }
    printf("-- Encoding/decoding verification passed\n");
    return 0;
}

/* ---------- --probe ---------- */

typedef struct {
    ipp_caps_t *caps;
    int last_group;
} probe_ctx;

static void probe_attr(void *ud, int group, int tag, const char *name,
                       const unsigned char *val, int vlen)
{
    probe_ctx *pc = (probe_ctx *)ud;
    ipp_caps_feed(pc->caps, group, tag, name, val, vlen); /* Extract capabilities while printing */
    if (vlen == 0 && !strcmp(name, "media-size"))
        return; /* Synthetic collection-end event is for extraction only, not human-readable listing */
    if (group != pc->last_group) { /* Group changed: print group heading */
        printf("\n---- %s (0x%02X) ----\n", ipp_group_name(group), group);
        pc->last_group = group;
    }
    printf("  %-34s [%s] = ", name, ipp_tag_name(tag));
    if (tag == 0x21 || tag == 0x23) {
        printf("%d", vlen >= 4 ? (val[0] << 24 | val[1] << 16 | val[2] << 8 | val[3]) : 0);
    } else if (tag == 0x22) {
        printf("%s", vlen >= 1 && val[0] ? "true" : "false");
    } else {
        int printable = 1;
        for (int i = 0; i < vlen && printable; i++)
            if (val[i] < 0x20 || val[i] > 0x7E) printable = 0;
        if (printable && vlen > 0) {
            printf("%.*s", vlen, (const char *)val);
        } else {
            printf("(hex %d bytes)", vlen);
        }
    }
    if (tag == 0x23) {
        int v = vlen >= 4 ? (val[0] << 24 | val[1] << 16 | val[2] << 8 | val[3]) : 0;
        if (!strcmp(name, "printer-state")) printf("  = %s", ipp_printer_state_name(v));
        else if (!strcmp(name, "job-state")) printf("  = %s", ipp_job_state_name(v));
        else if (!strcmp(name, "print-quality")) {
            static const char *q[] = { "default", "?", "?", "draft(3)", "normal(4)", "high(5)" };
            printf("  = %s", v >= 0 && v <= 5 ? q[v] : "?");
        }
    }
    printf("\n");
}

static int do_probe(const char *ip, int port)
{
    printf("== IPP Get-Printer-Attributes %s:%d ==\n", ip, port);
    ipp_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    probe_ctx pc = { &caps, -1 };
    int e = ipp_get_printer_attributes_raw(ip, port, probe_attr, &pc);
    if (e != IPP_OK) {
        fprintf(stderr, "Probe failed [%s]: %s (IPP status 0x%04X, %s)\n",
                ipp_strerror(e), ipp_last_detail(), ipp_last_status(), ipp_strerror(e));
        return 1;
    }

    /* Summary: aggregate individual attributes into capability structure, then print highlights */
    ipp_caps_finish(&caps);
    printf("\n==== Summary ====\n");
    printf("printer-name           : %s\n", caps.printer_name[0] ? caps.printer_name : "?");
    printf("printer-make-and-model: %s\n", caps.printer_make_and_model[0] ? caps.printer_make_and_model : "?");
    printf("printer-state          : %s  reasons: %s\n",
           ipp_printer_state_name(caps.printer_state),
           caps.printer_state_reasons[0] ? caps.printer_state_reasons : "none");
    printf("document-format (%d)   :", caps.n_formats);
    for (int i = 0; i < caps.n_formats; i++) printf(" %s", caps.document_formats[i]);
    printf("\nmedia (%d)             :", caps.n_media);
    for (int i = 0; i < caps.n_media; i++) {
        printf(" %s", caps.media_supported[i]);
        if (caps.media_native[i]) printf("*");           /* *=native keyword can be sent */
        if (caps.media_x_um[i] > 0)
            printf("(%.1fx%.1fmm)", caps.media_x_um[i] / 1000.0, caps.media_y_um[i] / 1000.0);
    }
    printf("\nprint-color-mode (%d)  :", caps.n_modes);
    for (int i = 0; i < caps.n_modes; i++) printf(" %s", caps.print_color_modes[i]);
    printf("\nprint-quality (%d)     :", caps.n_quals);
    for (int i = 0; i < caps.n_quals; i++) printf(" %s", ipp_quality_kw(caps.qualities[i]));
    if (caps.quality_default) printf(" (default %s)", ipp_quality_kw(caps.quality_default));
    printf("\nprint-scaling (%d)     :", caps.n_scalings);
    for (int i = 0; i < caps.n_scalings; i++) printf(" %s", caps.print_scalings[i]);
    printf("\ncopies-supported       : %d..%d\n", caps.copies_min, caps.copies_max);
    printf("printer-resolution-def : %dx%d dpi%s\n", caps.res_default_x, caps.res_default_y,
           caps.res_default_x ? "" : " (not advertised; service fallback 300)");
    printf("media-default          : %s\n", caps.media_default[0] ? caps.media_default : "?");
    if (caps.jpeg_max_w)
        printf("Derived maximum target pixels: %dx%d (largest media × default resolution)\n",
               caps.jpeg_max_w, caps.jpeg_max_h);
    int tw, th;
    if (caps.media_default[0] && ipp_media_target_px(&caps, caps.media_default, &tw, &th) == 0)
        printf("Default media target pixels: %s → %dx%d\n", caps.media_default, tw, th);
    printf("\nResult: %s\n",
           caps.n_formats ? "if image/jpeg is listed, selftest --print can send JPEG directly"
                          : "no format list found — check document-format-supported output");
    return 0;
}

/* ---------- --print ---------- */

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    unsigned char *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static int do_print(const char *ip, const char *jpeg_path, int port, int copies)
{
    size_t jl = 0;
    unsigned char *jpeg = read_file(jpeg_path, &jl);
    if (!jpeg) { fprintf(stderr, "Failed to read file: %s\n", jpeg_path); return 1; }
    printf("== Print-Job %s:%d <- %s (%zu bytes, copies=%d) ==\n",
           ip, port, jpeg_path, jl, copies);
    ipp_print_opts_t o;
    memset(&o, 0, sizeof(o));
    o.copies = copies;
    o.job_name = "x2print-selftest";
    o.document_format = "image/jpeg"; /* 4:2:2 JPEG; let printer rasterize it */

    int job_id = 0;
    int e = ipp_print_jpeg(ip, port, jpeg, jl, &o, &job_id);
    free(jpeg);
    if (e != IPP_OK) {
        fprintf(stderr, "Print-Job failed [%s]: %s (IPP status 0x%04X)\n",
                ipp_strerror(e), ipp_last_detail(), ipp_last_status());
        return 1;
    }
    printf("job_id=%d; polling status (every 2s, up to 120s)...\n", job_id);
    for (int i = 0; i < 60; i++) {
        sleep_ms(2000);
        ipp_job_t job;
        e = ipp_get_job_attributes(ip, port, job_id, &job);
        if (e != IPP_OK) {
            fprintf(stderr, "Get-Job-Attributes failed [%s] %s\n",
                    ipp_strerror(e), ipp_last_detail());
            return 1;
        }
        printf("  job %d: %s (%s)\n", job.job_id,
               ipp_job_state_name(job.job_state),
               job.state_reasons[0] ? job.state_reasons : "-");
        if (job.job_state >= 7) { /* canceled/aborted/completed are terminal */
            printf("%s\n", job.job_state == 9 ? ">> Print complete" : ">> Job terminated abnormally");
            return job.job_state == 9 ? 0 : 3;
        }
    }
    printf(">> Timed out before terminal state (job may still be queued)\n");
    return 4;
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    x2p_netlog_set_level(X2P_LOG_INFO);
    if (argc < 2) { usage(argv[0]); return 1; }
    if (!strcmp(argv[1], "--mdns"))
        return do_mdns(argc >= 3 ? atoi(argv[2]) : 10000, argc >= 4 ? argv[3] : NULL);
    if (!strcmp(argv[1], "--dnsmock"))
        return do_dnsmock();
    if (!strcmp(argv[1], "--dns")) {
        if (argc < 4) { usage(argv[0]); return 1; }
        return do_dns(argv[2], argv[3], argc >= 5 ? argv[4] : "a");
    }
    if (!strcmp(argv[1], "--probe")) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return do_probe(argv[2], argc >= 4 ? atoi(argv[3]) : IPP_DEF_PORT);
    }
    if (!strcmp(argv[1], "--print")) {
        if (argc < 4) { usage(argv[0]); return 1; }
        return do_print(argv[2], argv[3],
                        argc >= 5 ? atoi(argv[4]) : IPP_DEF_PORT,
                        argc >= 6 ? atoi(argv[5]) : 1);
    }
    usage(argv[0]);
    return 1;
}
