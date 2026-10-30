/* ipp.c — Minimal IPP client + HTTP/1.1 wrapper implementation
 *
 * Encoding (RFC 8010): version(2) | operation-id(2) | request-id(4) |
 *   [group tag(1) + attribute(value tag(1) name length(2) name value length(2) value)* ]* | 0x03 | data
 * Decoding reverses this structure; skip unknown value tags by length without crashing.
 * Flatten collection (0x34) members (memberAttrName 0x4A) into ordinary attributes for display/extraction.
 *
 * Transport: cleartext HTTP/1.1 over TCP 631, POST /ipp/print, Content-Type: application/ipp,
 *   Connection: close; responses support Content-Length(identity) and chunked transfer encoding.
 *
 * Version negotiation: default IPP/2.0(0x0200); if printer returns 0x0503
 *   (server-error-version-not-supported), fall back to 1.1(0x0110) and retry once.
 */
#define _DEFAULT_SOURCE

#include "ipp.h"
#include "netlog.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ---------------- Error details ---------------- */

static unsigned int g_status;       /* Most recent IPP status-code */
static char g_detail[256];          /* Human-readable detail */

unsigned int ipp_last_status(void) { return g_status; }
const char *ipp_last_detail(void)  { return g_detail; }

static void set_detail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_detail, sizeof(g_detail), fmt, ap);
    va_end(ap);
}

const char *ipp_strerror(int err)
{
    switch (err) {
    case IPP_OK:        return "success";
    case IPP_E_ARG:     return "invalid argument";
    case IPP_E_SOCKET:  return "socket creation failed";
    case IPP_E_CONNECT: return "connection failed/timed out";
    case IPP_E_SEND:    return "send failed";
    case IPP_E_TIMEOUT: return "response timed out";
    case IPP_E_TRUNC:   return "incomplete response";
    case IPP_E_HTTP:    return "HTTP layer error";
    case IPP_E_PARSE:   return "IPP message parse failed";
    case IPP_E_STATUS:  return "IPP status is not ok";
    case IPP_E_NOMEM:   return "out of memory";
    default:            return "unknown error";
    }
}

/* ---------------- Dynamic buffer ---------------- */

typedef struct {
    unsigned char *b;
    size_t len, cap;
    int oom;
} buf_t;

static int b_reserve(buf_t *o, size_t need)
{
    if (o->oom) return -1;
    if (o->len + need <= o->cap) return 0;
    size_t nc = o->cap ? o->cap * 2 : 8192;
    while (nc < o->len + need) nc *= 2;
    unsigned char *nb = realloc(o->b, nc);
    if (!nb) { o->oom = 1; return -1; }
    o->b = nb;
    o->cap = nc;
    return 0;
}

static void b_u8(buf_t *o, unsigned v)
{
    if (b_reserve(o, 1) == 0) o->b[o->len++] = (unsigned char)v;
}

static void b_u16(buf_t *o, unsigned v)
{
    if (b_reserve(o, 2) == 0) {
        o->b[o->len++] = (unsigned char)(v >> 8);
        o->b[o->len++] = (unsigned char)v;
    }
}

static void b_u32(buf_t *o, unsigned v)
{
    if (b_reserve(o, 4) == 0) {
        o->b[o->len++] = (unsigned char)(v >> 24);
        o->b[o->len++] = (unsigned char)(v >> 16);
        o->b[o->len++] = (unsigned char)(v >> 8);
        o->b[o->len++] = (unsigned char)v;
    }
}

static void b_raw(buf_t *o, const void *p, size_t n)
{
    if (n && b_reserve(o, n) == 0) {
        memcpy(o->b + o->len, p, n);
        o->len += n;
    }
}

/* String attribute: value tag + name + value */
static void b_str_attr(buf_t *o, unsigned tag, const char *name, const char *val)
{
    size_t nl = strlen(name), vl = strlen(val);
    b_u8(o, tag);
    b_u16(o, (unsigned)nl);
    b_raw(o, name, nl);
    b_u16(o, (unsigned)vl);
    b_raw(o, val, vl);
}

/* Integer/enum attribute: 4-byte big-endian */
static void b_int_attr(buf_t *o, unsigned tag, const char *name, int v)
{
    size_t nl = strlen(name);
    b_u8(o, tag);
    b_u16(o, (unsigned)nl);
    b_raw(o, name, nl);
    b_u16(o, 4);
    b_u32(o, (unsigned)v);
}

/* Boolean attribute */
static void b_bool_attr(buf_t *o, const char *name, int v)
{
    size_t nl = strlen(name);
    b_u8(o, 0x22);
    b_u16(o, (unsigned)nl);
    b_raw(o, name, nl);
    b_u16(o, 1);
    b_u8(o, v ? 1 : 0);
}

/* ---------------- HTTP/1.1 transport ---------------- */

/* Timeout settings (see ipp_set_timeouts in ipp.h; P0: fail fast on dead endpoints).
 * Defaults: connect 3s (was 5s)/receive 20s; callers tighten for probing/cancel. */
static int g_to_connect_ms = 3000;
static int g_to_recv_ms    = 20000;

void ipp_set_timeouts(int connect_ms, int recv_ms)
{
    g_to_connect_ms = connect_ms > 0 ? connect_ms : 3000;
    g_to_recv_ms    = recv_ms    > 0 ? recv_ms    : 20000;
}

static int tcp_connect(const char *ip, int port, int timeout_ms)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
        set_detail("Invalid IPv4 address: %s", ip);
        return IPP_E_ARG;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { set_detail("socket: %s", strerror(errno)); return IPP_E_SOCKET; }

    /* Implement connection timeout with nonblocking connect + select */
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (r != 0 && errno != EINPROGRESS) {
        set_detail("connect %s:%d: %s", ip, port, strerror(errno));
        close(fd);
        return IPP_E_CONNECT;
    }
    if (r != 0) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(fd, &wf);
        struct timeval tv = { .tv_sec = timeout_ms / 1000,
                              .tv_usec = (timeout_ms % 1000) * 1000 };
        if (select(fd + 1, NULL, &wf, NULL, &tv) <= 0) {
            set_detail("connect %s:%d timed out (%dms)", ip, port, timeout_ms);
            close(fd);
            return IPP_E_CONNECT;
        }
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
        if (soerr != 0) {
            set_detail("connect %s:%d: %s", ip, port, strerror(soerr));
            close(fd);
            return IPP_E_CONNECT;
        }
    }
    fcntl(fd, F_SETFL, fl); /* Restore blocking mode */
    if (g_to_recv_ms > 0) {
        struct timeval rt = { .tv_sec = g_to_recv_ms / 1000,
                              .tv_usec = (g_to_recv_ms % 1000) * 1000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
    }
    struct timeval st = { .tv_sec = 60, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof(st));
    return fd;
}

static int send_all(int fd, const unsigned char *p, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, p + off, n - off, 0);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && (errno == EINTR)) continue;
        set_detail("send: %s", strerror(errno));
        return IPP_E_SEND;
    }
    return IPP_OK;
}

/* Decode chunked body (move in-place to out); return decoded length or -1 if malformed/truncated */
static int dechunk(const unsigned char *p, int n, unsigned char *out, int outcap)
{
    int ip = 0, op = 0;
    while (ip < n) {
        int v = 0, digits = 0;
        while (ip < n && p[ip] != '\r' && p[ip] != '\n' && p[ip] != ';') {
            int c = p[ip];
            int d = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) return -1;
            v = v * 16 + d;
            digits++;
            ip++;
        }
        if (!digits) return -1;
        while (ip < n && p[ip] != '\r' && p[ip] != '\n') ip++; /* Skip chunk extensions */
        while (ip < n && (p[ip] == '\r' || p[ip] == '\n')) ip++;
        if (v == 0) return op;               /* Terminal chunk */
        if (ip + v > n || op + v > outcap) return -1;
        memmove(out + op, p + ip, (size_t)v); /* In-place decode: dst<=src, so memmove is safe */
        op += v;
        ip += v;
        while (ip < n && (p[ip] == '\r' || p[ip] == '\n')) ip++;
    }
    return -1; /* No zero-size chunk = truncated */
}

/* Case-insensitive prefix match */
static int istart(const char *line, const char *pre)
{
    size_t n = strlen(pre);
    return strncasecmp(line, pre, n) == 0;
}

/* ---- Endpoint path override (mDNS TXT rp=, etc.) ---- */
static char g_path[96] = IPP_DEF_PATH;

void ipp_set_path(const char *path)
{
    if (!path || !path[0]) { g_path[0] = 0; strcpy(g_path, IPP_DEF_PATH); return; }
    /* Ensure leading slash (add it for rp=ipp/print form) */
    char tmp[96];
    if (path[0] == '/') snprintf(tmp, sizeof(tmp), "%.95s", path);
    else snprintf(tmp, sizeof(tmp), "/%.94s", path);
    strcpy(g_path, tmp);
}

const char *ipp_get_path(void) { return g_path; }

/* POST IPP request → resp buffer (IPP body only). Return IPP_OK or error code */
static int http_post_ipp(const char *ip, int port,
                         const unsigned char *body, size_t blen, buf_t *resp)
{
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "POST %s HTTP/1.1\r\n"
                        "Host: %s:%d\r\n"
                        "Content-Type: application/ipp\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        g_path, ip, port, blen);
    if (hlen <= 0 || (size_t)hlen >= sizeof(hdr)) return IPP_E_ARG;

    int fd = tcp_connect(ip, port, g_to_connect_ms);
    if (fd < 0) return fd;

    int e = send_all(fd, (const unsigned char *)hdr, (size_t)hlen);
    if (e == IPP_OK) e = send_all(fd, body, blen);
    if (e != IPP_OK) { close(fd); return e; }
    LOGI("Sent POST %s (IPP body %zu bytes); waiting for response", g_path, blen);

    /* Read until EOF (Connection: close) or Content-Length/chunked termination */
    resp->len = 0;
    for (;;) {
        if (b_reserve(resp, 16384) != 0) { close(fd); return IPP_E_NOMEM; }
        ssize_t r = recv(fd, resp->b + resp->len, resp->cap - resp->len, 0);
        if (r > 0) {
            resp->len += (size_t)r;
            if (resp->len > 1024 * 1024) { /* Limit protection */
                set_detail("HTTP response exceeds 1MB");
                close(fd);
                return IPP_E_HTTP;
            }
            continue;
        }
        if (r == 0) break;                       /* Server closed = complete */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            set_detail("Timed out reading response (%zu bytes received)", resp->len);
            close(fd);
            return IPP_E_TIMEOUT;
        }
        break; /* For other errors, try parsing the bytes received so far */
    }
    close(fd);

    if (resp->len == 0) { set_detail("Empty HTTP response"); return IPP_E_TRUNC; }
    resp->b[resp->len] = 0; /* For strstr-style text search (buffer has extra space) */

    /* ---- Parse HTTP headers ---- */
    unsigned char *raw = resp->b;
    size_t rlen = resp->len;
    char *hend = strstr((char *)raw, "\r\n\r\n");
    size_t hlen2 = hend ? (size_t)(hend + 4 - (char *)raw) : 0;
    if (!hend) { /* Accept \n\n as well */
        hend = strstr((char *)raw, "\n\n");
        hlen2 = hend ? (size_t)(hend + 2 - (char *)raw) : 0;
    }
    if (!hend) { set_detail("Incomplete HTTP headers"); return IPP_E_HTTP; }

    int code = 0;
    char line[256];
    size_t pos = 0;
    while (pos < hlen2) {
        size_t le = pos;
        while (le < hlen2 && raw[le] != '\n') le++;
        size_t ll = le - pos;
        if (ll && raw[pos + ll - 1] == '\r') ll--;
        if (ll >= sizeof(line)) ll = sizeof(line) - 1;
        memcpy(line, raw + pos, ll);
        line[ll] = '\0';
        if (istart(line, "HTTP/")) {
            code = atoi(line + 9);
        }
        pos = le + 1;
    }
    if (code != 200) {
        set_detail("HTTP status %d (expected 200)", code);
        return IPP_E_HTTP;
    }

    /* Header fields: find Content-Length / Transfer-Encoding */
    long clen = -1;
    int chunked = 0;
    char *hp = (char *)raw;
    while ((hp = strstr(hp, "\r\n")) != NULL && (size_t)(hp - (char *)raw) < hlen2) {
        hp += 2;
        if (istart(hp, "Content-Length:")) clen = atol(hp + 15);
        else if (istart(hp, "Transfer-Encoding:") && strstr(hp, "chunked")) chunked = 1;
    }

    unsigned char *bstart = raw + hlen2;
    size_t bavail = rlen - hlen2;
    if (chunked) {
        int dn = dechunk(bstart, (int)bavail, bstart, (int)bavail);
        if (dn < 0) { set_detail("Chunked decode failed/truncated"); return IPP_E_HTTP; }
        resp->len = (size_t)dn;
    } else {
        if (clen >= 0 && (size_t)clen <= bavail) resp->len = (size_t)clen;
        else resp->len = bavail; /* No length and stream still open: best effort */
    }
    memmove(resp->b, bstart, resp->len); /* Move IPP body to buffer start (overwrite HTTP headers) */
    if (resp->len < 8) { set_detail("IPP response body too short (%zu)", resp->len); return IPP_E_TRUNC; }
    LOGI("HTTP 200, IPP response body %zu bytes", resp->len);
    return IPP_OK;
}

/* ---------------- Request construction ---------------- */
static unsigned g_reqid = 0x2100; /* Request ID base, avoid 0 */
static char g_printer_uri[192];   /* Target printer URI; ipp_txn fills using actual ip/port */

static void put_common_op_attrs(buf_t *b, const char *user)
{
    b_u8(b, 0x01); /* operation-attributes-tag */
    b_str_attr(b, 0x47, "attributes-charset", "utf-8");
    b_str_attr(b, 0x48, "attributes-natural-language", "en");
    /* printer-uri is the required operation target per RFC 8011; camera testing showed that
       omitting it returns 0x0400. It must follow natural-language and precede requesting-user-name. */
    b_str_attr(b, 0x45, "printer-uri", g_printer_uri);
    b_str_attr(b, 0x42, "requesting-user-name", user ? user : "x2print");
}

static size_t build_get_printer_attrs(buf_t *b, unsigned ver)
{
    b->len = 0;
    b_u16(b, ver);
    b_u16(b, 0x000B); /* Get-Printer-Attributes */
    b_u32(b, ++g_reqid);
    put_common_op_attrs(b, "x2print");
    /* Single value "all": 1S firmware handles multi-valued requested-attributes incorrectly
     * (returns only the last item, omitting media-supported/formats); query media-col separately
     * when needed (see ipp_get_printer_attributes). */
    b_str_attr(b, 0x44, "requested-attributes", "all");
    b_u8(b, 0x03); /* end-of-attributes */
    return b->len;
}

/* Second-stage probe: explicitly request media-col-database (some IPP stacks, e.g. CUPS,
 * do not include it in "all"). */
static size_t build_get_printer_attrs_mcol(buf_t *b, unsigned ver)
{
    b->len = 0;
    b_u16(b, ver);
    b_u16(b, 0x000B);
    b_u32(b, ++g_reqid);
    put_common_op_attrs(b, "x2print");
    b_str_attr(b, 0x44, "requested-attributes", "media-col-database");
    b_u8(b, 0x03);
    return b->len;
}

static size_t build_print_job(buf_t *b, unsigned ver, const unsigned char *jpeg,
                              size_t jlen, const ipp_print_opts_t *o)
{
    b->len = 0;
    b_u16(b, ver);
    b_u16(b, 0x0002); /* Print-Job */
    b_u32(b, ++g_reqid);
    put_common_op_attrs(b, "x2print");
    b_str_attr(b, 0x42, "job-name", (o->job_name && o->job_name[0]) ? o->job_name : "x2print");
    b_bool_attr(b, "ipp-attribute-fidelity", 0); /* Be permissive: unknown attributes are not errors */
    b_str_attr(b, 0x49, "document-format",
               (o->document_format && o->document_format[0]) ? o->document_format
                                                             : "image/jpeg");
    /* Job Template attributes (job group) */
    b_u8(b, 0x02); /* job-attributes-tag */
    if (o->copies > 0) b_int_attr(b, 0x21, "copies", o->copies);
    if (o->paper && o->paper[0]) b_str_attr(b, 0x44, "media", o->paper);
    if (o->color_mode && o->color_mode[0])
        b_str_attr(b, 0x44, "print-color-mode", o->color_mode);
    if (o->quality > 0) b_int_attr(b, 0x23, "print-quality", o->quality);
    if (o->print_scaling && o->print_scaling[0])
        b_str_attr(b, 0x44, "print-scaling", o->print_scaling);
    b_u8(b, 0x03);
    b_raw(b, jpeg, jlen); /* Data follows immediately */
    return b->len;
}

static size_t build_get_job_attrs(buf_t *b, unsigned ver, int job_id)
{
    b->len = 0;
    b_u16(b, ver);
    b_u16(b, 0x0009); /* Get-Job-Attributes */
    b_u32(b, ++g_reqid);
    put_common_op_attrs(b, "x2print");
    b_int_attr(b, 0x21, "job-id", job_id); /* Operation attributes group */
    b_u8(b, 0x03);
    return b->len;
}

static size_t build_cancel_job(buf_t *b, unsigned ver, int job_id)
{
    b->len = 0;
    b_u16(b, ver);
    b_u16(b, 0x0008); /* Cancel-Job */
    b_u32(b, ++g_reqid);
    put_common_op_attrs(b, "x2print");
    b_int_attr(b, 0x21, "job-id", job_id); /* Operation target: printer-uri + job-id */
    b_u8(b, 0x03);
    return b->len;
}

/* ---------------- Response parsing ---------------- */

static int be32(const unsigned char *p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

/* Parse IPP response with one callback per value; status_out receives status-code. Return 0/-1.
 * Additional values (name length=0) reuse the previous attribute name; collection members
 * are flattened under the name from memberAttrName. */
static int ipp_parse(const unsigned char *p, size_t n, ipp_attr_cb cb, void *ud,
                     unsigned *status_out)
{
    if (n < 8) return -1;
    unsigned ver = ((unsigned)p[0] << 8) | p[1];
    unsigned st  = ((unsigned)p[2] << 8) | p[3];
    *status_out = st;
    LOGD("IPP response version=%u.%u status=0x%04X", ver >> 8, ver & 0xFF, st);

    size_t pos = 8;
    char cur_name[128] = {0};
    char coll_root[128] = {0}; /* Collection root attribute name (for media-col-* detection) */
    int group = 0;
    int depth = 0; /* Collection nesting depth */

    while (pos < n) {
        unsigned tag = p[pos];
        if (tag <= 0x0F) { /* Delimiter tag */
            pos++;
            if (tag == 0x03) break;               /* end-of-attributes */
            group = (int)tag;
            depth = 0;
            cur_name[0] = '\0';
            coll_root[0] = '\0';
            continue;
        }
        if (tag == 0x37) { /* endCollection: record also has name-length/name/value-length/value fields */
            pos++; /* Value-tag byte */
            if (depth > 0 && --depth == 0 && strstr(coll_root, "media-col") && cb) {
                /* Top-level collection end: synthesize "media-size" event for fallback size
                 * extraction. Keep coll_root: each appended collection for the same property
                 * (media-col-database has multiple media-col collections) needs its own end event. */
                cb(ud, group, 0x44, "media-size", (const unsigned char *)"", 0);
            }
            if (pos + 4 > n) return -1;
            unsigned nl2 = ((unsigned)p[pos] << 8) | p[pos + 1];
            pos += 2;
            if (pos + (size_t)nl2 + 2 > n) return -1;
            unsigned vl2 = ((unsigned)p[pos + nl2] << 8) | p[pos + nl2 + 1];
            pos += nl2 + 2 + vl2;
            cur_name[0] = '\0';
            continue;
        }
        if (pos + 5 > n) return -1;
        unsigned nl = ((unsigned)p[pos + 1] << 8) | p[pos + 2];
        if (pos + 3 + nl + 2 > n) return -1;
        const char *an = (const char *)(p + pos + 3);
        unsigned vl = ((unsigned)p[pos + 3 + nl] << 8) | p[pos + 4 + nl];
        size_t vpos = pos + 5 + nl;
        if (vpos + vl > n) return -1;
        const unsigned char *val = p + vpos;

        if (nl > 0) { /* Named value: update current attribute name */
            size_t c = nl < sizeof(cur_name) - 1 ? nl : sizeof(cur_name) - 1;
            memcpy(cur_name, an, c);
            cur_name[c] = '\0';
        }

        if (tag == 0x34) { /* beginCollection: enter collection; its own value is empty */
            if (depth == 0 && cur_name[0])              /* Top-level collection: remember root name
                (for appended value nl=0, cur_name still holds root; overwrite so each media-col closes) */
                snprintf(coll_root, sizeof(coll_root), "%.127s", cur_name);
            depth++;
            pos = vpos + vl;
            continue;
        }
        if (tag == 0x4A) { /* memberAttrName: value is member name; following values belong to it */
            size_t c = vl < sizeof(cur_name) - 1 ? vl : sizeof(cur_name) - 1;
            memcpy(cur_name, val, c);
            cur_name[c] = '\0';
            pos = vpos + vl;
            continue;
        }
        /* Ordinary value (including collection-member leaves); unknown tags are also
         * delivered/skipped according to length */
        if (cb) cb(ud, group, (int)tag, cur_name, val, (int)vl);
        pos = vpos + vl;
    }
    return 0;
}

/* ---------------- Capability/job extraction ---------------- */

static void caps_add_str(char arr[][48], int *cnt, int max, const char *v)
{
    if (*cnt >= max) return;
    for (int i = 0; i < *cnt; i++)
        if (strcmp(arr[i], v) == 0) return; /* Deduplicate */
    snprintf(arr[*cnt], 48, "%.47s", v);
    (*cnt)++;
}

/* Add/find media entry (deduplicate by name); return index. Set native for native keyword;
 * update dimensions when dims>0. */
static int caps_add_media(ipp_caps_t *c, const char *v, int native, int x_um, int y_um)
{
    for (int i = 0; i < c->n_media; i++) {
        if (strcmp(c->media_supported[i], v) == 0) {
            if (native) c->media_native[i] = 1;
            if (x_um > 0 && !c->media_x_um[i]) { c->media_x_um[i] = x_um; c->media_y_um[i] = y_um; }
            return i;
        }
    }
    if (c->n_media >= IPP_MAX_MEDIA) return -1;
    snprintf(c->media_supported[c->n_media], 64, "%s", v);
    c->media_native[c->n_media] = native;
    c->media_x_um[c->n_media] = x_um;
    c->media_y_um[c->n_media] = y_um;
    return c->n_media++;
}

/* Convert media-size x/y-dimension (1/100mm) to friendly media name; return entry index */
static int media_from_dims(ipp_caps_t *c, int x100, int y100)
{
    static const struct { int x, y; const char *n; } tbl[] = {
        { 8890,  12700, "3.5x5in" },  {10160, 15240, "4x6in(6in)" },
        {10160, 20320, "4x8in" },     {12700, 17780, "5x7in" },
        {10500, 14850, "A6" },        {14850, 21000, "A5" },
        {21000, 29700, "A4" },        { 6858, 12192, "2.7x4.8in(3in)" },
        {21590, 27940, "letter" },    {12700, 12700, "5x5in" },
    };
    int lo = x100 < y100 ? x100 : y100, hi = x100 < y100 ? y100 : x100;
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        int tlo = tbl[i].x < tbl[i].y ? tbl[i].x : tbl[i].y;
        int thi = tbl[i].x < tbl[i].y ? tbl[i].y : tbl[i].x;
        if (abs(lo - tlo) <= tlo / 64 && abs(hi - thi) <= thi / 64) { /* ~1.5% tolerance */
            return caps_add_media(c, tbl[i].n, 0, x100 * 10, y100 * 10);
        }
    }
    char s[64];
    snprintf(s, sizeof(s), "%gx%gmm", x100 / 100.0, y100 / 100.0);
    return caps_add_media(c, s, 0, x100 * 10, y100 * 10);
}

static void caps_reasons(ipp_caps_t *c, const char *v)
{
    if (c->printer_state_reasons[0])
        strncat(c->printer_state_reasons, " ",
                sizeof(c->printer_state_reasons) - strlen(c->printer_state_reasons) - 1);
    strncat(c->printer_state_reasons, v,
            sizeof(c->printer_state_reasons) - strlen(c->printer_state_reasons) - 1);
}

/* Convert resolution value (9 bytes: x(4) y(4) units(1)) → dpi; units 3=dpi, 4=dpcm(×2.54) */
static void res_to_dpi(const unsigned char *val, int vlen, int *dx, int *dy)
{
    if (vlen < 9) return;
    int x = be32(val), y = be32(val + 4);
    if (val[8] == 4) { x = (x * 254 + 50) / 100; y = (y * 254 + 50) / 100; }
    else if (val[8] != 3) return;                 /* Unknown unit: ignore */
    if (x > 0 && y > 0) { *dx = x; *dy = y; }
}

void ipp_caps_feed(ipp_caps_t *c, int grp, int tag, const char *name,
                   const unsigned char *val, int vlen)
{
    (void)grp;
    if (tag == 0x21 || tag == 0x23) { /* integer / enum */
        int v = vlen >= 4 ? be32(val) : 0;
        if (!strcmp(name, "printer-state")) c->printer_state = v;
        else if (!strcmp(name, "print-quality-supported")) {
            if (c->n_quals < IPP_MAX_QUALS) c->qualities[c->n_quals++] = v;
        } else if (!strcmp(name, "print-quality-default")) c->quality_default = v;
        else if (!strcmp(name, "x-dimension")) c->_lx = v;   /* 1/100mm */
        else if (!strcmp(name, "y-dimension")) c->_ly = v;   /* 1/100mm */
        return;
    }
    if (tag == 0x33) { /* rangeOfInteger(8B) / resolution(9B) */
        if (!strcmp(name, "copies-supported") && vlen >= 8) {
            c->copies_min = be32(val);
            c->copies_max = be32(val + 4);
        } else if (!strcmp(name, "printer-resolution-default")) {
            res_to_dpi(val, vlen, &c->res_default_x, &c->res_default_y);
        }
        return;
    }
    /* Handle the rest as text values (length may include NUL; copy first) */
    char s[96];
    size_t cl = (size_t)vlen < sizeof(s) - 1 ? (size_t)vlen : sizeof(s) - 1;
    memcpy(s, val, cl);
    s[cl] = '\0';

    if (!strcmp(name, "document-format-supported")) caps_add_str(c->document_formats, &c->n_formats, IPP_MAX_FORMATS, s);
    else if (!strcmp(name, "print-color-mode-supported")) caps_add_str(c->print_color_modes, &c->n_modes, IPP_MAX_MODES, s);
    else if (!strcmp(name, "print-color-mode-default")) snprintf(c->color_mode_default, sizeof(c->color_mode_default), "%.47s", s);
    else if (!strcmp(name, "print-scaling-supported")) {
        if (c->n_scalings < IPP_MAX_SCAL) snprintf(c->print_scalings[c->n_scalings++], 32, "%.31s", s);
    } else if (!strcmp(name, "print-scaling-default")) snprintf(c->print_scaling_default, sizeof(c->print_scaling_default), "%.31s", s);
    else if (!strcmp(name, "media-supported") || !strcmp(name, "media-ready")) {
        if (tag == 0x44) caps_add_media(c, s, 1, 0, 0); /* Native-keyword media list */
    } else if (!strcmp(name, "media-size-name")) {
        /* Flatten media-col collection members: native keyword plus pending dimensions
         * (x/y-dimension may arrive first) */
        caps_add_media(c, s, 1, c->_lx * 10, c->_ly * 10);
        c->_szname = 1;
    } else if (!strcmp(name, "media-size")) {
        /* Synthetic event at collection end: if media-size-name is absent, use x/y dimensions
         * to synthesize a fallback media name (not native) */
        if (!c->_szname && c->_lx > 0 && c->_ly > 0) media_from_dims(c, c->_lx, c->_ly);
        c->_szname = 0;
        c->_lx = c->_ly = 0;
    } else if (!strcmp(name, "printer-state-reasons")) caps_reasons(c, s);
    else if (!strcmp(name, "printer-name")) snprintf(c->printer_name, sizeof(c->printer_name), "%.95s", s);
    else if (!strcmp(name, "printer-make-and-model")) snprintf(c->printer_make_and_model, sizeof(c->printer_make_and_model), "%.127s", s);
    else if (!strcmp(name, "media-default")) {
        snprintf(c->media_default, sizeof(c->media_default), "%.63s", s);
        /* Defensive: add default media if absent from supported list (still considered native/sendable) */
        if (c->media_default[0]) caps_add_media(c, c->media_default, 1, 0, 0);
    }
}

/* Parse dimensions encoded in PWG media keyword as fallback: convert trailing "_WxHin"/
 * "_WxHmm" to physical dimensions according to registry naming conventions (e.g.
 * na_index-4x6_4x6in → 101.6x152.4mm, iso_a4_210x297mm).
 * Used only when printer does not provide media-col physical dimensions; parse failure
 * returns -1 (not an error). */
static int kw_parse_dims(const char *kw, int *x_um, int *y_um)
{
    const char *last = strrchr(kw, '_');
    if (!last) last = kw; else last++;
    size_t len = strlen(last);
    if (len < 5) return -1;                       /* Shortest form "1x1in" */
    int mm = 0;
    if (!strcmp(last + len - 2, "in")) mm = 0;
    else if (!strcmp(last + len - 2, "mm")) mm = 1;
    else return -1;
    char buf[32];
    if (len - 2 >= sizeof(buf)) return -1;
    memcpy(buf, last, len - 2);
    buf[len - 2] = 0;
    char *xend = strchr(buf, 'x');
    if (!xend) return -1;
    *xend = 0;
    double w = atof(buf), h = atof(xend + 1);
    if (w <= 0 || h <= 0) return -1;
    long long xu = mm ? (long long)(w * 1000 + 0.5) : (long long)(w * 25400 + 0.5);
    long long yu = mm ? (long long)(h * 1000 + 0.5) : (long long)(h * 25400 + 0.5);
    if (xu < 10000 || xu > 2000000 || yu < 10000 || yu > 2000000) return -1; /* Valid range 1cm..2m */
    *x_um = (int)xu;
    *y_um = (int)yu;
    return 0;
}

/* Finalize probe: derive maximum JPEG pixels = largest media size × default resolution
 * (fallback 300dpi if unavailable) */
void ipp_caps_finish(ipp_caps_t *caps)
{
    /* Fallback: if native keyword has no dimensions (some 1S printers provide only
     * media-col-default/scattered sizes), derive from keyword naming convention; entries
     * with media-col dimensions are unchanged. */
    for (int i = 0; i < caps->n_media; i++) {
        if (caps->media_native[i] && caps->media_x_um[i] <= 0)
            kw_parse_dims(caps->media_supported[i], &caps->media_x_um[i], &caps->media_y_um[i]);
    }
    int dx = caps->res_default_x > 0 ? caps->res_default_x : 300;
    int dy = caps->res_default_y > 0 ? caps->res_default_y : 300;
    caps->jpeg_max_w = caps->jpeg_max_h = 0;
    for (int i = 0; i < caps->n_media; i++) {
        if (caps->media_x_um[i] <= 0 || caps->media_y_um[i] <= 0) continue;
        int w = (int)(((long long)caps->media_x_um[i] * dx + 12700) / 25400);  /* Round to nearest */
        int h = (int)(((long long)caps->media_y_um[i] * dy + 12700) / 25400);
        if (w > caps->jpeg_max_w) caps->jpeg_max_w = w;
        if (h > caps->jpeg_max_h) caps->jpeg_max_h = h;
    }
}

/* Native media keyword → target pixels (µm × default resolution; fallback 300dpi) */
int ipp_media_target_px(const ipp_caps_t *caps, const char *media, int *w, int *h)
{
    if (!caps || !media || !media[0] || !w || !h) return -1;
    for (int i = 0; i < caps->n_media; i++) {
        if (strcmp(caps->media_supported[i], media) != 0) continue;
        if (!caps->media_native[i]) return -1;         /* Synthesized name cannot be sent */
        if (caps->media_x_um[i] <= 0 || caps->media_y_um[i] <= 0) return -2;
        int dx = caps->res_default_x > 0 ? caps->res_default_x : 300;
        int dy = caps->res_default_y > 0 ? caps->res_default_y : 300;
        *w = (int)(((long long)caps->media_x_um[i] * dx + 12700) / 25400);   /* Round to nearest */
        *h = (int)(((long long)caps->media_y_um[i] * dy + 12700) / 25400);
        return 0;
    }
    return -1;
}

/* ipp_txn callback adapter: pass cb arguments to ipp_caps_feed */
typedef struct {
    ipp_caps_t *c;
} caps_ctx;

static void caps_cb(void *ud, int grp, int tag, const char *name,
                    const unsigned char *val, int vlen)
{
    ipp_caps_feed(((caps_ctx *)ud)->c, grp, tag, name, val, vlen);
}

/* ---------------- Version negotiation + transaction ---------------- */

typedef size_t (*build_fn)(buf_t *b, unsigned ver);

/* Execute one complete transaction; automatically retry with lower version on 0x0503 */
static int ipp_txn(const char *ip, int port, build_fn build, ipp_attr_cb cb, void *ud)
{
    if (port <= 0) port = IPP_DEF_PORT;
    snprintf(g_printer_uri, sizeof g_printer_uri, "ipp://%s:%u%s",
             ip, (unsigned)port, g_path);
    static unsigned ver = 0x0200; /* Default IPP/2.0 */
    buf_t req = {0}, resp = {0};
    int e = IPP_E_NOMEM;

    for (int attempt = 0; attempt < 2; attempt++) {
        req.len = 0;
        if (build(&req, ver) == 0 || req.oom) goto out;
        e = http_post_ipp(ip, port, req.b, req.len, &resp);
        if (e != IPP_OK) goto out;

        unsigned st = 0;
        if (ipp_parse(resp.b, resp.len, cb, ud, &st) != 0) {
            e = IPP_E_PARSE;
            set_detail("Failed to parse IPP response");
            goto out;
        }
        if (st == 0x0503 && ver != 0x0110) { /* Version unsupported → retry with 1.1 */
            LOGW("Printer does not support IPP/2.0 (status 0x0503); retrying with 1.1");
            ver = 0x0110;
            resp.len = 0;
            continue;
        }
        if (st >= 0x0100) { /* 4xx/5xx status */
            g_status = st;
            set_detail("IPP status-code 0x%04X", st);
            e = IPP_E_STATUS;
        }
        goto out; /* Success or terminal state */
    }
out:
    free(req.b);
    free(resp.b);
    return e;
}

/* ---------------- Public API ---------------- */

int ipp_get_printer_attributes_raw(const char *ip, int port, ipp_attr_cb cb, void *ud)
{
    if (!ip || !cb) return IPP_E_ARG;
    return ipp_txn(ip, port, build_get_printer_attrs, cb, ud);
}

int ipp_get_printer_attributes(const char *ip, int port, ipp_caps_t *caps)
{
    if (!ip || !caps) return IPP_E_ARG;
    memset(caps, 0, sizeof(*caps));
    caps_ctx ctx = { caps };
    int e = ipp_txn(ip, port, build_get_printer_attrs, caps_cb, &ctx);
    if (e == IPP_OK) {
        /* Second-stage probe: if "all" response has no physical media dimensions, explicitly
         * request media-col-database and feed it again (CUPS etc. omit media-col from "all").
         * Skip if first response already contained it (some 1S printers return collections
         * only for the explicit request). */
        int has_dims = 0;
        for (int i = 0; i < caps->n_media; i++)
            if (caps->media_x_um[i] > 0) { has_dims = 1; break; }
        if (!has_dims)
            e = ipp_txn(ip, port, build_get_printer_attrs_mcol, caps_cb, &ctx);
    }
    ipp_caps_finish(caps);                       /* Finalize derived fields (harmless on failure) */
    return e;
}

/* Extract Print-Job response */
typedef struct {
    int job_id;
    int job_state;
    char reasons[128];
    char job_uri[192];
} job_ctx;

static void job_cb(void *ud, int grp, int tag, const char *name,
                   const unsigned char *val, int vlen)
{
    (void)grp;
    job_ctx *j = (job_ctx *)ud;
    if (tag == 0x21 && !strcmp(name, "job-id")) j->job_id = vlen >= 4 ? be32(val) : 0;
    else if (tag == 0x23 && !strcmp(name, "job-state")) j->job_state = vlen >= 4 ? be32(val) : 0;
    else if (!strcmp(name, "job-state-reasons") && vlen > 0 && vlen < 96) {
        char s[96];
        memcpy(s, val, (size_t)vlen);
        s[vlen] = '\0';
        if (!j->reasons[0]) snprintf(j->reasons, sizeof(j->reasons), "%s", s);
        else {
            strncat(j->reasons, " ", sizeof(j->reasons) - strlen(j->reasons) - 1);
            strncat(j->reasons, s, sizeof(j->reasons) - strlen(j->reasons) - 1);
        }
    } else if (!strcmp(name, "job-uri") && vlen > 0 && vlen < 96) {
        memcpy(j->job_uri, val, (size_t)vlen);
        j->job_uri[vlen] = '\0';
    }
}

/* Fallback: trailing number in job-uri, e.g. ipp://host/ipp/print/job/123 */
static int jobid_from_uri(const char *uri)
{
    const char *p = strrchr(uri, '/');
    return p ? atoi(p + 1) : 0;
}

int ipp_print_jpeg(const char *ip, int port, const unsigned char *jpeg, size_t len,
                   const ipp_print_opts_t *opts, int *job_id)
{
    if (!ip || !jpeg || len == 0 || !job_id) return IPP_E_ARG;
    ipp_print_opts_t d;
    if (opts) d = *opts;
    else memset(&d, 0, sizeof(d));

    job_ctx jc;
    memset(&jc, 0, sizeof(jc));

    /* Inline one transaction (Print-Job builder takes JPEG argument, not convenient for function pointer) */
    if (port <= 0) port = IPP_DEF_PORT;
    static unsigned ver = 0x0200;
    buf_t req = {0}, resp = {0};
    int e = IPP_E_NOMEM;
    for (int attempt = 0; attempt < 2; attempt++) {
        req.len = 0;
        if (build_print_job(&req, ver, jpeg, len, &d) == 0 || req.oom) goto out;
        e = http_post_ipp(ip, port, req.b, req.len, &resp);
        if (e != IPP_OK) goto out;
        unsigned st = 0;
        if (ipp_parse(resp.b, resp.len, job_cb, &jc, &st) != 0) {
            e = IPP_E_PARSE;
            set_detail("Failed to parse IPP response");
            goto out;
        }
        if (st == 0x0503 && ver != 0x0110) {
            LOGW("Printer does not support IPP/2.0 (status 0x0503); retrying with 1.1");
            ver = 0x0110;
            resp.len = 0;
            continue;
        }
        if (st >= 0x0100) {
            g_status = st;
            set_detail("IPP status-code 0x%04X", st);
            e = IPP_E_STATUS;
        }
        goto out;
    }
out:
    free(req.b);
    free(resp.b);
    if (e == IPP_OK) {
        *job_id = jc.job_id ? jc.job_id : jobid_from_uri(jc.job_uri);
        LOGI("Print-Job complete: job_id=%d state=%s", *job_id,
             ipp_job_state_name(jc.job_state));
        if (!*job_id) { e = IPP_E_PARSE; set_detail("Response is missing job-id/job-uri"); }
    }
    return e;
}

int ipp_get_job_attributes(const char *ip, int port, int job_id, ipp_job_t *job)
{
    if (!ip || !job || job_id <= 0) return IPP_E_ARG;
    memset(job, 0, sizeof(*job));
    job->job_id = job_id;

    /* Get-Job-Attributes job-id uses operation-attributes group; inline because transaction
     * wrapper does not conveniently accept it */
    if (port <= 0) port = IPP_DEF_PORT;
    static unsigned ver = 0x0200;
    buf_t req = {0}, resp = {0};
    int e = IPP_E_NOMEM;
    job_ctx jc;
    memset(&jc, 0, sizeof(jc));
    for (int attempt = 0; attempt < 2; attempt++) {
        req.len = 0;
        if (build_get_job_attrs(&req, ver, job_id) == 0 || req.oom) goto out;
        e = http_post_ipp(ip, port, req.b, req.len, &resp);
        if (e != IPP_OK) goto out;
        unsigned st = 0;
        if (ipp_parse(resp.b, resp.len, job_cb, &jc, &st) != 0) {
            e = IPP_E_PARSE;
            set_detail("Failed to parse IPP response");
            goto out;
        }
        if (st == 0x0503 && ver != 0x0110) {
            ver = 0x0110;
            resp.len = 0;
            continue;
        }
        if (st >= 0x0100) {
            g_status = st;
            set_detail("IPP status-code 0x%04X", st);
            e = IPP_E_STATUS;
        }
        goto out;
    }
out:
    free(req.b);
    free(resp.b);
    if (e == IPP_OK) {
        job->job_state = jc.job_state;
        snprintf(job->state_reasons, sizeof(job->state_reasons), "%s", jc.reasons);
    }
    return e;
}

int ipp_cancel_job(const char *ip, int port, int job_id)
{
    if (!ip || job_id <= 0) return IPP_E_ARG;

    /* Cancel-Job builder takes job_id; inline transaction as for Get-Job-Attributes */
    if (port <= 0) port = IPP_DEF_PORT;
    static unsigned ver = 0x0200;
    buf_t req = {0}, resp = {0};
    int e = IPP_E_NOMEM;
    for (int attempt = 0; attempt < 2; attempt++) {
        req.len = 0;
        if (build_cancel_job(&req, ver, job_id) == 0 || req.oom) goto out;
        e = http_post_ipp(ip, port, req.b, req.len, &resp);
        if (e != IPP_OK) goto out;
        unsigned st = 0;
        if (ipp_parse(resp.b, resp.len, NULL, NULL, &st) != 0) {
            e = IPP_E_PARSE;
            set_detail("Failed to parse IPP response");
            goto out;
        }
        if (st == 0x0503 && ver != 0x0110) {
            ver = 0x0110;
            resp.len = 0;
            continue;
        }
        if (st >= 0x0100) {
            g_status = st;
            set_detail("IPP status-code 0x%04X", st);
            e = IPP_E_STATUS;
        } else {
            LOGI("Cancel-Job job=%d complete, status=0x%04X", job_id, st);
        }
        goto out;
    }
out:
    free(req.b);
    free(resp.b);
    return e;
}

/* ---------------- Display helpers ---------------- */

const char *ipp_group_name(int g)
{
    switch (g) {
    case 0x01: return "operation attributes";
    case 0x02: return "job attributes";
    case 0x03: return "end";
    case 0x04: return "printer attributes";
    case 0x05: return "unsupported attributes";
    default:   return "unknown group";
    }
}

const char *ipp_tag_name(int tag)
{
    switch (tag) {
    case 0x21: return "integer";
    case 0x22: return "boolean";
    case 0x23: return "enum";
    case 0x31: return "octetString";
    case 0x32: return "dateTime";
    case 0x33: return "resolution";
    case 0x34: return "beginCollection";
    case 0x35: return "textWithLanguage";
    case 0x36: return "nameWithLanguage";
    case 0x37: return "endCollection";
    case 0x41: return "text";
    case 0x42: return "name";
    case 0x44: return "keyword";
    case 0x45: return "uri";
    case 0x46: return "uriScheme";
    case 0x47: return "charset";
    case 0x48: return "naturalLanguage";
    case 0x49: return "mimeMediaType";
    case 0x4A: return "memberAttrName";
    default:   return "unknown";
    }
}

const char *ipp_printer_state_name(int s)
{
    switch (s) {
    case 3: return "idle(3)";
    case 4: return "processing(4)";
    case 5: return "stopped(5)";
    default: return "unknown";
    }
}

const char *ipp_job_state_name(int s)
{
    switch (s) {
    case 3: return "pending(3)";
    case 4: return "pending-held(4)";
    case 5: return "processing(5)";
    case 6: return "processing-stopped(6)";
    case 7: return "canceled(7)";
    case 8: return "aborted(8)";
    case 9: return "completed(9)";
    default: return "unknown";
    }
}
