/* config.c — Minimal INI parser/writer
 * Key table (INTERFACE.md §5 v2): ssid s | copies i | media s | print_quality s
 *                          | color_mode s | preset i
 * Parsing rules: ;/# comments, ignore [section] contents, split key=value at first '=',
 * trim whitespace, accept CRLF.
 */
#include "config.h"
#include "state.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* ---- Service-level extension keys (phase 2+): not D-Bus properties; loaded and saved ----
 * printer_ip      : Cache of known printer endpoints (comma-separated host[:port]; updated
 *                   with discovered endpoints; nonempty value gets priority in
 *                   Start_discovery's direct-connection fast path)
 * ipp_port        : IPP port (default 631)
 * photo_root      : Default Get_photo_list directory (empty = camera default /mnt/media_runtime)
 * wifi_backend    : Connect_to_ap execution layer: "mock" (integration testing, no Wi-Fi) /
 *                   "cam" (camera orchestration)
 * idle_restore_min: Idle watchdog (minutes): after this interval with no task or business
 *                   call while joined/ready, automatically restore baseline Wi-Fi and
 *                   return to disconnected; 0=disabled; default=5
 */
typedef struct {
    const char *key;
    char        val[128];
} prn_extra_t;
static prn_extra_t g_extra[] = {
    { "printer_ip",       "" },
    { "ipp_port",         "" },
    { "photo_root",       "" },
    { "wifi_backend",     "" },
    { "idle_restore_min", "" },
};

static char g_cfg_path[300];              /* Remembered by load for atomic save in set */

const char *prn_config_get(const char *key)
{
    if (!key) return "";
    for (size_t i = 0; i < sizeof g_extra / sizeof g_extra[0]; i++)
        if (!strcmp(g_extra[i].key, key)) return g_extra[i].val;
    return "";
}

/* Update a service-level extension key and save (memory-only if path is unknown);
 * 0=ok, -1=unknown key/I/O error. */
int prn_config_set(const char *key, const char *val)
{
    if (!key || !val) return -1;
    for (size_t i = 0; i < sizeof g_extra / sizeof g_extra[0]; i++) {
        if (strcmp(g_extra[i].key, key)) continue;
        snprintf(g_extra[i].val, sizeof g_extra[i].val, "%s", val);
        if (g_cfg_path[0]) return prn_config_save(g_cfg_path);
        return 0;
    }
    return -1;
}

/* Capture extension key/value while loading; returns 1=recognized */
static int extra_load(const char *key, const char *val)
{
    for (size_t i = 0; i < sizeof g_extra / sizeof g_extra[0]; i++) {
        if (strcmp(g_extra[i].key, key)) continue;
        snprintf(g_extra[i].val, sizeof g_extra[i].val, "%s", val);
        return 1;
    }
    return 0;
}

static void extra_write(FILE *f)
{
    for (size_t i = 0; i < sizeof g_extra / sizeof g_extra[0]; i++)
        fprintf(f, "%s = %s\n", g_extra[i].key, g_extra[i].val);
}

/* Write default configuration (first launch) */
static int write_default(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) { LOGE("config: create %s failed", path); return -1; }
    fputs("; x2print printer-service configuration (auto-generated)\n[printer]\n"
          "ssid =\ncopies = 1\nmedia =\nprint_quality = normal\n"
          "color_mode = auto\npreset = 0\n", f);
    fputs("; Service-level extension keys (see NOTES.md §10/§12 / config.h)\n", f);
    fputs("printer_ip =\nipp_port =\nphoto_root =\nwifi_backend =\n"
          "idle_restore_min = 5\n", f);
    fclose(f);
    LOGI("config: default written to %s", path);
    return 0;
}

int prn_config_load(const char *path)
{
    snprintf(g_cfg_path, sizeof g_cfg_path, "%s", path);
    FILE *f = fopen(path, "r");
    if (!f) {
        LOGW("config: %s missing, creating defaults", path);
        return write_default(path);
    }
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') continue;                       /* Section names are informational only */
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *val = trim(eq + 1);
        if (extra_load(key, val)) { LOGI("config: extra key '%s' = '%s'", key, val); n++; continue; }
        int idx = prn_prop_index(key);
        const prn_prop_info *pi = prn_prop_info_get(idx);
        if (!pi || !pi->writable) { LOGW("config: ignore key '%s'", key); continue; }

        if (pi->type == 's') {                          /* ssid/media/print_quality/color_mode */
            if (prn_set_string(idx, val) != 0) LOGW("config: bad %s value, keep default", key);
        } else {
            char *end = NULL;
            long v = strtol(val, &end, 10);
            if (end == val || (end && *end)) { LOGW("config: '%s' bad int '%s'", key, val); continue; }
            if (prn_set_int(idx, (int)v) == -2) LOGW("config: '%s' out of range %ld", key, v);
        }
        n++;
    }
    fclose(f);
    LOGI("config: loaded %d key(s) from %s", n, path);
    return 0;
}

int prn_config_save(const char *path)
{
    prn_state *st = prn_state_get();
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { LOGE("config: save open %s failed", tmp); return -1; }
    fprintf(f, "; x2print printer-service configuration (auto-generated)\n[printer]\n"
               "ssid = %s\n"
               "copies = %d\n"
               "media = %s\n"
               "print_quality = %s\n"
               "color_mode = %s\n"
               "preset = %d\n",
            st->ssid, st->copies, st->media, st->print_quality,
            st->color_mode, st->preset);
    extra_write(f);                                   /* Write service-level extension keys back unchanged */
    if (fclose(f) != 0) { LOGE("config: close %s failed", tmp); return -1; }
    if (rename(tmp, path) != 0) {                     /* Atomic replacement */
        remove(path);
        if (rename(tmp, path) != 0) { LOGE("config: rename %s failed", path); return -1; }
    }
    LOGD("config: saved to %s", path);
    return 0;
}
